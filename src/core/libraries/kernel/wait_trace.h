// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

/// Record that the calling thread is about to block in the named guest wait call. The guest
/// return address is captured so a dump shows which guest call site is blocked. Cheap enough to
/// call on every wait, but only enabled when SHADPS4_DUMP_WAITS_AT is set.
// obj identifies the kernel object being waited on (the condvar, mutex, semaphore...), so a dump
// can tell two different objects that share the same guest call site apart.
void RecordGuestWait(const char* name, u64 guest_ra, u64 obj = 0);

/// Keep the most recent semaphore wait/signal events so a wait dump can show whether the
/// semaphore a frozen thread blocks on was ever signalled afterwards.
void RecordSemaEvent(const char* kind, u32 handle, s32 count);

/// Track which mutexes a guest thread currently holds. A thread that parks on a semaphore while
/// still holding a lock is a deadlock in progress, and this makes that visible in the dump.
void RecordMutexAcquired(u32 tid, u64 mutex, const char* name, u64 guest_ra);
void RecordMutexReleased(u32 tid, u64 mutex);

/// Schedule dumps of the wait ring at the comma separated seconds in SHADPS4_DUMP_WAITS_AT.
void StartWaitRingDumpTimer();

/// Log the last recorded guest waits (newest last).
void DumpGuestWaitRing();

} // namespace Libraries::Kernel
