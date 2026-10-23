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

/// Sample the instruction pointer of one guest thread (named by SHADPS4_SAMPLE_RIP_THREAD) and
/// log a histogram every few seconds. Used to tell a guest spin loop (concentrated addresses in
/// the module) from a host wait (one address in a system DLL). No-op when the variable is unset.
void StartThreadRipSampler();

} // namespace Libraries::Kernel

