#include "dh/config/KeyNames.h"

#include <cctype>
#include <string>

namespace dh::config {

namespace {

struct Named {
    const char* name;
    int         vk;
};

constexpr Named kNamed[] = {
    {"numpad-decimal", 0x6E},
    {"numpad-divide", 0x6F},
    {"numpad-multiply", 0x6A},
    {"numpad-subtract", 0x6D},
    {"numpad-add", 0x6B},
    {"insert", 0x2D},
    {"delete", 0x2E},
    {"home", 0x24},
    {"end", 0x23},
    {"pageup", 0x21},
    {"pagedown", 0x22},
    {"pause", 0x13},
    {"scrolllock", 0x91},
};

std::string lower(std::string_view s) {
    std::string n;
    n.reserve(s.size());
    for (char c : s) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n;
}

/// Parses 1..maxDigits decimal digits; -1 on failure.
int parseNumber(std::string_view s, int maxDigits) {
    if (s.empty() || static_cast<int>(s.size()) > maxDigits) return -1;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    return v;
}

} // namespace

int parseKeyName(std::string_view name) {
    const std::string n = lower(name);
    if (n.empty() || n == "none") return 0;

    if (n.size() == 1) {
        if (n[0] >= 'a' && n[0] <= 'z') return 'A' + (n[0] - 'a'); // VK_A..VK_Z = ASCII uppercase
        if (n[0] >= '0' && n[0] <= '9') return n[0];               // VK_0..VK_9 = ASCII digits
    }
    if (n[0] == 'f' && n.size() >= 2 && n[1] != '0') {
        const int k = parseNumber(std::string_view(n).substr(1), 2);
        if (k >= 1 && k <= 24) return 0x70 + (k - 1); // VK_F1 = 0x70
    }
    if (n.size() == 7 && n.compare(0, 6, "numpad") == 0 && n[6] >= '0' && n[6] <= '9') return 0x60 + (n[6] - '0');
    for (const auto& e : kNamed) {
        if (n == e.name) return e.vk;
    }
    return 0;
}

bool isValidKeyName(std::string_view name) {
    const std::string n = lower(name);
    return n.empty() || n == "none" || parseKeyName(n) != 0;
}

bool isVanillaKey(int vk) {
    // F1, F2, F3, F5, F11
    return vk == 0x70 || vk == 0x71 || vk == 0x72 || vk == 0x74 || vk == 0x7A;
}

} // namespace dh::config
