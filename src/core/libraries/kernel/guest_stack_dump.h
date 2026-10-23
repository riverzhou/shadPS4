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

} // namespace Libraries::Kernel
