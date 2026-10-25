// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Libraries::Kernel {

/// Start the debug timer configured with SHADPS4_DUMP_GUEST_STACKS_AT (comma separated
/// seconds). When a timer expires every guest thread is suspended and its instruction and
/// stack pointers are logged, which shows where a title is stuck even when the emulator itself
/// keeps running. No-op when the variable is unset.
void StartGuestStackDumpTimer();

/// Log instruction and stack pointer of every live guest thread.
void DumpGuestThreadStacks();

/// One line per thread with its current instruction pointer. Used by stall dumps to see where a
/// spinning thread actually is inside the guest image.
void LogThreadRips();

/// Sample the instruction pointer of one guest thread (named by SHADPS4_SAMPLE_RIP_THREAD) and
/// log a histogram every few seconds. Used to tell a guest spin loop (concentrated addresses in
/// the module) from a host wait (one address in a system DLL). No-op when the variable is unset.
void StartThreadRipSampler();

/// Log the caller chain of the current thread as raw stack words. Used from HLE entry points
/// (e.g. the title's own log writer) to name the guest code behind a call when the code is not
/// part of any module image.
void LogCurrentStackWords(const char* tag);

/// Start the timer configured with SHADPS4_DUMP_GUEST_MEM=<seconds>. When it expires every
/// mapped, CPU-readable guest region is written under user/guest_mem (capped by
/// SHADPS4_DUMP_GUEST_MEM_MAX_MB, default 2048) with a manifest. Needed for code the title loads
/// from its own archive: it never becomes a Module, so SHADPS4_DUMP_MODULE_DIR cannot see it.
void StartGuestMemoryDumpTimer();

} // namespace Libraries::Kernel

