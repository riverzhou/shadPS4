// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include "common/logging/log.h"
#include "common/types.h"

namespace VideoCore::GpuTrace {

// Debug-only: a small ring of the most recent GPU submissions. When the device is lost the
// ring is dumped so the operation that hung the GPU can be identified without a debugger.
enum class Op : u32 {
    Draw = 1,
    DrawIndexed = 2,
    Dispatch = 3,
    DispatchIndirect = 4,
    CopyBuffer = 5,
    CopyImage = 6,
    BindSparse = 7,
    Submit = 8,
    Acquire = 9,
    Present = 10,
};

struct Entry {
    u32 op{};
    u32 aux{};
    u64 a{};
    u64 b{};
    u64 c{};
};

inline constexpr u32 RingSize = 2048;

// Set by any submit that observes VK_ERROR_DEVICE_LOST. Post-loss Vulkan queries (fault info,
// checkpoint data) fault inside the NVIDIA driver, so they must be skipped afterwards.
inline std::atomic<bool> g_device_lost{false};

// Number of present submissions that carried a device-diagnostic checkpoint. The watchdog
// compares it with the last checkpoint the GPU completed to distinguish "idle" from "stuck".
inline std::atomic<u64> g_present_submitted{0};
inline std::array<Entry, RingSize> ring{};
inline std::atomic<u32> write_pos{0};

inline void Record(Op op, u64 a = 0, u64 b = 0, u64 c = 0, u32 aux = 0) {
    const u32 index = write_pos.fetch_add(1, std::memory_order_relaxed) % RingSize;
    ring[index] = Entry{static_cast<u32>(op), aux, a, b, c};
}

inline const char* OpName(u32 op) {
    switch (static_cast<Op>(op)) {
    case Op::Draw:
        return "Draw";
    case Op::DrawIndexed:
        return "DrawIndexed";
    case Op::Dispatch:
        return "Dispatch";
    case Op::DispatchIndirect:
        return "DispatchIndirect";
    case Op::CopyBuffer:
        return "CopyBuffer";
    case Op::CopyImage:
        return "CopyImage";
    case Op::BindSparse:
        return "BindSparse";
    case Op::Submit:
        return "Submit";
    case Op::Acquire:
        return "Acquire";
    case Op::Present:
        return "Present";
    }
    return "?";
}

inline void Dump(const char* reason) {
    const u32 count = write_pos.load(std::memory_order_relaxed);
    const u32 window = 256;
    const u32 start = count > window ? count - window : (count > RingSize ? count - RingSize : 0);
    LOG_CRITICAL(Render, "GPU trace ({} ops, dumping {}): {}", count, count - start, reason);
    for (u32 i = start; i < count; ++i) {
        const auto& e = ring[i % RingSize];
        LOG_CRITICAL(Render, "  #{} {} a={:#x} b={:#x} c={:#x} aux={}", i - start,
                     OpName(e.op), e.a, e.b, e.c, e.aux);
    }
}

} // namespace VideoCore::GpuTrace
