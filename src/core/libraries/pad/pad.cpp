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

// Automated input is a debugging aid: it presses Cross (and navigates the start-up display
// settings page) without a user. Default off; the SHADPS4_AUTO_INPUT environment variable
// enables it for a single run without touching the configuration.
// Quiets the automatic confirm cadence while still reporting a connected pad: used to tell
// whether a title deadlocks because of the button presses themselves or just because of the
// virtual pad being present.
static bool AutoInputQuiet() {
    static const bool quiet = std::getenv("SHADPS4_AUTO_INPUT_QUIET") != nullptr;
    return quiet;
}

static bool AutoInputEnabled() {
    static const bool enabled = [] {
        if (const char* env = std::getenv("SHADPS4_AUTO_INPUT"); env != nullptr) {
            return env[0] != '\0' && env[0] != '0';
        }
        return EmulatorSettings.IsAutoInput();
    }();
    return enabled;
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
    // Called thousands of times per second by the title's update thread; logging this at info level
    // costs a formatting and a disk write per poll.
    LOG_DEBUG(Lib_Pad, "called handle = {}", handle);
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
    const bool auto_input = AutoInputEnabled();
    const bool auto_input_quiet = AutoInputQuiet();
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
            // A zeroed stick reads as "full left and full up", not as centred. The title drives
            // with it, so a pad that reports connected has to report a centred stick too.
            pData[i].leftStick = {128, 128};
            pData[i].rightStick = {128, 128};
            pData[i].orientation = {0.0f, 0.0f, 0.0f, 1.0f};
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
    // A frame counter for the tools: the title polls the pad about once per rendered frame, and
    // at one frame per second a wall-clock script drifts out of step with the pages. Recording
    // and replaying the counter instead of the clock keeps the presses on the same frames.
    if (processed > 0) {
        static const char* counter_file = std::getenv("SHADPS4_PAD_COUNTER_FILE");
        static u64 pad_frame = 0;
        static u64 pad_frame_ms = 0;
        const u64 now_ms = static_cast<u64>(SDL_GetTicks());
        if (counter_file != nullptr && (pad_frame_ms == 0 || now_ms - pad_frame_ms >= 50)) {
            ++pad_frame;
            pad_frame_ms = now_ms;
            std::ofstream out{counter_file, std::ios::trunc};
            out << pad_frame;
        }
    }

    // The external input file is a separate channel: it must work even when the automatic
    // confirm cadence is off, so a script can drive menus with hand-timed presses only.
    static const bool input_file_enabled = std::getenv("SHADPS4_AUTO_INPUT_FILE") != nullptr;
    // SHADPS4_KEYBOARD_PAD=1 makes the emulator read a real keyboard as a pad: arrow keys are the
    // d-pad, Enter is Cross, Backspace is Circle. The key events come from the normal window input
    // queue, so automation can drive the title with ordinary keystrokes instead of synthesising
    // pad state inside the emulator.
    static const bool keyboard_pad = std::getenv("SHADPS4_KEYBOARD_PAD") != nullptr;
    if (keyboard_pad && processed > 0) {
        // The emulator's own loop does not necessarily feed SDL's keyboard state, so read the OS
        // key state directly on Windows: this is the same state a person's key press produces,
        // and it needs no event pump. Other platforms fall back to SDL's array.
#ifdef _WIN64
        const auto pressed = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        const bool k_up = pressed(VK_UP);
        const bool k_down = pressed(VK_DOWN);
        const bool k_left = pressed(VK_LEFT);
        const bool k_right = pressed(VK_RIGHT);
        const bool k_cross = pressed(VK_RETURN);
        const bool k_circle = pressed(VK_BACK);
        const bool k_options = pressed(VK_ESCAPE);
        // Throttle and brake: the title's pedal choice can be R2/L2, and a menu-only keyboard
        // could not drive at all. Space = R2 (accelerate), left shift = L2 (brake).
        const bool k_r2 = pressed(VK_SPACE);
        const bool k_l2 = pressed(VK_LSHIFT);
        // Left stick, for the title's default "steer with the left stick" choice: A/D steer,
        // W/S push the stick forward/back.
        const bool k_stick_left = pressed('A');
        const bool k_stick_right = pressed('D');
        const bool k_stick_up = pressed('W');
        const bool k_stick_down = pressed('S');
#else
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const bool k_up = keys[SDL_SCANCODE_UP];
        const bool k_down = keys[SDL_SCANCODE_DOWN];
        const bool k_left = keys[SDL_SCANCODE_LEFT];
        const bool k_right = keys[SDL_SCANCODE_RIGHT];
        const bool k_cross = keys[SDL_SCANCODE_RETURN];
        const bool k_circle = keys[SDL_SCANCODE_BACKSPACE];
        const bool k_options = keys[SDL_SCANCODE_ESCAPE];
        const bool k_r2 = keys[SDL_SCANCODE_SPACE];
        const bool k_l2 = keys[SDL_SCANCODE_LSHIFT];
        const bool k_stick_left = keys[SDL_SCANCODE_A];
        const bool k_stick_right = keys[SDL_SCANCODE_D];
        const bool k_stick_up = keys[SDL_SCANCODE_W];
        const bool k_stick_down = keys[SDL_SCANCODE_S];
#endif
        u32 extra = 0;
        if (k_up) {
            extra |= std::to_underlying(OrbisPadButtonDataOffset::Up);
        }
        if (k_down) {
            extra |= std::to_underlying(OrbisPadButtonDataOffset::Down);
        }
        if (k_left) {
            extra |= std::to_underlying(OrbisPadButtonDataOffset::Left);
        }
        if (k_right) {
            extra |= std::to_underlying(OrbisPadButtonDataOffset::Right);
        }
        if (k_cross) {
            extra |= std::to_underlying(OrbisPadButtonDataOffset::Cross);
        }
        if (k_circle) {
            extra |= std::to_underlying(OrbisPadButtonDataOffset::Circle);
        }
        if (k_options) {
            extra |= std::to_underlying(OrbisPadButtonDataOffset::Options);
        }
        if (k_r2) {
            extra |= std::to_underlying(OrbisPadButtonDataOffset::R2);
            pData[0].analogButtons.r2 = 255;
        }
        if (k_l2) {
            extra |= std::to_underlying(OrbisPadButtonDataOffset::L2);
            pData[0].analogButtons.l2 = 255;
        }
        if (k_stick_left != k_stick_right) {
            pData[0].leftStick.x = k_stick_left ? 0 : 255;
        }
        if (k_stick_up != k_stick_down) {
            pData[0].leftStick.y = k_stick_up ? 0 : 255;
        }
        if (extra != 0) {
            pData[0].buttons =
                static_cast<OrbisPadButtonDataOffset>(std::to_underlying(pData[0].buttons) | extra);
        }
    }
    if (processed > 0 && (auto_input || input_file_enabled)) {
        // Confirm-only cadence is the default: it dismisses dialogs, skips the intro movie,
        // confirms the default row of a list and doubles as throttle once a race runs. Direction
        // keys are injected only while the title reports its start-up display settings screen on
        // display, because a direction pressed on a list screen changes the selection (a
        // timing-based guess eventually lands there and silently picks the wrong entry).
        // Hold times must survive the frame rate: the title samples the pad once per frame and
        // runs at a few frames per second here, so a 120 ms pulse can fall between two samples
        // and be lost. Every injection below is therefore several hundred milliseconds long.
        constexpr u64 CONFIRM_PERIOD_MS = 2000;
        constexpr u64 CONFIRM_HOLD_START_MS = 40;
        constexpr u64 CONFIRM_HOLD_END_MS = 420;
        // One display-settings page needs Down, Right and Cross in that order; the pulses have to
        // stay longer than a frame (the title renders at a few frames per second while it loads)
        // but the gaps can be short once the page has appeared.
        static const u64 settings_period_ms = [] {
            const char* env = std::getenv("SHADPS4_AUTO_SETTINGS_PERIOD_MS");
            return env != nullptr ? std::strtoull(env, nullptr, 10) : 1900ull;
        }();
        constexpr u64 SETTINGS_PERIOD_MS = 3600;
        const u64 ms = static_cast<u64>(SDL_GetTicks());
        // Press only after the picture has settled. A person waits for the screen to stop moving
        // before confirming; injecting during a transition can land in the middle of a menu
        // hand-over and leave the title waiting for a screen that will never appear.
        static const bool idle_gate = std::getenv("SHADPS4_AUTO_INPUT_IDLE") != nullptr;
        if (idle_gate) {
            const u64 last_flip = ::Libraries::VideoOut::LastFlipMs();
            const u64 now_steady = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now().time_since_epoch())
                                                       .count());
            static const u64 settle_ms = [] {
                const char* env = std::getenv("SHADPS4_AUTO_INPUT_IDLE_MS");
                return env != nullptr ? std::strtoull(env, nullptr, 10) : 250ull;
            }();
            if (last_flip != 0 && now_steady - last_flip < settle_ms) {
                return processed;
            }
        }
        if (auto_input_quiet) {
            // Virtual pad stays connected, but nothing is pressed.
            return processed;
        }
        // SHADPS4_AUTO_INPUT_DELAY_MS holds the automatic presses back for a while after start-up.
        // Pressing while the first menu pages are still being built can leave the title waiting for
        // a screen that never appears, so the delay lets the UI settle before anything is injected.
        static const u64 auto_input_delay_ms = [] {
            const char* env = std::getenv("SHADPS4_AUTO_INPUT_DELAY_MS");
            return env != nullptr ? std::strtoull(env, nullptr, 10) : 0ull;
        }();
        if (auto_input_delay_ms != 0 &&
            static_cast<u64>(SDL_GetTicks()) < auto_input_delay_ms) {
            return processed;
        }
        // Debug switch: scripted button presses at absolute times (milliseconds since start),
        // e.g. SHADPS4_AUTO_PRESS="100000:options,103000:down,107000:cross". Used to walk menus
        // that automated confirm-only input cannot reach.
        static const std::vector<std::pair<u64, u32>> scripted_presses = [] {
            std::vector<std::pair<u64, u32>> presses;
            const char* env = std::getenv("SHADPS4_AUTO_PRESS");
            if (env == nullptr) {
                return presses;
            }
            for (const auto& entry : std::views::split(std::string_view{env}, ',')) {
                const std::string item{entry.begin(), entry.end()};
                const auto colon = item.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                const u64 at = std::strtoull(item.c_str(), nullptr, 10);
                const std::string_view name{item.c_str() + colon + 1};
                u32 mask = 0;
                if (name == "cross") {
                    mask = static_cast<u32>(OrbisPadButtonDataOffset::Cross);
                } else if (name == "circle") {
                    mask = static_cast<u32>(OrbisPadButtonDataOffset::Circle);
                } else if (name == "options") {
                    mask = static_cast<u32>(OrbisPadButtonDataOffset::Options);
                } else if (name == "down") {
                    mask = static_cast<u32>(OrbisPadButtonDataOffset::Down);
                } else if (name == "up") {
                    mask = static_cast<u32>(OrbisPadButtonDataOffset::Up);
                } else if (name == "right") {
                    mask = static_cast<u32>(OrbisPadButtonDataOffset::Right);
                } else if (name == "left") {
                    mask = static_cast<u32>(OrbisPadButtonDataOffset::Left);
                } else if (name == "lstick_left") {
                    mask = 0x10001;
                } else if (name == "lstick_right") {
                    mask = 0x10002;
                } else if (name == "lstick_up") {
                    mask = 0x10003;
                } else if (name == "lstick_down") {
                    mask = 0x10004;
                }
                if (mask != 0) {
                    presses.emplace_back(at, mask);
                }
            }
            return presses;
        }();
        static const bool settings_plain =
            std::getenv("SHADPS4_AUTO_SETTINGS_PLAIN") != nullptr;
        // Debug switch: stop injecting input after the given time so the title can advance its
        // own flow (attract-mode results screens, idle timeouts) without automated confirms.
        static const u64 auto_stop_ms = [] {
            const char* env = std::getenv("SHADPS4_AUTO_STOP_AFTER");
            return env != nullptr ? std::strtoull(env, nullptr, 10) : ~u64{0};
        }();
        // Debug switch: let an external tool inject single presses by writing a button name
        // ("cross", "circle", "options", "down", "up", "left", "right") to a file. The file is
        // consumed on the next read, which makes screenshot-driven menu automation possible.
        static const char* input_file = std::getenv("SHADPS4_AUTO_INPUT_FILE");
        static const bool input_file_logged = [&] {
            if (input_file != nullptr) {
                LOG_INFO(Lib_Pad, "auto input: external input file '{}'", input_file);
            }
            return true;
        }();
        static u32 file_button = 0;
        static u64 file_button_until = 0;
        if (input_file != nullptr && ms >= file_button_until) {
            file_button = 0;
            if (std::ifstream file{input_file}; file) {
                std::string name;
                std::getline(file, name);
                while (!name.empty() && (name.back() == 13 || name.back() == ' ')) {
                    name.pop_back();
                }
                // A key may carry its own hold ("cross@1500"): the start-up screens only need a
                // short press, while the post-wizard screens run at one frame per second and need
                // the button held across a whole frame or the title never samples it.
                u64 press_hold_ms = 0;
                if (const auto at = name.find('@'); at != std::string::npos) {
                    press_hold_ms = std::strtoull(name.c_str() + at + 1, nullptr, 10);
                    name.resize(at);
                }
                if (name == "cross") {
                    file_button = static_cast<u32>(OrbisPadButtonDataOffset::Cross);
                } else if (name == "circle") {
                    file_button = static_cast<u32>(OrbisPadButtonDataOffset::Circle);
                } else if (name == "options") {
                    file_button = static_cast<u32>(OrbisPadButtonDataOffset::Options);
                } else if (name == "down") {
                    file_button = static_cast<u32>(OrbisPadButtonDataOffset::Down);
                } else if (name == "up") {
                    file_button = static_cast<u32>(OrbisPadButtonDataOffset::Up);
                } else if (name == "left") {
                    file_button = static_cast<u32>(OrbisPadButtonDataOffset::Left);
                } else if (name == "right") {
                    file_button = static_cast<u32>(OrbisPadButtonDataOffset::Right);
                } else if (name == "r2") {
                    file_button = static_cast<u32>(OrbisPadButtonDataOffset::R2);
                } else if (name == "l2") {
                    file_button = static_cast<u32>(OrbisPadButtonDataOffset::L2);
                }
                if (file_button != 0) {
                    static const u64 default_hold_ms = [] {
                        const char* env = std::getenv("SHADPS4_AUTO_PRESS_HOLD_MS");
                        return env != nullptr ? std::strtoull(env, nullptr, 10) : 1200ull;
                    }();
                    const u64 hold_ms = press_hold_ms != 0 ? press_hold_ms : default_hold_ms;
                    LOG_INFO(Lib_Pad, "auto input: external press '{}' for {} ms", name, hold_ms);
                    file_button_until = ms + hold_ms;
                }
                std::error_code remove_ec;
                std::filesystem::remove(input_file, remove_ec);
                if (remove_ec) {
                    // A stale handle can keep the file from being deleted; truncate it instead so
                    // the press is not repeated on every following read.
                    std::ofstream clear{input_file, std::ios::trunc};
                }
            }
        }
        static const bool settings_adjust =
            std::getenv("SHADPS4_AUTO_SETTINGS_ADJUST") != nullptr;
        if (file_button != 0) {
            pData[0].buttons = pData[0].buttons |
                              static_cast<OrbisPadButtonDataOffset>(file_button);
            // The triggers are analog on this pad: a pressed R2 has to report full travel, or a
            // title that reads the axis (rather than the button bit) never sees the throttle.
            if (file_button == static_cast<u32>(OrbisPadButtonDataOffset::R2)) {
                pData[0].analogButtons.r2 = 255;
            } else if (file_button == static_cast<u32>(OrbisPadButtonDataOffset::L2)) {
                pData[0].analogButtons.l2 = 255;
            }
        } else if (!auto_input) {
            // Only the external channel is active.
        } else if (ms >= auto_stop_ms) {
            pData[0].buttons = static_cast<OrbisPadButtonDataOffset>(0);
        } else if (std::getenv("SHADPS4_AUTO_INPUT_FRAMES") != nullptr &&
                   GetMenuScreen() == MenuScreen::StartupSettings) {
            // Frame-based cadence: the title samples the pad once per rendered frame, so reporting a
            // button for exactly one read guarantees it is seen once and never held across a screen
            // transition -- which is what a person does when the renderer is slow.
            static u32 read_index = 0;
            static u32 step = 0;
            ++read_index;
            // Stop after a bounded burst: a press that keeps arriving once the wizard has finished
            // walks the menu pages again, which looks like the title bouncing back and forth.
            static u64 first_press_ms = 0;
            static bool started = false;
            if (!started) {
                started = true;
                first_press_ms = static_cast<u64>(SDL_GetTicks());
            }
            // Generous but bounded: the wizard needs a handful of presses per page and the slow
            // renderer eats some of them, while an unbounded burst walked the menus forever.
            // Enough for the three wizard pages with margin, but short enough that the presses stop
            // once the wizard is over instead of walking the menus behind it.
            if (static_cast<u64>(SDL_GetTicks()) - first_press_ms > 180000 || step >= 40) {
                return processed;
            }
            // Stop the moment the wizard is over. Pressing on walks the menus behind it, which is
            // what made the settings pages appear to jump backwards.
            if (SettingsPagesFinalized() >= 3) {
                return processed;
            }
            // The page animates in; pressing during that window was measured to freeze the title.
            if (SettingsVisibleMs() < 20000) {
                return processed;
            }
            if (read_index % 3 == 1) {
                // Measured on screen: the page keeps the focus on its slider, so Down is required
                // to reach the Back/Next row (without it nothing is confirmed at all), then Right
                // settles on Next and Cross confirms it.
                constexpr std::array<OrbisPadButtonDataOffset, 3> sequence{
                    OrbisPadButtonDataOffset::Down,
                    OrbisPadButtonDataOffset::Right,
                    OrbisPadButtonDataOffset::Cross,
                };
                pData[0].buttons = pData[0].buttons | sequence[step % sequence.size()];
                ++step;
            }
        } else if (GetMenuScreen() == MenuScreen::StartupSettings && !settings_plain) {
            // The page opens with the slider focused: step down to the button row, move right to
            // "Next", then confirm. Each page accepts input only after its intro animation.
            const u64 phase = ms % (settings_adjust ? 4200 : settings_period_ms);
            if (settings_adjust) {
                // Debug switch: change the value before confirming so the title has something to
                // persist (tests the display-settings save path).
                if (phase < 380 || (phase >= 600 && phase < 980)) {
                    pData[0].buttons = pData[0].buttons | OrbisPadButtonDataOffset::Right;
                } else if (phase >= 1600 && phase < 1980) {
                    pData[0].buttons = pData[0].buttons | OrbisPadButtonDataOffset::Down;
                } else if (phase >= 2400 && phase < 2780) {
                    pData[0].buttons = pData[0].buttons | OrbisPadButtonDataOffset::Right;
                } else if (phase >= 3200 && phase < 3580) {
                    pData[0].buttons = pData[0].buttons | OrbisPadButtonDataOffset::Cross;
                }
            } else if (phase < 420) {
                pData[0].buttons = pData[0].buttons | OrbisPadButtonDataOffset::Down;
            } else if (phase >= 640 && phase < 1060) {
                pData[0].buttons = pData[0].buttons | OrbisPadButtonDataOffset::Right;
            } else if (phase >= 1280 && phase < 1700) {
                pData[0].buttons = pData[0].buttons | OrbisPadButtonDataOffset::Cross;
            }
        } else if (std::optional<u32> scripted = [&]() -> std::optional<u32> {
                       for (const auto& [at, mask] : scripted_presses) {
                           if (ms >= at && ms < at + 400) {
                               return mask;
                           }
                       }
                       return std::nullopt;
                   }()) {
            if (*scripted & 0x10000) {
                const u32 dir = *scripted & 0xFFFF;
                pData[0].leftStick.x = dir == 1 ? 0 : (dir == 2 ? 255 : 128);
                pData[0].leftStick.y = dir == 3 ? 0 : (dir == 4 ? 255 : 128);
            } else {
                pData[0].buttons = pData[0].buttons |
                                  static_cast<OrbisPadButtonDataOffset>(*scripted);
            }
        } else if (std::getenv("SHADPS4_AUTO_MENU_SCRIPT") != nullptr && ms > 150000) {
            // Debug switch: walk out of the attract-mode race through its pause menu so an
            // automated run can reach the main menu (and the save flow that runs there).
            const u64 phase = ms % 20000;
            const auto press = [&](u64 at, auto button) {
                if (phase >= at && phase < at + 150) {
                    pData[0].buttons = pData[0].buttons | button;
                }
            };
            press(0, OrbisPadButtonDataOffset::Options);
            press(6000, OrbisPadButtonDataOffset::Down);
            press(7000, OrbisPadButtonDataOffset::Down);
            press(8000, OrbisPadButtonDataOffset::Down);
            press(9500, OrbisPadButtonDataOffset::Cross);
            press(12000, OrbisPadButtonDataOffset::Cross);
        } else if (const char* exit_demo = std::getenv("SHADPS4_AUTO_EXIT_DEMO");
                   exit_demo != nullptr && ms > 150000) {
            // Debug switch: once the title is looping its attract-mode race, press a "back"
            // button instead of confirming, so automated runs can reach the main menu and the
            // save flow that runs there. Value selects the button ("circle" or "options").
            const std::string_view spec{exit_demo};
            const bool use_circle = spec.find("circle") != std::string_view::npos;
            u64 threshold_ms = 150000;
            if (const auto at = spec.find('@'); at != std::string_view::npos) {
                threshold_ms = std::strtoull(std::string{spec.substr(at + 1)}.c_str(), nullptr, 10);
            }
            if (ms < threshold_ms) {
                // Not yet in the attract-mode loop.
                const u64 phase = ms % CONFIRM_PERIOD_MS;
                if (phase >= CONFIRM_HOLD_START_MS && phase < CONFIRM_HOLD_END_MS) {
                    pData[0].buttons = pData[0].buttons | OrbisPadButtonDataOffset::Cross;
                }
                goto auto_input_done;
            }
            const u64 phase = ms % 5000;
            if (phase < 120) {
                pData[0].buttons = pData[0].buttons |
                                  (use_circle ? OrbisPadButtonDataOffset::Circle
                                              : OrbisPadButtonDataOffset::Options);
            }
        } else {
            const u64 phase = ms % CONFIRM_PERIOD_MS;
            if (phase >= CONFIRM_HOLD_START_MS && phase < CONFIRM_HOLD_END_MS) {
                pData[0].buttons = pData[0].buttons | OrbisPadButtonDataOffset::Cross;
            }
        }
    auto_input_done:
        // Log the press edges, not a sample: the injected timeline is what separates a missed
        // confirm from a screen that ignores input.
        static u32 buttons_prev = 0;
        const u32 buttons = std::to_underlying(pData[0].buttons);
        if (buttons != buttons_prev) {
            LOG_INFO(Lib_Pad, "auto input: buttons {:#x} -> {:#x} at {} ms (screen={})",
                     buttons_prev, buttons, ms, static_cast<int>(GetMenuScreen()));
            buttons_prev = buttons;
        }
        // Log the delivered state (after injection) at a low rate so menu input can be traced.
        static std::atomic<u32> delivered_log{0};
        if (delivered_log.fetch_add(1, std::memory_order_relaxed) % 240 == 0) {
            LOG_INFO(Lib_Pad, "scePadRead delivered: handle={} num={} buttons={:#x} connected={}",
                     handle, num, std::to_underlying(pData[0].buttons), pData[0].connected);
        }
    }
    if (std::getenv("SHADPS4_PAD_STATS") != nullptr) {
        // Count what each handle actually receives: a press that never lands on the handle the
        // title reads is invisible to it, however many times the emulator applied it.
        static std::atomic<u32> reads[16]{};
        static std::atomic<u32> pressed[16]{};
        static std::atomic<u32> tick{};
        const int slot = handle & 15;
        reads[slot].fetch_add(1, std::memory_order_relaxed);
        if (std::to_underlying(pData[0].buttons) != 0) {
            pressed[slot].fetch_add(1, std::memory_order_relaxed);
        }
        if (tick.fetch_add(1, std::memory_order_relaxed) % 4000 == 0) {
            for (int i = 0; i < 16; ++i) {
                if (reads[i].load() != 0) {
                    LOG_INFO(Lib_Pad, "pad stats handle={} reads={} pressed={}", i,
                             reads[i].load(), pressed[i].load());
                }
            }
        }
    }
    if (Libraries::Kernel::TraceHleEnabled()) {
        // A script that sees a stall writes user/dump_request; answering it here means the ring
        // describes the moment of the stall rather than whenever the dump timer happens to fire.
        static std::atomic<u32> dump_poll{0};
        if (dump_poll.fetch_add(1, std::memory_order_relaxed) % 300 == 0) {
            std::error_code ec;
            const std::filesystem::path request{"user/dump_request"};
            if (std::filesystem::exists(request, ec)) {
                std::filesystem::remove(request, ec);
                Libraries::Kernel::DumpHleTail("user/log/hle_tail.txt");
            }
        }
    }
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
