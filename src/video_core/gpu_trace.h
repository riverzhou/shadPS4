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
    DrawIndirect = 11,
    DrawIndexedIndirect = 12,
};

struct Entry {
    u32 op{};
    u32 aux{};
    u64 a{};
    u64 b{};
    u64 c{};
};

// Large enough to hold several whole submissions: a hung submission is dumped operation by
// operation, so the ring must cover the frames that are still in flight.
inline constexpr u32 RingSize = 1 << 19;

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

// Debug-only: one device diagnostic checkpoint before every draw and dispatch, so that a GPU hang
// can be attributed to the exact command that was executing when the device stopped responding.
// The marker doubles as an index into a table describing the command.
inline constexpr u32 CheckpointRingSize = 1 << 16;
inline std::array<Entry, CheckpointRingSize> checkpoint_ring{};
inline std::atomic<u64> next_checkpoint{1};

inline u64 NextCheckpoint(Op op, u64 a = 0, u64 b = 0, u64 c = 0) {
    const u64 marker = next_checkpoint.fetch_add(1, std::memory_order_relaxed);
    checkpoint_ring[marker % CheckpointRingSize] = Entry{static_cast<u32>(op), 0, a, b, c};
    return marker;
}

inline const Entry& CheckpointEntry(u64 marker) {
    return checkpoint_ring[marker % CheckpointRingSize];
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
    case Op::DrawIndirect:
        return "DrawIndirect";
    case Op::DrawIndexedIndirect:
        return "DrawIndexedIndirect";
    }
    return "?";
}

inline void DumpCheckpoint(u64 marker) {
    const u64 newest = next_checkpoint.load(std::memory_order_relaxed);
    if (marker == 0 || marker >= newest) {
        return;
    }
    const auto& e = CheckpointEntry(marker);
    LOG_CRITICAL(Render, "  checkpoint marker={} {} a={:#x} b={:#x} c={:#x} aux={}", marker,
                 OpName(e.op), e.a, e.b, e.c, e.aux);
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

// Dumps one whole submission (0 = the most recently recorded submit, 1 = the one before it).
// A submission that hangs the GPU is identified by locating the submit it belongs to instead of
// guessing from the last few operations. Every scheduler (draw/present/flip) has its own timeline,
// so submissions are filtered by scheduler id.
inline void DumpSubmissionRange(const char* reason, u32 scheduler, u32 which, u64 tick,
                                u32 max_lines = 65536) {
    const u32 count = write_pos.load(std::memory_order_relaxed);
    const u32 oldest = count > RingSize ? count - RingSize : 0;
    if (count == 0) {
        return;
    }

    u32 boundaries[8]{};
    u32 found = 0;
    for (u32 i = count; i > oldest && found < std::size(boundaries); --i) {
        const auto& e = ring[(i - 1) % RingSize];
        if (static_cast<Op>(e.op) == Op::Submit && e.aux == scheduler &&
            (tick == 0 || e.b == tick)) {
            boundaries[found++] = i - 1;
            if (tick != 0) {
                break;
            }
        }
    }
    if (which >= found) {
        if (tick != 0) {
            LOG_CRITICAL(Render, "GPU submission tick={:#x} not found in trace: {}", tick, reason);
        }
        return;
    }

    const u32 end = boundaries[which];
    u32 begin = oldest;
    for (u32 i = end; i > oldest; --i) {
        const auto& e = ring[(i - 1) % RingSize];
        if (static_cast<Op>(e.op) == Op::Submit && e.aux == scheduler) {
            begin = i;
            break;
        }
    }
    const u32 total = end > begin ? end - begin : 0;
    const auto& submit = ring[end % RingSize];
    LOG_CRITICAL(Render, "GPU submission sched={} ({} ops, cmdbufs={}, tick={:#x}): {}", scheduler,
                 total, submit.a, submit.b, reason);
    const u32 shown = std::min(total, max_lines);
    for (u32 i = 0; i < shown; ++i) {
        const auto& e = ring[(begin + i) % RingSize];
        LOG_CRITICAL(Render, "  [{}] {} a={:#x} b={:#x} c={:#x} aux={}", i, OpName(e.op), e.a, e.b,
                     e.c, e.aux);
    }
    if (shown < total) {
        LOG_CRITICAL(Render, "  ... {} operations omitted", total - shown);
    }
}

inline void DumpSubmission(const char* reason, u32 scheduler, u32 which, u32 max_lines = 65536) {
    DumpSubmissionRange(reason, scheduler, which, 0, max_lines);
}

inline void DumpSubmissionByTick(const char* reason, u32 scheduler, u64 tick,
                                 u32 max_lines = 65536) {
    DumpSubmissionRange(reason, scheduler, 0, tick, max_lines);
}

} // namespace VideoCore::GpuTrace
