#include "bedrock_common/Platform.h"

#include "ll/api/mod/NativeMod.h"

namespace dh::bedrock {

std::string sanitizePathComponent(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool bad = u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|'
                      || c == '?' || c == '*';
        out.push_back(bad ? '_' : c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) out = "_";
    return out;
}

std::filesystem::path ModPaths::worldDataDir(const std::string& levelName) const {
    return dataDir / "worlds" / std::filesystem::u8path(sanitizePathComponent(levelName));
}

ModPaths ModPaths::fromCurrentMod() {
    ModPaths p;
    if (auto mod = ll::mod::NativeMod::current()) {
        p.configDir = mod->getConfigDir();
        p.dataDir   = mod->getDataDir();
    }
    return p;
}

} // namespace dh::bedrock
