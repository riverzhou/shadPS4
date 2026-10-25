// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <type_traits>

#include "common/types.h"
#include "core/libraries/kernel/wait_trace.h"
#ifdef _WIN32
#include <malloc.h>
#endif

namespace Xbyak {
class CodeGenerator;
}

namespace Libraries::Fiber {
struct OrbisFiberContext;
}

namespace Core {

union DtvEntry {
    std::size_t counter;
    u8* pointer;
};

struct Tcb {
    Tcb* tcb_self;
    DtvEntry* tcb_dtv;
    void* tcb_thread;
    void* tcb_spare[2];
    u64 tcb_canary;
    ::Libraries::Fiber::OrbisFiberContext* tcb_fiber;
};

#ifdef _WIN32
/// Gets the thread local storage key for the TCB block.
u32 GetTcbKey();
#endif

/// Sets the data pointer to the TCB block.
void SetTcbBase(void* image_address);

/// Retrieves Tcb structure for the calling thread.
Tcb* GetTcbBase();

/// Makes sure TLS is initialized for the thread before entering guest.
void InitializeTLS();

template <auto f>
struct HostCallWrapperImpl;

template <class T>
u64 AsPlainU64(const T& value) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<u64>(value);
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        return static_cast<u64>(value);
    } else {
        return 0;
    }
}

template <class ReturnType, class... Args, PS4_SYSV_ABI ReturnType (*func)(Args...)>
struct HostCallWrapperImpl<func> {
    static ReturnType PS4_SYSV_ABI wrap(Args... args) {
        const bool trace = Libraries::Kernel::TraceHleEnabled();
        if constexpr (std::is_void_v<ReturnType>) {
            if (trace) {
                Libraries::Kernel::RecordHleCall(__PRETTY_FUNCTION__);
            }
            func(args...);
        } else {
            if (trace) {
                Libraries::Kernel::RecordHleCall(__PRETTY_FUNCTION__);
            }
            const ReturnType result = func(args...);
            if (trace) {
                Libraries::Kernel::RecordHleCallRet(__PRETTY_FUNCTION__, Core::AsPlainU64(result));
            }
            return result;
        }
    }
};

#define HOST_CALL(func) (Core::HostCallWrapperImpl<func>::wrap)

} // namespace Core
