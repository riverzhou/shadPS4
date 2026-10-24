// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/kernel/wait_trace.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "common/singleton.h"
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
std::mutex g_ring_mutex;
bool g_enabled = std::getenv("SHADPS4_DUMP_WAITS_AT") != nullptr;
std::atomic<u32> g_tid_hint{0};
} // namespace

namespace {
struct SemaEvent {
    const char* kind;
    u32 handle;
    s32 count;
    u64 ms;
};
constexpr std::size_t SemaRingSize = 128;
std::array<SemaEvent, SemaRingSize> g_sema_ring{};
std::atomic<u64> g_sema_index{0};
std::mutex g_sema_mutex;
} // namespace

void RecordSemaEvent(const char* kind, u32 handle, s32 count) {
    const u64 tick = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    std::scoped_lock lk{g_sema_mutex};
    const u64 slot = g_sema_index.fetch_add(1, std::memory_order_relaxed);
    g_sema_ring[slot % SemaRingSize] = SemaEvent{kind, handle, count, tick};
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
    // The index keeps consecutive dumps distinct so the log's duplicate filter cannot drop them.
    LOG_INFO(Kernel, "guest wait ring #{} (last wait per thread, base in the boot log)", index);
    {
        std::scoped_lock lk{g_sema_mutex};
        const u64 total = g_sema_index.load(std::memory_order_relaxed);
        const u64 count = total < SemaRingSize ? total : SemaRingSize;
        LOG_INFO(Kernel, "  last {} semaphore events (oldest first):", count);
        for (u64 i = 0; i < count; ++i) {
            const auto& ev = g_sema_ring[(total - count + i) % SemaRingSize];
            LOG_INFO(Kernel, "    {} handle={} count={} at={} ms", ev.kind, ev.handle, ev.count,
                     ev.ms);
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
    }
}

void StartWaitRingDumpTimer() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!g_enabled) {
            return;
        }
        const char* env = std::getenv("SHADPS4_DUMP_WAITS_AT");
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
