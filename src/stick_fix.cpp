// uevr-stick-fix: keeps each VR thumbstick working while the other
// controller is asleep, for any game running under UEVR with OpenXR.
//
// The bug: UEVR's OpenXR::get_stick_axis looks up the stick binding under the
// interaction profile of hands[0] (the LEFT hand) for both sticks. When one
// controller idles and the runtime changes or drops its interaction profile
// (seen on Quest 3 over Virtual Desktop), UEVR reads the other hand's stick
// as centred until the sleeping controller moves again.
//
// The fix: UEVR passes the game's XInput state to plugins after filling it.
// If UEVR left a stick at zero, this asks the OpenXR runtime directly for
// UEVR's own "joystick" action, filtered to that hand (no profile lookup),
// and writes the value in, scaled and dead-zoned the way UEVR does. Sticks
// that UEVR zeroes on purpose (d-pad shifting, snap turn, its own menu) are
// left alone. When UEVR already has a value, nothing happens.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "uevr/API.h"

#define XR_NO_PROTOTYPES
#include "openxr/openxr.h"
#include "openxr/openxr_loader_negotiation.h"

namespace {

const UEVR_PluginInitializeParam* g_param = nullptr;

// ---- Logging ----------------------------------------------------------------

template <typename... Args>
void log(const char* format, Args... args) {
    if (g_param == nullptr || g_param->functions == nullptr || g_param->functions->log_info == nullptr) {
        return;
    }
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    g_param->functions->log_info("[stick_fix] %s", buffer);
}

void log_text(const char* text) {
    log("%s", text);
}

// ---- UEVR settings (read from the game profile's config.txt) ----------------

// UEVR's DPadMethod enum
enum DPadMethod { RIGHT_TOUCH = 0, LEFT_TOUCH = 1, LEFT_JOYSTICK = 2, RIGHT_JOYSTICK = 3, GESTURE_HEAD = 4, GESTURE_HEAD_RIGHT = 5 };

struct Settings {
    float deadzone = 0.2f;       // VR_JoystickDeadzone (per axis)
    bool swap = false;           // VR_SwapControllerInputs
    bool dpad_shifting = true;   // VR_DPadShifting
    int dpad_method = RIGHT_TOUCH; // VR_DPadShiftingMethod
    bool snap_turn = false;      // VR_SnapTurn
};

Settings g_settings;
wchar_t g_config_path[MAX_PATH] = L"";
FILETIME g_config_time{};
DWORD g_next_config_check = 0;

bool parse_bool(const char* v) {
    return std::strncmp(v, "true", 4) == 0 || std::strncmp(v, "1", 1) == 0;
}

void load_settings() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_config_path, L"r") != 0 || f == nullptr) {
        return;
    }
    Settings s;
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char* eq = std::strchr(line, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        const char* key = line;
        const char* value = eq + 1;
        if (std::strcmp(key, "VR_JoystickDeadzone") == 0) s.deadzone = (float)std::atof(value);
        else if (std::strcmp(key, "VR_SwapControllerInputs") == 0) s.swap = parse_bool(value);
        else if (std::strcmp(key, "VR_DPadShifting") == 0) s.dpad_shifting = parse_bool(value);
        else if (std::strcmp(key, "VR_DPadShiftingMethod") == 0) s.dpad_method = std::atoi(value);
        else if (std::strcmp(key, "VR_SnapTurn") == 0) s.snap_turn = parse_bool(value);
    }
    std::fclose(f);
    g_settings = s;
    log("settings: deadzone %.2f, swap %s, d-pad shifting %s (method %d), snap turn %s",
        s.deadzone, s.swap ? "on" : "off", s.dpad_shifting ? "on" : "off", s.dpad_method, s.snap_turn ? "on" : "off");
}

// Re-reads config.txt when UEVR saves it (checked every 2 s)
void refresh_settings() {
    const DWORD now = GetTickCount();
    if (now < g_next_config_check) {
        return;
    }
    g_next_config_check = now + 2000;

    if (g_config_path[0] == L'\0') {
        wchar_t dir[MAX_PATH] = L"";
        if (g_param->functions->get_persistent_dir == nullptr ||
            g_param->functions->get_persistent_dir(dir, MAX_PATH) == 0) {
            return;
        }
        std::swprintf(g_config_path, MAX_PATH, L"%ls\\config.txt", dir);
    }

    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(g_config_path, GetFileExInfoStandard, &attrs)) {
        return;
    }
    if (CompareFileTime(&attrs.ftLastWriteTime, &g_config_time) != 0) {
        g_config_time = attrs.ftLastWriteTime;
        load_settings();
    }
}

// ---- OpenXR access ----------------------------------------------------------

PFN_xrGetActionStateVector2f g_get_vector2f = nullptr;
XrPath g_hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH}; // [0] left, [1] right
bool g_failed = false;
DWORD g_next_try = 0;
int g_tries = 0;
constexpr int MAX_TRIES = 10;
int g_error_logs = 0;

// UEVR links the OpenXR loader into UEVRBackend.dll, so a separate
// openxr_loader.dll in the process doesn't know UEVR's handles. Every OpenXR
// runtime DLL exports xrNegotiateLoaderRuntimeInterface, which gives its own
// xrGetInstanceProcAddr; the runtime knows UEVR's instance and session.
PFN_xrGetInstanceProcAddr find_runtime() {
    PFN_xrGetInstanceProcAddr result = nullptr;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snap, &entry); ok && result == nullptr; ok = Module32NextW(snap, &entry)) {
        const auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(entry.hModule, "xrNegotiateLoaderRuntimeInterface");
        if (negotiate == nullptr) {
            continue;
        }
        XrNegotiateLoaderInfo loader_info{};
        loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        loader_info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        loader_info.structSize = sizeof(loader_info);
        loader_info.minInterfaceVersion = 1;
        loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
        loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        loader_info.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
        XrNegotiateRuntimeRequest request{};
        request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
        request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        if (negotiate(&loader_info, &request) == XR_SUCCESS && request.getInstanceProcAddr != nullptr) {
            result = request.getInstanceProcAddr;
            log("OpenXR runtime: %ls", entry.szExePath);
        }
    }
    CloseHandle(snap);
    return result;
}

void give_up_after(const char* why) {
    if (++g_tries >= MAX_TRIES) {
        log("%s; giving up (UEVR's own stick handling is unchanged)", why);
        g_failed = true;
    }
}

bool ready() {
    if (g_failed) {
        return false;
    }
    if (g_get_vector2f != nullptr) {
        return true;
    }
    const DWORD now = GetTickCount();
    if (now < g_next_try) {
        return false;
    }
    g_next_try = now + 1000;

    const auto vr = g_param->vr;
    const auto openxr = g_param->openxr;
    if (vr == nullptr || openxr == nullptr || !vr->is_runtime_ready()) {
        return false;
    }
    if (!vr->is_openxr()) {
        log_text("UEVR is using OpenVR, which doesn't have this bug; nothing to do");
        g_failed = true;
        return false;
    }

    const auto instance = (XrInstance)openxr->get_xr_instance();
    if (instance == XR_NULL_HANDLE || openxr->get_xr_session() == nullptr) {
        return false;
    }

    const auto get_proc = find_runtime();
    if (get_proc == nullptr) {
        give_up_after("no OpenXR runtime DLL found");
        return false;
    }

    PFN_xrStringToPath string_to_path = nullptr;
    PFN_xrGetActionStateVector2f get_vector2f = nullptr;
    get_proc(instance, "xrStringToPath", (PFN_xrVoidFunction*)&string_to_path);
    get_proc(instance, "xrGetActionStateVector2f", (PFN_xrVoidFunction*)&get_vector2f);
    if (string_to_path == nullptr || get_vector2f == nullptr) {
        give_up_after("the runtime didn't provide xrStringToPath/xrGetActionStateVector2f");
        return false;
    }

    XrResult r = string_to_path(instance, "/user/hand/left", &g_hand_path[0]);
    if (r == XR_SUCCESS) {
        r = string_to_path(instance, "/user/hand/right", &g_hand_path[1]);
    }
    if (r != XR_SUCCESS) {
        log("xrStringToPath failed (%d)", (int)r);
        give_up_after("couldn't make the hand paths");
        return false;
    }

    g_get_vector2f = get_vector2f;
    log_text("ready");
    return true;
}

// One hand's stick straight from OpenXR (hand: 0 left, 1 right)
bool read_stick(int hand, float& x, float& y) {
    // UEVR's action handles are its XrActions under OpenXR. Looked up every
    // time, as UEVR re-creates its actions when bindings are edited.
    const auto action = (XrAction)g_param->vr->get_action_handle("/actions/default/in/Joystick");
    const auto session = (XrSession)g_param->openxr->get_xr_session();
    if (action == XR_NULL_HANDLE || session == XR_NULL_HANDLE) {
        return false;
    }
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = g_hand_path[hand];
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    const XrResult r = g_get_vector2f(session, &info, &state);
    if (r != XR_SUCCESS) {
        if (g_error_logs < 5) {
            ++g_error_logs;
            log("xrGetActionStateVector2f failed (%d)", (int)r);
        }
        return false;
    }
    if (!state.isActive) {
        return false;
    }
    x = state.currentState.x;
    y = state.currentState.y;
    return true;
}

// Is a thumbrest touched (hand: 0 left, 1 right)? Uses UEVR's action check,
// which reads per hand and isn't affected by the bug.
bool thumbrest_touched(int hand) {
    const auto vr = g_param->vr;
    const auto action = vr->get_action_handle(hand == 0 ? "/actions/default/in/ThumbrestTouchLeft" : "/actions/default/in/ThumbrestTouchRight");
    if (action == nullptr) {
        return false;
    }
    const auto source = hand == 0 ? vr->get_left_joystick_source() : vr->get_right_joystick_source();
    return vr->is_action_active(action, source);
}

// Does UEVR zero this stick on purpose right now? (hand: 0 left, 1 right;
// left_thumb: whether it feeds XInput's left thumb.) The d-pad methods zero
// by hand; head gestures and snap turn zero by XInput side.
bool zeroed_on_purpose(int hand, bool left_thumb, const XINPUT_STATE* state) {
    const Settings& s = g_settings;

    if (s.dpad_shifting) {
        switch (s.dpad_method) {
        case RIGHT_TOUCH:   // right thumbrest turns the left stick into a d-pad
            if (hand == 0 && thumbrest_touched(1)) return true;
            break;
        case LEFT_TOUCH:    // left thumbrest turns the right stick into a d-pad
            if (hand == 1 && thumbrest_touched(0)) return true;
            break;
        case LEFT_JOYSTICK: // left stick is always a d-pad
            if (hand == 0) return true;
            break;
        case RIGHT_JOYSTICK: // right stick is always a d-pad
            if (hand == 1) return true;
            break;
        default:            // head gestures: UEVR zeroes the left thumb while a
                            // gesture d-pad direction is held
            if (left_thumb && (state->Gamepad.wButtons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
                                                         XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) != 0) {
                return true;
            }
            break;
        }
    }

    // Snap turn zeroes the right thumb while held (the left thumb with the
    // right-joystick d-pad method); leave that stick to UEVR
    if (s.snap_turn) {
        const bool snap_left_thumb = s.dpad_shifting && s.dpad_method == RIGHT_JOYSTICK;
        if (left_thumb == snap_left_thumb) return true;
    }

    return false;
}

short to_thumb(float v, float deadzone) {
    if (std::fabs(v) < deadzone) {
        return 0;
    }
    float scaled = v * 32767.0f;
    if (scaled > 32767.0f) scaled = 32767.0f;
    if (scaled < -32767.0f) scaled = -32767.0f;
    return (short)scaled;
}

int g_fill_logs[2] = {0, 0};
constexpr int MAX_FILL_LOGS = 3;

// Fill one XInput thumb from one hand if UEVR left it centred
void fix_stick(int hand, bool left_thumb, SHORT& tx, SHORT& ty, const XINPUT_STATE* state) {
    if (tx != 0 || ty != 0) {
        return; // UEVR already has a value
    }
    if (zeroed_on_purpose(hand, left_thumb, state)) {
        return;
    }
    float x = 0.0f, y = 0.0f;
    if (!read_stick(hand, x, y)) {
        return;
    }
    const short nx = to_thumb(x, g_settings.deadzone);
    const short ny = to_thumb(y, g_settings.deadzone);
    if (nx == 0 && ny == 0) {
        return; // really centred
    }
    tx = nx;
    ty = ny;
    if (g_fill_logs[hand] < MAX_FILL_LOGS) {
        ++g_fill_logs[hand];
        log("UEVR read the %s stick as centred; filled in (%.2f, %.2f) from the runtime%s",
            hand == 0 ? "left" : "right", x, y, g_fill_logs[hand] == MAX_FILL_LOGS ? " (further fills not logged)" : "");
    }
}

void on_xinput_get_state(unsigned int* retval, unsigned int user_index, void* p) {
    if (retval == nullptr || *retval != ERROR_SUCCESS || p == nullptr) {
        return;
    }
    // Leave input alone while UEVR's own menu is open
    if (g_param->functions->is_drawing_ui != nullptr && g_param->functions->is_drawing_ui()) {
        return;
    }
    refresh_settings();
    if (!ready()) {
        return;
    }
    auto state = (XINPUT_STATE*)p;
    // Which hand feeds which XInput thumb
    const int left_thumb_hand = g_settings.swap ? 1 : 0;
    const int right_thumb_hand = g_settings.swap ? 0 : 1;
    fix_stick(left_thumb_hand, true, state->Gamepad.sThumbLX, state->Gamepad.sThumbLY, state);
    fix_stick(right_thumb_hand, false, state->Gamepad.sThumbRX, state->Gamepad.sThumbRY, state);
}

} // namespace

extern "C" __declspec(dllexport) void uevr_plugin_required_version(UEVR_PluginVersion* version) {
    // Only long-standing parts of the plugin interface are used
    version->major = 2;
    version->minor = 0;
    version->patch = 0;
}

extern "C" __declspec(dllexport) bool uevr_plugin_initialize(const UEVR_PluginInitializeParam* param) {
    g_param = param;
    if (param == nullptr || param->callbacks == nullptr || param->callbacks->on_xinput_get_state == nullptr) {
        return false;
    }
    param->callbacks->on_xinput_get_state(&on_xinput_get_state);
    log_text("loaded");
    return true;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
