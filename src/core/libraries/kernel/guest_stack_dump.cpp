// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/kernel/guest_stack_dump.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/thread_state.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Libraries::Kernel {

#ifdef _WIN32
namespace {
void DumpOneThread(Pthread* thread) {
    if (thread == nullptr || !thread->native_thr) {
        return;
    }
    const auto handle = reinterpret_cast<HANDLE>(thread->native_thr->GetHandle());
    if (handle == nullptr || handle == GetCurrentThread()) {
        return;
    }
    if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
        return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(handle, &ctx)) {
        LOG_INFO(Kernel, "guest thread '{}' tid={} rip={:#x} rsp={:#x} rbp={:#x}",
                 thread->name, thread->tid.load(), ctx.Rip, ctx.Rsp, ctx.Rbp);
        // The guest stack is mapped into this process, so the return addresses can be read
        // directly. Only print words that look like code/stack addresses to keep it short.
        const auto* stack = reinterpret_cast<const u64*>(ctx.Rsp);
        for (int i = 0; i < 24; ++i) {
            u64 value = 0;
            __try {
                value = stack[i];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (value >= 0x400000 && value < 0x8000000000ull) {
                LOG_INFO(Kernel, "  [rsp+{:#x}] = {:#x}", i * 8, value);
            }
        }
    }
    ResumeThread(handle);
}
} // namespace
#endif

void DumpGuestThreadStacks() {
#ifdef _WIN32
    LOG_INFO(Kernel, "guest stack dump requested");
    auto* thread_state = ThrState::Instance();
    if (!thread_state->thread_list_lock.try_lock()) {
        LOG_ERROR(Kernel, "guest stack dump: thread list is locked by another thread");
        return;
    }
    struct Unlocker {
        std::mutex& mutex;
        ~Unlocker() { mutex.unlock(); }
    } unlocker{thread_state->thread_list_lock};
    LOG_INFO(Kernel, "guest stack dump: {} live threads", thread_state->threads.size());
    for (Pthread* thread : thread_state->threads) {
        DumpOneThread(thread);
    }
#else
    LOG_INFO(Kernel, "guest stack dump is only implemented on Windows");
#endif
}

void StartThreadRipSampler() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* name = std::getenv("SHADPS4_SAMPLE_RIP_THREAD");
        if (name == nullptr) {
            return;
        }
        const std::string target{name};
        std::thread{[target] {
            // Let the title finish loading before touching its threads; suspending a guest
            // thread while the emulator is still wiring up libraries is not safe.
            std::this_thread::sleep_for(std::chrono::seconds(45));
            std::map<u64, u32> histogram;
            std::map<u64, u32> guest_words;
            std::map<u64, u32> host_words;
#ifdef _WIN32
            const auto module = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                module + static_cast<u64>(dos->e_lfanew));
            const u64 host_base = module;
            const u64 host_size = nt->OptionalHeader.SizeOfImage;
#else
            const u64 host_base = 0;
            const u64 host_size = 0;
#endif
            const auto start = std::chrono::steady_clock::now();
            auto last_report = start;
            while (true) {
                auto* thread_state = ThrState::Instance();
                Pthread* found = nullptr;
                {
                    std::scoped_lock lk{thread_state->thread_list_lock};
                    for (Pthread* thread : thread_state->threads) {
                        if (thread != nullptr && thread->name == target) {
                            found = thread;
                            break;
                        }
                    }
                }
                if (found != nullptr && found->native_thr) {
                    const auto handle =
                        reinterpret_cast<HANDLE>(found->native_thr->GetHandle());
                    if (handle != nullptr && handle != GetCurrentThread() &&
                        SuspendThread(handle) != static_cast<DWORD>(-1)) {
                        CONTEXT ctx{};
                        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                        if (GetThreadContext(handle, &ctx)) {
                            ++histogram[ctx.Rip];
                            // While the thread is parked in a system wait its stack still holds
                            // the return addresses, including the guest call site of the HLE
                            // function that is blocked. Only keep words that look like module
                            // addresses (low 32-bit address space, above the usual 16 MB).
                            const auto* stack = reinterpret_cast<const u64*>(ctx.Rsp);
                            for (int i = 0; i < 48; ++i) {
                                u64 value = 0;
                                __try {
                                    value = stack[i];
                                } __except (EXCEPTION_EXECUTE_HANDLER) {
                                    break;
                                }
                                if (value >= 0x400000 && value < 0x100000000ull) {
                                    ++guest_words[value];
                                }
                                // Words inside the emulator image are the host call frames of
                                // the blocked HLE function; symbolizing them names the wait.
                                if (value >= host_base && value < host_base + host_size) {
                                    ++host_words[value];
                                }
                            }
                        }
                        ResumeThread(handle);
                    }
                }
                const auto now = std::chrono::steady_clock::now();
                if (now - last_report >= std::chrono::seconds(5)) {
                    last_report = now;
                    std::vector<std::pair<u32, u64>> top;
                    for (const auto& [rip, count] : histogram) {
                        top.emplace_back(count, rip);
                    }
                    std::ranges::sort(top, std::greater<>{});
                    LOG_INFO(Kernel, "rip histogram for '{}' ({} samples)", target, histogram.size());
                    for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 4); ++i) {
                        LOG_INFO(Kernel, "  {:#x} x{}", top[i].second, top[i].first);
                    }
                    std::vector<std::pair<u32, u64>> guest_top;
                    for (const auto& [addr, count] : guest_words) {
                        guest_top.emplace_back(count, addr);
                    }
                    std::ranges::sort(guest_top, std::greater<>{});
                    LOG_INFO(Kernel, "stack words in the guest range:");
                    for (std::size_t i = 0;
                         i < std::min<std::size_t>(guest_top.size(), 8); ++i) {
                        LOG_INFO(Kernel, "  {:#x} x{}", guest_top[i].second, guest_top[i].first);
                    }
                    std::vector<std::pair<u32, u64>> host_top;
                    for (const auto& [addr, count] : host_words) {
                        host_top.emplace_back(count, addr - host_base);
                    }
                    std::ranges::sort(host_top, std::greater<>{});
                    LOG_INFO(Kernel, "stack words in the emulator image (module offsets):");
                    for (std::size_t i = 0; i < std::min<std::size_t>(host_top.size(), 12); ++i) {
                        LOG_INFO(Kernel, "  +{:#x} x{}", host_top[i].second, host_top[i].first);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }}.detach();
        LOG_INFO(Kernel, "rip sampler started for thread '{}'", target);
    });
}

void StartGuestStackDumpTimer() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* env = std::getenv("SHADPS4_DUMP_GUEST_STACKS_AT");
        if (env == nullptr) {
            return;
        }
        std::vector<u64> times;
        std::string_view rest{env};
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
                DumpGuestThreadStacks();
            }
        }}.detach();
        LOG_INFO(Kernel, "guest stack dump scheduled at {} points", times.size());
    });
}

} // namespace Libraries::Kernel
