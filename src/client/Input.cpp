#include "client/Input.h"

#include "bedrock_common/FileLogger.h"
#include "dh/config/KeyNames.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "user32.lib")

#include <map>

namespace dh::input {

bool pressed(int vk) {
    // Edge detection per key, but shared between callers within the same frame: several handlers (HUD, renderer)
    // may poll the same key, and the first poll must not eat the edge for the others. Polls within 10 ms of the
    // first sample reuse its result.
    struct State {
        bool          was       = false;
        bool          edge      = false;
        unsigned long sampledAt = 0;
    };
    static std::map<int, State> states;
    State&                      st  = states[vk];
    const unsigned long         now = GetTickCount();
    if (st.sampledAt != 0 && now - st.sampledAt < 10) return st.edge;
    HWND  fg  = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool down    = pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    st.edge      = down && !st.was;
    st.was       = down;
    st.sampledAt = now ? now : 1;
    return st.edge;
}

} // namespace dh::input

namespace dh::input {

bool shiftDown() {
    HWND  fg  = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

} // namespace dh::input

namespace dh::input {

int resolveKey(std::string const& name, char const* optionPath) {
    if (!dh::config::isValidKeyName(name)) {
        DH_LOG_W("config: %s: unknown key name \"%s\" (treated as none; see config.json docs)", optionPath, name.c_str());
        return 0;
    }
    const int vk = dh::config::parseKeyName(name);
    if (vk != 0 && dh::config::isVanillaKey(vk))
        DH_LOG_W("config: %s: \"%s\" is a key Minecraft uses itself (F1, F2, F3, F5, F11); pick another", optionPath, name.c_str());
    return vk;
}

} // namespace dh::input
