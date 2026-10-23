// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/system/deviceservice.h"

#include <atomic>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"

namespace Libraries::DeviceService {

// The device service reports state changes of the system's devices (display output and its
// settings among them). Titles poll it while they wait for the system to settle; a stub that
// only returns zero keeps them waiting forever, so the arguments are logged while the semantics
// are being worked out.
s32 PS4_SYSV_ABI sceDeviceServiceInitialize() {
    LOG_INFO(Lib_SystemService, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceDeviceServiceGetEventState(u64 arg0, u64 arg1, u64 arg2, u64 arg3, u64 arg4,
                                               u64 arg5) {
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG_INFO(Lib_SystemService, "sceDeviceServiceGetEventState({:#x}, {:#x}, {:#x}, {:#x})",
                 arg0, arg1, arg2, arg3);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceDeviceServiceQueryDeviceInfo_(u64 arg0, u64 arg1) {
    LOG_INFO(Lib_SystemService, "called({:#x}, {:#x})", arg0, arg1);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceDeviceServiceGetGeneration(u64 arg0) {
    LOG_INFO(Lib_SystemService, "called({:#x})", arg0);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceDeviceServiceTerminate() {
    LOG_INFO(Lib_SystemService, "called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("84fDxStrG44", "libSceDeviceService", 1, "libSceMbus",
                 sceDeviceServiceInitialize);
    LIB_FUNCTION("9ddRUOV8Q5A", "libSceDeviceService", 1, "libSceMbus",
                 sceDeviceServiceGetEventState);
    LIB_FUNCTION("UNMEa+5lrUA", "libSceDeviceService", 1, "libSceMbus",
                 sceDeviceServiceQueryDeviceInfo_);
    LIB_FUNCTION("oFon+A5v1z8", "libSceDeviceService", 1, "libSceMbus",
                 sceDeviceServiceGetGeneration);
    LIB_FUNCTION("Uq8uW74rVpU", "libSceDeviceService", 1, "libSceMbus",
                 sceDeviceServiceTerminate);
}

} // namespace Libraries::DeviceService
