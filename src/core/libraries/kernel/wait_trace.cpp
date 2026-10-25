// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/wait_trace.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <set>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/libraries/fiber/fiber.h"
#include "core/libraries/kernel/guest_stack_dump.h"
#include "core/linker.h"
#include "core/libraries/kernel/threads/pthread.h"

namespace Libraries::Kernel {

namespace {
constexpr std::size_t RingSize = 32;

struct WaitEntry {
    const char* name;
    u64 guest_ra;
    u64 obj;
    u32 tid;
    u64 ms;
    char thread[20]{};
};

// One entry per guest thread: a deadlock snapshot needs the *current* wait of every thread, and
// a busy thread (the updater) would otherwise push the interesting entries out of a ring.
std::map<u32, WaitEntry> g_last_wait;
// The last few synchronisation calls of every thread: a blocked worker that took a lock right
// before parking shows up here even though only its final wait is recorded above.
constexpr std::size_t HistorySize = 8;
struct HistoryEntry {
    const char* name;
    u64 guest_ra;
    u64 ms;
};
struct History {
    std::array<HistoryEntry, HistorySize> entries{};
    u32 count{0};
};
std::map<u32, History> g_history;
std::mutex g_ring_mutex;
// SHADPS4_FREEZE_DUMP=<secs[,secs...]> is the one switch to reach for when the title hangs: it
// turns on the wait ring, the semaphore event ring and the mutex contention tracer, and writes a
// consolidated report so a single run is enough to see where every thread is stuck.
bool g_freeze_dump = std::getenv("SHADPS4_FREEZE_DUMP") != nullptr;
bool g_enabled = g_freeze_dump || std::getenv("SHADPS4_DUMP_WAITS_AT") != nullptr;

std::filesystem::path FreezeReportPath() {
    auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    path /= "freeze_report.txt";
    return path;
}

void WriteFreezeReport(const std::string& text) {
    if (!g_freeze_dump) {
        return;
    }
    std::error_code ec;
    std::ofstream out{FreezeReportPath(), std::ios::app};
    if (!out) {
        return;
    }
    out << text;
}
std::atomic<u32> g_tid_hint{0};
} // namespace

namespace {
struct SemaEvent {
    const char* kind;
    u32 handle;
    s32 count;
    u64 ms;
    char thread[20]{};
};
constexpr std::size_t SemaRingSize = 4096;
constexpr std::size_t MaxSemaHandles = 64;
std::array<std::atomic<u32>, MaxSemaHandles> g_sema_waits{};
std::array<std::atomic<u32>, MaxSemaHandles> g_sema_signals{};
std::map<u32, std::deque<SemaEvent>> g_sema_recent;
std::array<SemaEvent, SemaRingSize> g_sema_ring{};
std::atomic<u64> g_sema_index{0};
std::mutex g_sema_mutex;
} // namespace

namespace {
struct HeldMutex {
    u64 mutex;
    const char* name;
    u64 guest_ra;
};
std::map<u32, std::vector<HeldMutex>> g_held_mutexes;
std::mutex g_held_mutex;
} // namespace

namespace {
std::map<u64, u64> g_mutex_guest_addresses;
std::mutex g_mutex_guest_addresses_lock;
} // namespace

void RecordMutexGuestAddress(u64 object, u64 guest_address) {
    if (!g_enabled || object == 0 || guest_address == 0) {
        return;
    }
    std::scoped_lock lk{g_mutex_guest_addresses_lock};
    g_mutex_guest_addresses[object] = guest_address;
}

void RecordMutexAcquired(u32 tid, u64 mutex, const char* name, u64 guest_ra) {
    if (!g_enabled) {
        return;
    }
    if (name != nullptr && std::string_view{name} == "SceLibc_Mtx") {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 20000) {
            LOG_INFO(Kernel, "scemtx: acquire tid={} obj={:#x} ra={:#x}", tid, mutex, guest_ra);
        }
    }
    std::scoped_lock lk{g_held_mutex};
    g_held_mutexes[tid].push_back(HeldMutex{mutex, name, guest_ra});
}

namespace {
struct OwnerInfo {
    std::string name;
    std::string owner_name;
    u32 owner_tid;
    u64 waiter_ra;
    u32 hits;
};
std::map<u64, OwnerInfo> g_mutex_owners;
std::mutex g_mutex_owners_lock;
} // namespace

void RecordMutexOwner(u64 mutex, const char* name, u32 owner_tid, const char* owner_name,
                      u64 waiter_ra) {
    if (!g_enabled) {
        return;
    }
    std::scoped_lock lk{g_mutex_owners_lock};
    auto& info = g_mutex_owners[mutex];
    info.name = name != nullptr ? name : "?";
    info.owner_name = owner_name != nullptr ? owner_name : "?";
    info.owner_tid = owner_tid;
    info.waiter_ra = waiter_ra;
    info.hits++;
}

void RecordMutexReleased(u32 tid, u64 mutex) {
    if (!g_enabled) {
        return;
    }
    {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 20000) {
            LOG_INFO(Kernel, "scemtx: release tid={} obj={:#x}", tid, mutex);
        }
    }
    std::scoped_lock lk{g_held_mutex};
    auto it = g_held_mutexes.find(tid);
    if (it == g_held_mutexes.end()) {
        return;
    }
    auto& held = it->second;
    for (auto m = held.rbegin(); m != held.rend(); ++m) {
        if (m->mutex == mutex) {
            held.erase(std::next(m).base());
            break;
        }
    }
    if (held.empty()) {
        g_held_mutexes.erase(it);
    }
}

namespace {
struct SemaInfo {
    std::string name;
    s32 init_count;
    s32 max_count;
    u32 overflows;
};
std::map<u32, SemaInfo> g_sema_info;
std::mutex g_sema_info_lock;
} // namespace

namespace {
struct CondInfo {
    u64 mutex{0};
    u32 waiters{0};
    u64 waits{0};
    u64 signals{0};
    u64 last_wait_ms{0};
    u64 last_signal_ms{0};
};
std::map<u64, CondInfo> g_cond_info;
std::mutex g_cond_info_lock;

u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}
} // namespace

void RecordCondWait(u64 cond, u64 mutex) {
    if (!g_enabled) {
        return;
    }
    std::scoped_lock lk{g_cond_info_lock};
    auto& info = g_cond_info[cond];
    info.mutex = mutex;
    info.waiters++;
    info.waits++;
    info.last_wait_ms = NowMs();
}

void RecordCondWaitDone(u64 cond) {
    if (!g_enabled) {
        return;
    }
    std::scoped_lock lk{g_cond_info_lock};
    auto it = g_cond_info.find(cond);
    if (it != g_cond_info.end() && it->second.waiters > 0) {
        it->second.waiters--;
    }
}

void TraceCondSignal(u64 cond, const char* what) {
    static const bool on = std::getenv("SHADPS4_TRACE_COND_SIGNAL") != nullptr;
    if (!on) {
        return;
    }
    // one line per (cond, thread): a busy condvar would otherwise flood the log
    static std::mutex seen_lock;
    static std::set<std::pair<u64, std::string>> seen;
    const auto* current = g_curthread;
    const std::string thread = current != nullptr ? current->name : "?";
    {
        std::scoped_lock lk{seen_lock};
        if (!seen.emplace(cond, thread).second) {
            return;
        }
    }
    LOG_INFO(Kernel, "cond signal {:#x} ({}) by '{}' tid={}", cond, what, thread,
             current != nullptr ? current->tid.load() : 0);
}

void RecordCondSignal(u64 cond, bool broadcast) {
    if (!g_enabled) {
        return;
    }
    std::scoped_lock lk{g_cond_info_lock};
    auto& info = g_cond_info[cond];
    info.signals++;
    info.last_signal_ms = NowMs();
    if (broadcast && info.waiters > 0) {
        info.waiters = 0;
    }
}

void RecordSemaCreated(u32 handle, const char* name, s32 init_count, s32 max_count) {
    if (!g_enabled) {
        return;
    }
    std::scoped_lock lk{g_sema_info_lock};
    auto& info = g_sema_info[handle];
    info.name = name != nullptr ? name : "?";
    info.init_count = init_count;
    info.max_count = max_count;
}

void RecordSemaOverflow(u32 handle) {
    if (!g_enabled) {
        return;
    }
    std::scoped_lock lk{g_sema_info_lock};
    g_sema_info[handle].overflows++;
}

void RecordSemaEvent(const char* kind, u32 handle, s32 count) {
    const u64 tick = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (handle < MaxSemaHandles) {
        auto& counter = kind[0] == 'w' ? g_sema_waits[handle] : g_sema_signals[handle];
        counter.fetch_add(1, std::memory_order_relaxed);
    }
    std::scoped_lock lk{g_sema_mutex};
    const u64 slot = g_sema_index.fetch_add(1, std::memory_order_relaxed);
    SemaEvent event{kind, handle, count, tick};
    if (const auto* current = g_curthread; current != nullptr) {
        std::snprintf(event.thread, sizeof(event.thread), "%s", current->name.c_str());
    }
    g_sema_ring[slot % SemaRingSize] = event;
    auto& recent = g_sema_recent[handle];
    recent.push_back(event);
    if (recent.size() > 8) {
        recent.pop_front();
    }
}

bool TraceHleEnabled() {
    static const bool enabled = std::getenv("SHADPS4_TRACE_HLE") != nullptr;
    return enabled;
}

namespace {
struct HleHistory {
    static constexpr std::size_t Depth = 16;
    const char* names[Depth]{};
    u64 times[Depth]{};
    u64 rets[Depth]{};
    std::size_t next = 0;
};

thread_local HleHistory g_hle_history;
std::mutex g_hle_lock;
std::map<u32, HleHistory> g_hle_by_tid;
} // namespace

void RecordHleCallRet(const char* name, u64 ret) {
    RecordHleCall(name);
    if (!TraceHleEnabled()) {
        return;
    }
    std::scoped_lock lk{g_hle_lock};
    const auto* current = g_curthread;
    if (current == nullptr) {
        return;
    }
    const auto it = g_hle_by_tid.find(static_cast<u32>(current->tid.load()));
    if (it != g_hle_by_tid.end() && it->second.next > 0) {
        it->second.rets[(it->second.next - 1) % HleHistory::Depth] = ret;
    }
}

void RecordHleCall(const char* name) {
    if (!TraceHleEnabled()) {
        return;
    }
    // The guest calls mutex lock/unlock on every iteration of its loops; recording those would
    // push everything else out of the ring, and they are already covered by the wait trace.
    // Kernel synchronisation dominates every loop and already has its own trace; what a stall
    // report needs is the non-kernel work a thread was doing last, so only those are kept.
    const std::string_view pretty{name};
    if (pretty.find("Libraries::Kernel::") != std::string_view::npos ||
        pretty.find("Libraries::Pad::") != std::string_view::npos) {
        return;
    }
    auto& hist = g_hle_history;
    const auto slot = hist.next % HleHistory::Depth;
    hist.names[slot] = name;
    hist.times[slot] = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
    ++hist.next;
    const auto* current = g_curthread;
    if (current == nullptr) {
        return;
    }
    std::scoped_lock lk{g_hle_lock};
    if (g_hle_by_tid.size() < 400) {
        g_hle_by_tid[static_cast<u32>(current->tid.load())] = hist;
    }
}

void DumpHleTail(const char* path) {
    const std::string nl(1, static_cast<char>(10));
    std::ofstream out{path, std::ios::trunc};
    if (!out) {
        return;
    }
    std::scoped_lock lk{g_hle_lock};
    for (const auto& [tid, hist] : g_hle_by_tid) {
        out << "tid=" << tid << nl;
        const std::size_t count = std::min<std::size_t>(hist.next, HleHistory::Depth);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t slot = (hist.next - count + i) % HleHistory::Depth;
            if (hist.names[slot] == nullptr) {
                continue;
            }
            out << "  " << hist.times[slot] << " ret=" << hist.rets[slot] << " "
                << std::string_view{hist.names[slot]}.substr(0, 120) << nl;
        }
    }
    if (std::getenv("SHADPS4_DUMP_RIPS") != nullptr) {
        LogThreadRips();
    }
    // Per-thread last wait: a stall dump then carries the exact object the title is parked on.
    {
        const std::string nl(1, static_cast<char>(10));
        std::scoped_lock lk{g_ring_mutex};
        out << "  --- last wait per thread ---" << nl;
        for (const auto& [tid, e] : g_last_wait) {
            out << "  tid=" << tid << " '" << e.thread << "' " << e.name << " obj=" << e.obj
                << nl;
        }
    }
    out << DumpSemaSnapshot();
    out << DumpEqueueStats();
    LOG_INFO(Kernel, "HLE tail written to {}", path);
}

void ClearGuestWait() {
    if (!g_enabled) {
        return;
    }
    const auto* current = g_curthread;
    if (current == nullptr) {
        return;
    }
    std::scoped_lock lk{g_ring_mutex};
    const auto it = g_last_wait.find(static_cast<u32>(current->tid.load()));
    if (it != g_last_wait.end()) {
        it->second.obj = 0;
    }
}

void RecordGuestWait(const char* name, u64 guest_ra, u64 obj) {
    if (!g_enabled) {
        return;
    }
    // A title can spin on one wait millions of times per second; only record when the call site
    // changes or after a pause, which keeps the ring meaningful and the mutex cool.
    static thread_local u64 last_ra = ~u64{0};
    static thread_local u64 last_ms = 0;
    const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
    if (guest_ra == last_ra && now - last_ms < 100) {
        return;
    }
    last_ra = guest_ra;
    last_ms = now;
    WaitEntry entry{name, guest_ra, obj, ++g_tid_hint, now};
    const auto* current = g_curthread;
    if (current != nullptr) {
        entry.tid = static_cast<u32>(current->tid.load());
        const auto& thread_name = current->name;
        std::snprintf(entry.thread, sizeof(entry.thread), "%s", thread_name.c_str());
    }
    std::scoped_lock lk{g_ring_mutex};
    if (g_last_wait.size() < 400 || g_last_wait.contains(entry.tid)) {
        g_last_wait[entry.tid] = entry;
    }
    auto& hist = g_history[entry.tid];
    auto& slot = hist.entries[hist.count % HistorySize];
    slot = HistoryEntry{name, guest_ra, now};
    hist.count++;
}

void DumpGuestWaitRing() {
    if (!g_enabled) {
        return;
    }
    static std::atomic<u32> dump_index{0};
    const u32 index = dump_index.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lk{g_ring_mutex, std::defer_lock};
    if (!lk.try_lock()) {
        LOG_INFO(Kernel, "guest wait ring #{} skipped (ring busy)", index);
        return;
    }
    const std::string nl(1, static_cast<char>(10));
    LOG_INFO(Kernel, "guest wait ring #{} (last wait per thread, base in the boot log)", index);
    std::string report = "=== freeze dump #" + std::to_string(index) + " ===" + nl;

    Libraries::Fiber::FiberTraceDump(report);

    // What each waiting thread called right before it went to sleep. A stall is almost always a
    // call that never came back, so the tail of this history names the subsystem.
    if (TraceHleEnabled()) {
        std::scoped_lock hle_lock{g_hle_lock};
        report += "  --- last HLE calls per thread ---" + nl;
        for (const auto& [tid, hist] : g_hle_by_tid) {
            std::string line = "  tid=" + std::to_string(tid) + " :";
            const std::size_t count = std::min<std::size_t>(hist.next, HleHistory::Depth);
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t slot = (hist.next - count + i) % HleHistory::Depth;
                const char* name = hist.names[slot];
                if (name == nullptr) {
                    continue;
                }
                std::string_view pretty{name};
                // __PRETTY_FUNCTION__ looks like "... wrap(Args...) [ReturnType (*func)(Args...)]";
                // keep the bracketed part, which carries the HLE function name.
                const auto open = pretty.find('[');
                const auto close = pretty.rfind(']');
                if (open != std::string_view::npos && close != std::string_view::npos && close > open) {
                    pretty = pretty.substr(open + 1, close - open - 1);
                }
                line += " " + std::string(pretty);
            }
            report += line + nl;
        }
    }

    {
        std::scoped_lock info_lock{g_sema_info_lock};
        for (const auto& [handle, info] : g_sema_info) {
            LOG_INFO(Kernel, "  sema {} '{}' init={} max={} rejected_signals={}", handle, info.name,
                     info.init_count, info.max_count, info.overflows);
            report += std::string("  sema ") + std::to_string(handle) + " '" + info.name +
                      "' init=" + std::to_string(info.init_count) + " max=" +
                      std::to_string(info.max_count) + " rejected_signals=" +
                      std::to_string(info.overflows) + nl;
        }
    }
    // Totals per handle answer what a ring cannot: was this semaphore ever signalled?
    for (std::size_t h = 0; h < MaxSemaHandles; ++h) {
        const u32 waits = g_sema_waits[h].load(std::memory_order_relaxed);
        const u32 signals = g_sema_signals[h].load(std::memory_order_relaxed);
        if (waits == 0 && signals == 0) {
            continue;
        }
        LOG_INFO(Kernel, "  sema handle {}: {} waits, {} signals", h, waits, signals);
        report += "  sema handle " + std::to_string(h) + ": " + std::to_string(waits) +
                  " waits, " + std::to_string(signals) + " signals" + nl;
    }

    {
        std::scoped_lock sema_lock{g_sema_mutex};
        for (const auto& [handle, events] : g_sema_recent) {
            if (events.empty()) {
                continue;
            }
            LOG_INFO(Kernel, "  sema {} recent events:", handle);
            report += std::string("  sema ") + std::to_string(handle) + " recent events:" + nl;
            for (const auto& ev : events) {
                LOG_INFO(Kernel, "    {} count={} by '{}'", ev.kind, ev.count, ev.thread);
                report += std::string("    ") + ev.kind + " count=" + std::to_string(ev.count) +
                          " by '" + ev.thread + "'" + nl;
            }
        }
    }
    {
        std::scoped_lock sema_lock{g_sema_mutex};
        const u64 total = g_sema_index.load(std::memory_order_relaxed);
        const u64 count = total < SemaRingSize ? total : SemaRingSize;
        LOG_INFO(Kernel, "  last {} semaphore events (oldest first):", count);
        u64 first = count > 200 ? count - 200 : 0;
        for (u64 i = 0; i < count; ++i) {
            const auto& ev = g_sema_ring[(total - count + i) % SemaRingSize];
            LOG_INFO(Kernel, "    {} handle={} count={} by '{}' at={} ms", ev.kind, ev.handle,
                     ev.count, ev.thread, ev.ms);
            if (i < first) {
                continue;
            }
            report += "  sema " + std::string(ev.kind) + " handle=" + std::to_string(ev.handle) +
                      " count=" + std::to_string(ev.count) + nl;
        }
    }

    {
        std::scoped_lock lk{g_cond_info_lock};
        const u64 now = NowMs();
        bool announced = false;
        for (const auto& [cond, info] : g_cond_info) {
            if (info.waiters == 0) {
                continue;
            }
            if (!announced) {
                LOG_INFO(Kernel, "  condvars with waiters:");
                report += "  condvars with waiters:" + nl;
                announced = true;
            }
            // Dump the wait object itself: the words around the condvar usually hold a vtable-like
            // pointer and flags that identify the subsystem owning it.
            if (info.mutex != 0) {
                char hex[3 * 64 + 1]{};
                const auto* bytes = reinterpret_cast<const u8*>(info.mutex);
                std::size_t written = 0;
                for (std::size_t i = 0; i < 64 && written + 3 < sizeof(hex); ++i) {
                    std::snprintf(hex + written, sizeof(hex) - written, "%02x ", bytes[i]);
                    written += 3;
                }
                LOG_INFO(Kernel, "      object bytes at {:#x}: {}", info.mutex, hex);
                report += std::string("      object bytes at ") + std::to_string(info.mutex) +
                          ": " + hex + nl;
            }
            const u64 since_signal =
                info.last_signal_ms == 0 ? ~u64{0} : now - info.last_signal_ms;
            LOG_INFO(Kernel,
                     "    cond={:#x} mutex={:#x} waiters={} waits={} signals={} last_signal={} ms "
                     "ago",
                     cond, info.mutex, info.waiters, info.waits, info.signals,
                     since_signal == ~u64{0} ? 0 : since_signal);
            report += std::string("    cond=") + std::to_string(cond) + " mutex=" +
                      std::to_string(info.mutex) + " waiters=" +
                      std::to_string(info.waiters) + " waits=" + std::to_string(info.waits) +
                      " signals=" + std::to_string(info.signals) + " last_signal=" +
                      (since_signal == ~u64{0} ? std::string("never") :
                                                 std::to_string(since_signal) + " ms ago") + nl;
        }
    }
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    for (const auto& [tid, e] : g_last_wait) {
        // Resolve the guest return address to its module so the dump points at a file that can be
        // disassembled (the eboot is an encrypted SELF, so the offset is what matters there).
        std::string where = "?";
        if (auto* module = linker->FindByAddress(e.guest_ra); module != nullptr) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s+0x%llx", module->GetModuleInfo().name.data(),
                          static_cast<unsigned long long>(e.guest_ra - module->GetBaseAddress()));
            where = buf;
        }
        LOG_INFO(Kernel, "  thread='{}' tid={} wait {:<22} ra={:#x} ({}) obj={:#x} at={} ms",
                 e.thread, e.tid, e.name, e.guest_ra, where, e.obj, e.ms);
        report += std::string("  thread='") + e.thread + "' tid=" + std::to_string(e.tid) +
                  " wait " + e.name + " ra=" + std::to_string(e.guest_ra) + " (" + where +
                  ") obj=" + std::to_string(e.obj) + nl;
        {
            std::scoped_lock held_lock{g_held_mutex};
            auto held_it = g_held_mutexes.find(e.tid);
            if (held_it != g_held_mutexes.end()) {
                for (const auto& held : held_it->second) {
                    LOG_INFO(Kernel, "      holds '{}' obj={:#x}", held.name, held.mutex);
                    report += std::string("      holds '") + held.name + "' obj=" +
                              std::to_string(held.mutex) + nl;
                }
            }
        }
        auto hist_it = g_history.find(e.tid);
        if (hist_it != g_history.end()) {
            const auto& hist = hist_it->second;
            const u32 shown = std::min<u32>(hist.count, HistorySize);
            for (u32 i = 0; i < shown; ++i) {
                const auto& h = hist.entries[(hist.count - shown + i) % HistorySize];
                std::string hist_where = "?";
                if (auto* module = linker->FindByAddress(h.guest_ra); module != nullptr) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%s+0x%llx",
                                  module->GetModuleInfo().name.data(),
                                  static_cast<unsigned long long>(h.guest_ra -
                                                                  module->GetBaseAddress()));
                    hist_where = buf;
                }
                report += std::string("      recent: ") + h.name + " (" + hist_where + ") at=" +
                          std::to_string(h.ms) + nl;
            }
        }
    }
    {
        std::scoped_lock lk{g_mutex_owners_lock};
        if (!g_mutex_owners.empty()) {
            LOG_INFO(Kernel, "  contended mutexes and their owners:");
            report += "  contended mutexes and their owners:" + nl;
            for (const auto& [address, info] : g_mutex_owners) {
                // Name what the owner is blocked in: a lock that stays held while its owner waits
                // is either a lock order inversion or a lock that was never released.
                std::string owner_wait = "?";
                if (const auto it = g_last_wait.find(info.owner_tid); it != g_last_wait.end()) {
                    owner_wait = std::string(it->second.name) + " obj=" +
                                 std::to_string(it->second.obj);
                }
                if (info.hits >= 20) {
                    LOG_WARNING(Kernel,
                                "lock order inversion suspected: mutex '{}' is held by '{}' tid={} "
                                "which is parked in {}, while other threads keep blocking on it",
                                info.name, info.owner_name, info.owner_tid, owner_wait);
                }
                u64 guest_address = 0;
                {
                    std::scoped_lock lk{g_mutex_guest_addresses_lock};
                    if (const auto it = g_mutex_guest_addresses.find(address);
                        it != g_mutex_guest_addresses.end()) {
                        guest_address = it->second;
                    }
                }
                LOG_INFO(Kernel,
                         "    '{}' obj={:#x} guest={:#x} owner='{}' tid={} waiting on {} (waiter "
                         "ra={:#x}, {} hits)",
                         info.name, address, guest_address, info.owner_name, info.owner_tid,
                         owner_wait, info.waiter_ra, info.hits);
                report += std::string("    '") + info.name + "' obj=" + std::to_string(address) +
                          " guest=" + std::to_string(guest_address) + " owner='" +
                          info.owner_name + "' tid=" + std::to_string(info.owner_tid) +
                          " waiting on " + owner_wait + " (waiter ra=" +
                          std::to_string(info.waiter_ra) + ", " + std::to_string(info.hits) +
                          " hits)" + nl;
            }
        }
    }
    WriteFreezeReport(report);

    // A late image dump has a resolved import GOT, so a PLT stub in a wait site can be mapped
    // back to the API it calls (the start-up dump happens before relocation).
    if (const char* module_dir = std::getenv("SHADPS4_DUMP_MODULE_DIR"); module_dir != nullptr) {
        static std::atomic<u32> dumps{0};
        if (dumps.fetch_add(1, std::memory_order_relaxed) == 0) {
            linker->DumpAllModules(std::filesystem::path{module_dir} / "late");
        }
    }
}

void StartWaitRingDumpTimer() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!g_enabled) {
            return;
        }
        const char* env = std::getenv("SHADPS4_DUMP_WAITS_AT");
        if (env == nullptr && g_freeze_dump) {
            env = std::getenv("SHADPS4_FREEZE_DUMP");
        }
        std::vector<u64> times;
        std::string_view rest{env == nullptr ? "" : env};
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            const auto token = rest.substr(0, comma);
            const u64 seconds = std::strtoull(std::string{token}.c_str(), nullptr, 10);
            if (seconds != 0) {
                times.push_back(seconds);
            }
            if (comma == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(comma + 1);
        }
        if (times.empty()) {
            return;
        }
        std::thread{[times] {
            const auto start = std::chrono::steady_clock::now();
            for (const u64 seconds : times) {
                std::this_thread::sleep_until(start + std::chrono::seconds(seconds));
                DumpGuestWaitRing();
            }
        }}.detach();
        LOG_INFO(Kernel, "guest wait ring dump scheduled at {} points", times.size());
    });
}

} // namespace Libraries::Kernel
