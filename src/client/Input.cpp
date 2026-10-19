#include "client/Input.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "user32.lib")

#include <map>

namespace dh::input {

bool pressed(int vk) {
    // Edge detection per key, but shared between callers within the same frame: several handlers (HUD, renderer,
    // spikes) poll the same key with different modifiers, and the first poll must not eat the edge for the others
    // (Ctrl+F8 never arrived at the renderer because the HUD's F8 poll ran first, 2026-10-19). Polls within 10 ms
    // of the first sample reuse its result.
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

bool ctrlDown() {
    HWND  fg  = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

} // namespace dh::input
