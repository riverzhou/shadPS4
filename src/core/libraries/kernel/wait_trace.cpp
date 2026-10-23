// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/kernel/wait_trace.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/logging/log.h"

namespace Libraries::Kernel {

namespace {
constexpr std::size_t RingSize = 32;

struct WaitEntry {
    const char* name;
    u64 guest_ra;
    u32 tid;
    u64 ms;
};

std::array<WaitEntry, RingSize> g_ring{};
std::size_t g_ring_pos{};
std::mutex g_ring_mutex;
bool g_enabled = std::getenv("SHADPS4_DUMP_WAITS_AT") != nullptr;
std::atomic<u32> g_tid_hint{0};
} // namespace

void RecordGuestWait(const char* name, u64 guest_ra) {
    if (!g_enabled) {
        return;
    }
    const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
    std::scoped_lock lk{g_ring_mutex};
    g_ring[g_ring_pos] = WaitEntry{name, guest_ra, ++g_tid_hint, now};
    g_ring_pos = (g_ring_pos + 1) % RingSize;
}

void DumpGuestWaitRing() {
    if (!g_enabled) {
        return;
    }
    std::scoped_lock lk{g_ring_mutex};
    LOG_INFO(Kernel, "guest wait ring (oldest first, base of the module is in the boot log)");
    for (std::size_t i = 0; i < RingSize; ++i) {
        const auto& e = g_ring[(g_ring_pos + i) % RingSize];
        if (e.name == nullptr) {
            continue;
        }
        LOG_INFO(Kernel, "  wait {:<24} ra={:#x} seq={} at={} ms", e.name, e.guest_ra, e.tid,
                 e.ms);
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
