// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

/// Record that the calling thread is about to block in the named guest wait call. The guest
/// return address is captured so a dump shows which guest call site is blocked. Cheap enough to
/// call on every wait, but only enabled when SHADPS4_DUMP_WAITS_AT is set.
void RecordGuestWait(const char* name, u64 guest_ra);

/// Schedule dumps of the wait ring at the comma separated seconds in SHADPS4_DUMP_WAITS_AT.
void StartWaitRingDumpTimer();

/// Log the last recorded guest waits (newest last).
void DumpGuestWaitRing();

} // namespace Libraries::Kernel
