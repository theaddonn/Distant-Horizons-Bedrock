#include "client/Input.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "user32.lib")

#include <map>

namespace dh::input {

bool pressed(int vk) {
    static std::map<int, bool> was;
    HWND                       fg  = GetForegroundWindow();
    DWORD                      pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool  down = pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool& w    = was[vk];
    bool  edge = down && !w;
    w          = down;
    return edge;
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
