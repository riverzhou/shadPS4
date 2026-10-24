// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/videoout/video_out.h"
#ifdef _WIN64
#include <windows.h>
#endif
#include <filesystem>
#include <fstream>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/libraries/libs.h"
#include "core/libraries/pad/pad_errors.h"
#include "core/libraries/pad/pad_menu_state.h"
#include "core/user_settings.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdlib>
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "pad.h"

#include <algorithm>
#include <array>
#include <set>
#include <optional>

namespace Libraries::Pad {

using Input::GameController;
using Input::GameControllers;
using namespace Libraries::UserService;

struct HandleKey {
    OrbisUserServiceUserId id;
    s32 device_class;
    s32 index;

    bool operator==(const HandleKey&) const = default;
};
struct HandleKeyHash {
    std::size_t operator()(const HandleKey& k) const {
        std::size_t h1 = std::hash<OrbisUserServiceUserId>{}(k.id);
        std::size_t h2 = std::hash<s32>{}(k.device_class);
        std::size_t h3 = std::hash<s32>{}(k.index);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

static bool g_initialized = false;
static u64 pad_handle_counter = 1;
static std::unordered_map<HandleKey, s32, HandleKeyHash> pad_handle_map{};
static std::unordered_map<s32, GameController*> handle_to_controller_map{};

int PS4_SYSV_ABI scePadClose(s32 handle) {
    LOG_WARNING(Lib_Pad, "called, handle: {}", handle);
    if (handle_to_controller_map.erase(handle) == 0) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    for (auto& it : pad_handle_map) {
        if (it.second == handle) {
            pad_handle_map.erase(it.first);
            break;
        }
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadConnectPort() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDeviceClassGetExtendedInformation(
    s32 handle, OrbisPadDeviceClassExtendedInformation* pExtInfo) {
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    std::memset(pExtInfo, 0, sizeof(OrbisPadDeviceClassExtendedInformation));
    if (EmulatorSettings.IsUsingSpecialPad()) {
        pExtInfo->deviceClass = (OrbisPadDeviceClass)EmulatorSettings.GetSpecialPadClass();
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDeviceClassParseData(s32 handle, const OrbisPadData* pData,
                                            OrbisPadDeviceClassData* pDeviceClassData) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDeviceOpen() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDisableVibration() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDisconnectDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDisconnectPort() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableAutoDetect() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableExtensionPort() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableSpecificDeviceClass() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableUsbConnection() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetBluetoothAddress() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetCapability() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

// Keyboard-only setups: titles poll the pad for a connected controller and ignore input
// while it reports "disconnected", which makes keyboard bindings useless without a gamepad.
// Present a virtual connected pad in that case (see Input.emulatePadWithoutController).
static bool EmulatePadWithoutController(bool has_controller) {
    if (has_controller) {
        return false;
    }
    return EmulatorSettings.IsEmulatePadWithoutController();
}

int PS4_SYSV_ABI scePadGetControllerInformation(s32 handle, OrbisPadControllerInformation* pInfo) {
    LOG_DEBUG(Lib_Pad, "called handle = {}", handle);
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    const Input::State state = it->second->ReadState();

    std::memset(pInfo, 0, sizeof(OrbisPadControllerInformation));
    pInfo->touchPadInfo.pixelDensity = 1;
    pInfo->touchPadInfo.resolution.x = 1920;
    pInfo->touchPadInfo.resolution.y = 950;
    pInfo->stickInfo.deadZoneLeft = 1;
    pInfo->stickInfo.deadZoneRight = 1;
    pInfo->connectionType = ORBIS_PAD_CONNECTION_TYPE_LOCAL;
    pInfo->connectedCount = state.connected_count;
    pInfo->deviceClass = OrbisPadDeviceClass::Standard;
    pInfo->connected = state.connected;
    // No physical controller is connected in offline/automated runs; GT7 polls
    // controller information and never reads the pad while it reports "disconnected".
    // Present a virtual connected pad so the game starts polling input (test path only).
    if (EmulatePadWithoutController(!state.connected)) {
        pInfo->connected = true;
        pInfo->connectedCount = 1;
    }
    if (state.connected) {
        pInfo->deviceClass = EmulatorSettings.IsUsingSpecialPad()
                                 ? (OrbisPadDeviceClass)EmulatorSettings.GetSpecialPadClass()
                                 : OrbisPadDeviceClass::Standard;
    }
    LOG_DEBUG(Lib_Pad, "c: {} cc: {}, ct: {}, dc: {}", pInfo->connected, pInfo->connectedCount,
              pInfo->connectionType, std::to_underlying(pInfo->deviceClass));
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetDataInternal() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetDeviceId() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetDeviceInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetExtControllerInformation(s32 handle,
                                                   OrbisPadExtendedControllerInformation* pInfo) {
    LOG_INFO(Lib_Pad, "called handle = {}", handle);
    std::memset(pInfo, 0, sizeof(OrbisPadExtendedControllerInformation));
    return scePadGetControllerInformation(handle, &pInfo->base);
}

int PS4_SYSV_ABI scePadGetExtensionUnitInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetFeatureReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetHandle(Libraries::UserService::OrbisUserServiceUserId userId, s32 type,
                                 s32 index) {
    if (!g_initialized) {
        return ORBIS_PAD_ERROR_NOT_INITIALIZED;
    }
    if (userId == -1) {
        return ORBIS_PAD_ERROR_DEVICE_NO_HANDLE;
    }
    auto it = pad_handle_map.find({userId, type, index});
    if (it == pad_handle_map.end()) {
        return ORBIS_PAD_ERROR_DEVICE_NO_HANDLE;
    }
    s32 pad_handle = it->second;
    LOG_DEBUG(Lib_Pad, "called, userid: {}, out pad handle: {}", userId, pad_handle);
    return pad_handle;
}

int PS4_SYSV_ABI scePadGetIdleCount() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetInfo(OrbisPadInfo* data) {
    LOG_WARNING(Lib_Pad, "(DUMMY) called");
    if (!data) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    auto& controllers = *Common::Singleton<GameControllers>::Instance();
    auto col = controllers[0]->GetLightBarRGB();
    std::memset(data, 0, sizeof(OrbisPadInfo));
    data->unk1 = 0x1;
    data->pad_handle = 1;
    data->unk3 = 0x00000101;
    data->colour = col.r + (col.g << 8) + (col.b << 16);
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetInfoByPortType() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetLicenseControllerInformation() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetMotionSensorPosition() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetMotionTimerUnit() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetSphereRadius() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetVersionInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadInit() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    g_initialized = true;
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsBlasterConnected() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsDS4Connected() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsLightBarBaseBrightnessControllable() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsMoveConnected() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsMoveReproductionModel() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsValidHandle() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadMbusInit() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadMbusTerm() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadOpen(Libraries::UserService::OrbisUserServiceUserId userId, s32 type,
                            s32 index, const OrbisPadOpenParam* pParam) {
    if (!g_initialized) {
        return ORBIS_PAD_ERROR_NOT_INITIALIZED;
    }
    if (userId < 0) {
        return ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER;
    }
    if (pad_handle_map.find({userId, type, index}) != pad_handle_map.end()) {
        return ORBIS_PAD_ERROR_ALREADY_OPENED;
    }
    auto& controllers = *Common::Singleton<GameControllers>::Instance();
    if (userId == ORBIS_USER_SERVICE_USER_ID_SYSTEM) {
        if (type == ORBIS_PAD_PORT_TYPE_REMOTE_CONTROL) {
            s32 new_handle = pad_handle_counter++;
            pad_handle_map[{userId, type, index}] = new_handle;
            handle_to_controller_map[new_handle] = controllers[4];
            LOG_INFO(Lib_Pad, "Opened a TV remote device, out handle: {}", new_handle);
            return new_handle;
        }
        return ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER;
    }
    if (type == ORBIS_PAD_PORT_TYPE_REMOTE_CONTROL) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    auto u = UserManagement.GetUserByID(userId);
    if (!u) {
        return ORBIS_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN;
    }
    s32 new_handle = pad_handle_counter++;
    pad_handle_map[{userId, type, index}] = new_handle;

    handle_to_controller_map[new_handle] =
        controllers[type == (EmulatorSettings.IsUsingSpecialPad() ? 2 : 0)
                        ? UserManagement.GetUserByID(userId)->player_index - 1
                        : 4];
    LOG_INFO(Lib_Pad,
             "called user_id = {}, type = {}, index = {}, player index = {}, out handle = {}",
             userId, type, index, u->player_index, new_handle);
    scePadResetLightBar(new_handle);
    scePadResetOrientation(new_handle);
    return new_handle;
}

int PS4_SYSV_ABI scePadOpenExt(Libraries::UserService::OrbisUserServiceUserId userId, s32 type,
                               s32 index, const OrbisPadOpenExtParam* pParam) {
    LOG_WARNING(Lib_Pad, "Redirect to scePadOpen");
    return scePadOpen(userId, type, index, nullptr);
}

int PS4_SYSV_ABI scePadOpenExt2() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadOutputReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int ProcessStates(OrbisPadData* pData, const Input::State* states, s32 num) {
    if (num > 0 && !states[0].connected) {
        pData[0] = {};
        pData[0].orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        pData[0].connected = false;
        return 1;
    }

    const bool gamepad_input_intercepted = ImGui::Core::IsGamepadInputCaptured();
    for (int i = 0; i < num; i++) {
        pData[i] = {};
        if (gamepad_input_intercepted) {
            pData[i].buttons = OrbisPadButtonDataOffset::Intercepted;
            pData[i].leftStick = {128, 128};
            pData[i].rightStick = {128, 128};
            pData[i].orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            pData[i].connected = states[i].connected;
            pData[i].timestamp = states[i].time;
            pData[i].connectedCount = states[i].connected_count;
            pData[i].deviceUniqueDataLen = 0;
            continue;
        }

        pData[i].buttons = states[i].buttonsState;
        pData[i].leftStick.x = states[i].axes[static_cast<int>(Input::Axis::LeftX)];
        pData[i].leftStick.y = states[i].axes[static_cast<int>(Input::Axis::LeftY)];
        pData[i].rightStick.x = states[i].axes[static_cast<int>(Input::Axis::RightX)];
        pData[i].rightStick.y = states[i].axes[static_cast<int>(Input::Axis::RightY)];
        pData[i].analogButtons.l2 = states[i].axes[static_cast<int>(Input::Axis::TriggerLeft)];
        pData[i].analogButtons.r2 = states[i].axes[static_cast<int>(Input::Axis::TriggerRight)];
        pData[i].acceleration.x = states[i].acceleration.x * 0.098;
        pData[i].acceleration.y = states[i].acceleration.y * 0.098;
        pData[i].acceleration.z = states[i].acceleration.z * 0.098;
        pData[i].angularVelocity.x = states[i].angularVelocity.x;
        pData[i].angularVelocity.y = states[i].angularVelocity.y;
        pData[i].angularVelocity.z = states[i].angularVelocity.z;
        pData[i].orientation = states[i].orientation;
        pData[i].touchData.touchNum =
            (states[i].touchpad[0].state ? 1 : 0) + (states[i].touchpad[1].state ? 1 : 0);

        if (!states[i].touchpad[0].state && states[i].touchpad[1].state) {
            pData[i].touchData.touch[0].x = states[i].touchpad[1].x;
            pData[i].touchData.touch[0].y = states[i].touchpad[1].y;
            pData[i].touchData.touch[0].id = states[i].touchpad[1].ID;
        } else {
            pData[i].touchData.touch[0].x = states[i].touchpad[0].x;
            pData[i].touchData.touch[0].y = states[i].touchpad[0].y;
            pData[i].touchData.touch[0].id = states[i].touchpad[0].ID;
            pData[i].touchData.touch[1].x = states[i].touchpad[1].x;
            pData[i].touchData.touch[1].y = states[i].touchpad[1].y;
            pData[i].touchData.touch[1].id = states[i].touchpad[1].ID;
        }
        if (Common::ElfInfo::Instance().FirmwareVer() > Common::ElfInfo::FW_350) {
            pData[i].touchData.time_since_touch_held_down = states[i].touch_time_since_held_down;
        }
        pData[i].connected = states[i].connected;
        pData[i].timestamp = states[i].time;
        pData[i].connectedCount = states[i].connected_count;
        pData[i].deviceUniqueDataLen = 0;
    }

    return num;
}

// Press one button named by an external file for a moment, then consume the request. The emulator
// does not decide anything: a script watches the title's UI log and drops a button name here when
// the page it expects is really on screen.
static void ApplyExternalPress(OrbisPadData* pData, int processed) {
    static const char* input_file = std::getenv("SHADPS4_AUTO_INPUT_FILE");
    if (input_file == nullptr || processed <= 0) {
        return;
    }
    static u32 held_button = 0;
    static u64 held_until = 0;
    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    if (now < held_until) {
        pData[0].buttons =
            static_cast<OrbisPadButtonDataOffset>(std::to_underlying(pData[0].buttons) | held_button);
        return;
    }
    held_button = 0;
    std::ifstream file{input_file};
    if (!file) {
        return;
    }
    std::string name;
    std::getline(file, name);
    file.close();
    while (!name.empty() &&
           (name.back() == ' ' || static_cast<unsigned char>(name.back()) < 32)) {
        name.pop_back();
    }
    static const std::array<std::pair<std::string_view, OrbisPadButtonDataOffset>, 7> buttons{{
        {"cross", OrbisPadButtonDataOffset::Cross},
        {"circle", OrbisPadButtonDataOffset::Circle},
        {"options", OrbisPadButtonDataOffset::Options},
        {"up", OrbisPadButtonDataOffset::Up},
        {"down", OrbisPadButtonDataOffset::Down},
        {"left", OrbisPadButtonDataOffset::Left},
        {"right", OrbisPadButtonDataOffset::Right},
    }};
    for (const auto& [key, button] : buttons) {
        if (name == key) {
            held_button = std::to_underlying(button);
            break;
        }
    }
    // consume the request even when the name is unknown, so a typo cannot repeat forever
    std::error_code ec;
    std::filesystem::remove(input_file, ec);
    if (ec) {
        std::ofstream clear{input_file, std::ios::trunc};
    }
    if (held_button == 0) {
        return;
    }
    static const u64 hold_ms = [] {
        const char* env = std::getenv("SHADPS4_AUTO_PRESS_HOLD_MS");
        return env != nullptr ? std::strtoull(env, nullptr, 10) : 1200ull;
    }();
    held_until = now + hold_ms;
    LOG_INFO(Lib_Pad, "external press '{}' for {} ms", name, hold_ms);
    pData[0].buttons =
        static_cast<OrbisPadButtonDataOffset>(std::to_underlying(pData[0].buttons) | held_button);
}

int PS4_SYSV_ABI scePadRead(s32 handle, OrbisPadData* pData, s32 num) {
    LOG_TRACE(Lib_Pad, "called");
    if (pData == nullptr || num < 1 || num > ORBIS_PAD_MAX_DATA_NUM) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    std::array<Input::State, ORBIS_PAD_MAX_DATA_NUM> states;
    const int ret_num = controller.ReadStates(states.data(), num);
    int processed = ProcessStates(pData, states.data(), ret_num);
    // Auto-confirm is only for controller-less setups: a real (SDL) gamepad must disable it
    // even if the guest-side pad slots report disconnected.
    const bool has_controller = [] {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids != nullptr) {
            SDL_free(ids);
        }
        return count > 0;
    }();
    const bool emulate_pad = EmulatePadWithoutController(has_controller);
    static const bool pad_log = std::getenv("SHADPS4_PAD_LOG") != nullptr;
    if (emulate_pad) {
        static std::atomic<u32> read_log{0};
        if (read_log.fetch_add(1, std::memory_order_relaxed) % 300 == 0) {
            LOG_DEBUG(Lib_Pad, "scePadRead: handle={} num={} ret_num={} processed={}", handle, num,
                      ret_num, processed);
        }
    }
    if (emulate_pad && processed == 0) {
        // No physical controller is connected, but the automated test input still needs a
        // pad state to modify. Present a neutral, connected pad for the requested slots.
        for (s32 i = 0; i < num; ++i) {
            pData[i] = {};
            pData[i].connected = 1;
            pData[i].connectedCount = 1;
        }
        processed = num;
    }
    if (processed > 0 && emulate_pad) {
        // GT7 ignores input from pads it considers disconnected; the automated test input
        // must present itself as a connected pad on every handle the game polls.
        for (s32 i = 0; i < processed && i < num; ++i) {
            pData[i].connected = 1;
            pData[i].connectedCount = 1;
        }
    }
    if (pad_log && processed > 0) {
        static u32 pad_log_prev = 0;
        const u32 cur = std::to_underlying(pData[0].buttons);
        if (cur != pad_log_prev) {
            LOG_INFO(Lib_Pad, "pad log: buttons {:#x} -> {:#x} (handle={} connected={})",
                     pad_log_prev, cur, handle, pData[0].connected);
            pad_log_prev = cur;
        }
    }
    ApplyExternalPress(pData, processed);
    return processed;
}

int PS4_SYSV_ABI scePadReadBlasterForTracker() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadExt(s32 handle, OrbisPadData* pData, s32 num) {
    LOG_TRACE(Lib_Pad, "handle: {}, num: {}", handle, num);
    // We don't currently support extensions, pass through.
    return scePadRead(handle, pData, num);
}

int PS4_SYSV_ABI scePadReadForTracker() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadHistory() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadState(s32 handle, OrbisPadData* pData) {
    LOG_TRACE(Lib_Pad, "handle: {}", handle);
    const int result = scePadRead(handle, pData, 1);
    return result < 0 ? result : ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadStateExt(s32 handle, OrbisPadData* pData) {
    LOG_TRACE(Lib_Pad, "handle: {}", handle);
    // We don't currently support extensions, pass through.
    return scePadReadState(handle, pData);
}

int PS4_SYSV_ABI scePadResetLightBar(s32 handle) {
    LOG_DEBUG(Lib_Pad, "called, handle: {}", handle);
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    auto u = UserManagement.GetUserByPlayerIndex(controller.user_id);
    s32 colour_index = u ? u->user_color - 1 : 0;
    Input::Colour colour{255, 0, 0};
    if (colour_index >= 0 && colour_index <= 3) {
        colour = Input::g_user_colours[colour_index];
    } else {
        LOG_ERROR(Lib_Pad, "Invalid user colour value {} for controller {}, falling back to blue",
                  colour_index, handle);
    }
    controller.SetLightBarRGB(colour);
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetLightBarAll() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetLightBarAllByPortType() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetOrientation(s32 handle) {
    LOG_INFO(Lib_Pad, "scePadResetOrientation called handle = {}", handle);

    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    controller.ResetOrientation();

    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetOrientationForTracker() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetAngularVelocityBiasCorrectionState() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetAngularVelocityDeadbandState(s32 handle, bool bEnable) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetAutoPowerOffCount() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetButtonRemappingInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetConnection() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetExtensionReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetFeatureReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetForceIntercepted() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLightBar(s32 handle, const OrbisPadLightBarParam* pParam) {
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    if (pParam != nullptr) {
        LOG_DEBUG(Lib_Pad, "called handle = {} rgb = {} {} {}", handle, pParam->r, pParam->g,
                  pParam->b);

        if (pParam->r < 0xD && pParam->g < 0xD && pParam->b < 0xD) {
            LOG_INFO(Lib_Pad, "Invalid lightbar setting");
            return ORBIS_PAD_ERROR_INVALID_LIGHTBAR_SETTING;
        }

        auto& controllers = *Common::Singleton<GameControllers>::Instance();
        controller.SetLightBarRGB(pParam->r, pParam->g, pParam->b);
        return ORBIS_OK;
    }
    return ORBIS_PAD_ERROR_INVALID_ARG;
}

int PS4_SYSV_ABI scePadSetLightBarBaseBrightness() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLightBarBlinking() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLightBarForTracker(s32 handle, const OrbisPadLightBarParam* pParam) {
    LOG_INFO(Lib_Pad, "called, r: {} g: {} b: {}", pParam->r, pParam->g, pParam->b);
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    controller.SetLightBarRGB(pParam->r, pParam->g, pParam->b);
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLoginUserNumber() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetMotionSensorState(s32 handle, bool bEnable) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
    // it's already handled by the SDL backend and will be on no matter what
    // (assuming the controller supports it)
}

int PS4_SYSV_ABI scePadSetProcessFocus() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetProcessPrivilege() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetProcessPrivilegeOfButtonRemapping() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetShareButtonMaskForRemotePlay() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetTiltCorrectionState(s32 handle, bool bEnable) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetUserColor() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetVibration(s32 handle, const OrbisPadVibrationParam* pParam) {
    auto it = handle_to_controller_map.find(handle);
    if (it == handle_to_controller_map.end()) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto& controller = *it->second;
    if (pParam != nullptr) {
        LOG_DEBUG(Lib_Pad, "scePadSetVibration called handle = {} data = {} , {}", handle,
                  pParam->smallMotor, pParam->largeMotor);
        controller.SetVibration(pParam->smallMotor, pParam->largeMotor);
        return ORBIS_OK;
    }
    return ORBIS_PAD_ERROR_INVALID_ARG;
}

int PS4_SYSV_ABI scePadSetVibrationForce() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetVrTrackingMode() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadShareOutputData() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadStartRecording() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadStopRecording() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSwitchConnection() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVertualDeviceAddDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceAddDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceDeleteDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceDisableButtonRemapping() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceGetRemoteSetting() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceInsertData() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_298D21481F94C9FA() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_51E514BCD3A05CA5() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_89C9237E393DA243() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_EF103E845B6F0420() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    Common::Singleton<GameControllers>::Instance()->TryOpenSDLControllers();

    LIB_FUNCTION("6ncge5+l5Qs", "libScePad", 1, "libScePad", scePadClose);
    LIB_FUNCTION("kazv1NzSB8c", "libScePad", 1, "libScePad", scePadConnectPort);
    LIB_FUNCTION("AcslpN1jHR8", "libScePad", 1, "libScePad",
                 scePadDeviceClassGetExtendedInformation);
    LIB_FUNCTION("IHPqcbc0zCA", "libScePad", 1, "libScePad", scePadDeviceClassParseData);
    LIB_FUNCTION("d7bXuEBycDI", "libScePad", 1, "libScePad", scePadDeviceOpen);
    LIB_FUNCTION("0aziJjRZxqQ", "libScePad", 1, "libScePad", scePadDisableVibration);
    LIB_FUNCTION("pnZXireDoeI", "libScePad", 1, "libScePad", scePadDisconnectDevice);
    LIB_FUNCTION("9ez71nWSvD0", "libScePad", 1, "libScePad", scePadDisconnectPort);
    LIB_FUNCTION("77ooWxGOIVs", "libScePad", 1, "libScePad", scePadEnableAutoDetect);
    LIB_FUNCTION("+cE4Jx431wc", "libScePad", 1, "libScePad", scePadEnableExtensionPort);
    LIB_FUNCTION("E1KEw5XMGQQ", "libScePad", 1, "libScePad", scePadEnableSpecificDeviceClass);
    LIB_FUNCTION("DD-KiRLBqkQ", "libScePad", 1, "libScePad", scePadEnableUsbConnection);
    LIB_FUNCTION("Q66U8FdrMaw", "libScePad", 1, "libScePad", scePadGetBluetoothAddress);
    LIB_FUNCTION("qtasqbvwgV4", "libScePad", 1, "libScePad", scePadGetCapability);
    LIB_FUNCTION("gjP9-KQzoUk", "libScePad", 1, "libScePad", scePadGetControllerInformation);
    LIB_FUNCTION("Uq6LgTJEmQs", "libScePad", 1, "libScePad", scePadGetDataInternal);
    LIB_FUNCTION("hDgisSGkOgw", "libScePad", 1, "libScePad", scePadGetDeviceId);
    LIB_FUNCTION("4rS5zG7RFaM", "libScePad", 1, "libScePad", scePadGetDeviceInfo);
    LIB_FUNCTION("hGbf2QTBmqc", "libScePad", 1, "libScePad", scePadGetExtControllerInformation);
    LIB_FUNCTION("1DmZjZAuzEM", "libScePad", 1, "libScePad", scePadGetExtensionUnitInfo);
    LIB_FUNCTION("PZSoY8j0Pko", "libScePad", 1, "libScePad", scePadGetFeatureReport);
    LIB_FUNCTION("u1GRHp+oWoY", "libScePad", 1, "libScePad", scePadGetHandle);
    LIB_FUNCTION("kiA9bZhbnAg", "libScePad", 1, "libScePad", scePadGetIdleCount);
    LIB_FUNCTION("1Odcw19nADw", "libScePad", 1, "libScePad", scePadGetInfo);
    LIB_FUNCTION("4x5Im8pr0-4", "libScePad", 1, "libScePad", scePadGetInfoByPortType);
    LIB_FUNCTION("vegw8qax5MI", "libScePad", 1, "libScePad", scePadGetLicenseControllerInformation);
    LIB_FUNCTION("WPIB7zBWxVE", "libScePad", 1, "libScePad", scePadGetMotionSensorPosition);
    LIB_FUNCTION("k4+nDV9vbT0", "libScePad", 1, "libScePad", scePadGetMotionTimerUnit);
    LIB_FUNCTION("do-JDWX+zRs", "libScePad", 1, "libScePad", scePadGetSphereRadius);
    LIB_FUNCTION("QuOaoOcSOw0", "libScePad", 1, "libScePad", scePadGetVersionInfo);
    LIB_FUNCTION("hv1luiJrqQM", "libScePad", 1, "libScePad", scePadInit);
    LIB_FUNCTION("bi0WNvZ1nug", "libScePad", 1, "libScePad", scePadIsBlasterConnected);
    LIB_FUNCTION("mEC+xJKyIjQ", "libScePad", 1, "libScePad", scePadIsDS4Connected);
    LIB_FUNCTION("d2Qk-i8wGak", "libScePad", 1, "libScePad",
                 scePadIsLightBarBaseBrightnessControllable);
    LIB_FUNCTION("4y9RNPSBsqg", "libScePad", 1, "libScePad", scePadIsMoveConnected);
    LIB_FUNCTION("9e56uLgk5y0", "libScePad", 1, "libScePad", scePadIsMoveReproductionModel);
    LIB_FUNCTION("pFTi-yOrVeQ", "libScePad", 1, "libScePad", scePadIsValidHandle);
    LIB_FUNCTION("CfwUlQtCFi4", "libScePad", 1, "libScePad", scePadMbusInit);
    LIB_FUNCTION("s7CvzS+9ZIs", "libScePad", 1, "libScePad", scePadMbusTerm);
    LIB_FUNCTION("xk0AcarP3V4", "libScePad", 1, "libScePad", scePadOpen);
    LIB_FUNCTION("WFIiSfXGUq8", "libScePad", 1, "libScePad", scePadOpenExt);
    LIB_FUNCTION("71E9e6n+2R8", "libScePad", 1, "libScePad", scePadOpenExt2);
    LIB_FUNCTION("DrUu8cPrje8", "libScePad", 1, "libScePad", scePadOutputReport);
    LIB_FUNCTION("q1cHNfGycLI", "libScePad", 1, "libScePad", scePadRead);
    LIB_FUNCTION("fm1r2vv5+OU", "libScePad", 1, "libScePad", scePadReadBlasterForTracker);
    LIB_FUNCTION("QjwkT2Ycmew", "libScePad", 1, "libScePad", scePadReadExt);
    LIB_FUNCTION("2NhkFTRnXHk", "libScePad", 1, "libScePad", scePadReadForTracker);
    LIB_FUNCTION("3u4M8ck9vJM", "libScePad", 1, "libScePad", scePadReadHistory);
    LIB_FUNCTION("YndgXqQVV7c", "libScePad", 1, "libScePad", scePadReadState);
    LIB_FUNCTION("5Wf4q349s+Q", "libScePad", 1, "libScePad", scePadReadStateExt);
    LIB_FUNCTION("DscD1i9HX1w", "libScePad", 1, "libScePad", scePadResetLightBar);
    LIB_FUNCTION("+4c9xRLmiXQ", "libScePad", 1, "libScePad", scePadResetLightBarAll);
    LIB_FUNCTION("+Yp6+orqf1M", "libScePad", 1, "libScePad", scePadResetLightBarAllByPortType);
    LIB_FUNCTION("rIZnR6eSpvk", "libScePad", 1, "libScePad", scePadResetOrientation);
    LIB_FUNCTION("jbAqAvLEP4A", "libScePad", 1, "libScePad", scePadResetOrientationForTracker);
    LIB_FUNCTION("KLmYx9ij2h0", "libScePad", 1, "libScePad",
                 scePadSetAngularVelocityBiasCorrectionState);
    LIB_FUNCTION("r44mAxdSG+U", "libScePad", 1, "libScePad", scePadSetAngularVelocityDeadbandState);
    LIB_FUNCTION("ew647HuKi2Y", "libScePad", 1, "libScePad", scePadSetAutoPowerOffCount);
    LIB_FUNCTION("MbTt1EHYCTg", "libScePad", 1, "libScePad", scePadSetButtonRemappingInfo);
    LIB_FUNCTION("MLA06oNfF+4", "libScePad", 1, "libScePad", scePadSetConnection);
    LIB_FUNCTION("bsbHFI0bl5s", "libScePad", 1, "libScePad", scePadSetExtensionReport);
    LIB_FUNCTION("xqgVCEflEDY", "libScePad", 1, "libScePad", scePadSetFeatureReport);
    LIB_FUNCTION("lrjFx4xWnY8", "libScePad", 1, "libScePad", scePadSetForceIntercepted);
    LIB_FUNCTION("RR4novUEENY", "libScePad", 1, "libScePad", scePadSetLightBar);
    LIB_FUNCTION("dhQXEvmrVNQ", "libScePad", 1, "libScePad", scePadSetLightBarBaseBrightness);
    LIB_FUNCTION("etaQhgPHDRY", "libScePad", 1, "libScePad", scePadSetLightBarBlinking);
    LIB_FUNCTION("iHuOWdvQVpg", "libScePad", 1, "libScePad", scePadSetLightBarForTracker);
    LIB_FUNCTION("o-6Y99a8dKU", "libScePad", 1, "libScePad", scePadSetLoginUserNumber);
    LIB_FUNCTION("clVvL4ZDntw", "libScePad", 1, "libScePad", scePadSetMotionSensorState);
    LIB_FUNCTION("flYYxek1wy8", "libScePad", 1, "libScePad", scePadSetProcessFocus);
    LIB_FUNCTION("DmBx8K+jDWw", "libScePad", 1, "libScePad", scePadSetProcessPrivilege);
    LIB_FUNCTION("FbxEpTRDou8", "libScePad", 1, "libScePad",
                 scePadSetProcessPrivilegeOfButtonRemapping);
    LIB_FUNCTION("yah8Bk4TcYY", "libScePad", 1, "libScePad", scePadSetShareButtonMaskForRemotePlay);
    LIB_FUNCTION("vDLMoJLde8I", "libScePad", 1, "libScePad", scePadSetTiltCorrectionState);
    LIB_FUNCTION("z+GEemoTxOo", "libScePad", 1, "libScePad", scePadSetUserColor);
    LIB_FUNCTION("yFVnOdGxvZY", "libScePad", 1, "libScePad", scePadSetVibration);
    LIB_FUNCTION("8BOObG94-tc", "libScePad", 1, "libScePad", scePadSetVibrationForce);
    LIB_FUNCTION("--jrY4SHfm8", "libScePad", 1, "libScePad", scePadSetVrTrackingMode);
    LIB_FUNCTION("zFJ35q3RVnY", "libScePad", 1, "libScePad", scePadShareOutputData);
    LIB_FUNCTION("80XdmVYsNPA", "libScePad", 1, "libScePad", scePadStartRecording);
    LIB_FUNCTION("gAHvg6JPIic", "libScePad", 1, "libScePad", scePadStopRecording);
    LIB_FUNCTION("Oi7FzRWFr0Y", "libScePad", 1, "libScePad", scePadSwitchConnection);
    LIB_FUNCTION("0MB5x-ieRGI", "libScePad", 1, "libScePad", scePadVertualDeviceAddDevice);
    LIB_FUNCTION("N7tpsjWQ87s", "libScePad", 1, "libScePad", scePadVirtualDeviceAddDevice);
    LIB_FUNCTION("PFec14-UhEQ", "libScePad", 1, "libScePad", scePadVirtualDeviceDeleteDevice);
    LIB_FUNCTION("pjPCronWdxI", "libScePad", 1, "libScePad",
                 scePadVirtualDeviceDisableButtonRemapping);
    LIB_FUNCTION("LKXfw7VJYqg", "libScePad", 1, "libScePad", scePadVirtualDeviceGetRemoteSetting);
    LIB_FUNCTION("IWOyO5jKuZg", "libScePad", 1, "libScePad", scePadVirtualDeviceInsertData);
    LIB_FUNCTION("KY0hSB+Uyfo", "libScePad", 1, "libScePad", Func_298D21481F94C9FA);
    LIB_FUNCTION("UeUUvNOgXKU", "libScePad", 1, "libScePad", Func_51E514BCD3A05CA5);
    LIB_FUNCTION("ickjfjk9okM", "libScePad", 1, "libScePad", Func_89C9237E393DA243);
    LIB_FUNCTION("7xA+hFtvBCA", "libScePad", 1, "libScePad", Func_EF103E845B6F0420);
};

} // namespace Libraries::Pad
