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

/// Remember how a semaphore was created (name, initial and maximum count) and whether a signal
/// was ever rejected because the count would have exceeded the maximum: a dropped signal leaves
/// a waiter blocked forever even though the signaller believed it succeeded.
void RecordSemaCreated(u32 handle, const char* name, s32 init_count, s32 max_count);

/// Track condition variable activity: how many threads wait on a condvar and when it was last
/// signalled. A condvar with waiters but no recent signal is where a title hangs.
void RecordCondWait(u64 cond, u64 mutex);
void RecordCondWaitDone(u64 cond);
void RecordCondSignal(u64 cond, bool broadcast);

/// Report who signalled a condvar, once per (cond, thread) pair. A wait that never gets its signal
/// is the freeze signature; this shows which call sites do signal and which one is missing.
void TraceCondSignal(u64 cond, const char* what);
void RecordSemaOverflow(u32 handle);

/// Track which mutexes a guest thread currently holds. A thread that parks on a semaphore while
/// still holding a lock is a deadlock in progress, and this makes that visible in the dump.
void RecordMutexAcquired(u32 tid, u64 mutex, const char* name, u64 guest_ra);

/// Remember where a host mutex object lives in guest memory, so a contention report can name the
/// address the title itself uses (a static address is what a disassembly can point at).
void RecordMutexGuestAddress(u64 object, u64 guest_address);
void RecordMutexReleased(u32 tid, u64 mutex);

/// Remember the owner reported by the contention tracer for a mutex address. The strings are
/// copied, so nothing dangles when the mutex is destroyed later.
void RecordMutexOwner(u64 mutex, const char* name, u32 owner_tid, const char* owner_name,
                      u64 waiter_ra);

/// Schedule dumps of the wait ring at the comma separated seconds in SHADPS4_DUMP_WAITS_AT.
void StartWaitRingDumpTimer();

/// Log the last recorded guest waits (newest last).
void DumpGuestWaitRing();

} // namespace Libraries::Kernel
