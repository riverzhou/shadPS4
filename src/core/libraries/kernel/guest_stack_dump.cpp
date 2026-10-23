// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/kernel/guest_stack_dump.h"

#include <chrono>
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
