#pragma once

// The ONE rule for the level identifiers shared by the server driver and the client driver (GLUE-4).
//
//   levelKey  = sanitise(levelKeyPrefix + worldName + "_" + dimensionResourceLocation)   [a-zA-Z0-9-_], <= 150
//   levelName = sendLevelKeys ? levelKey : dimensionResourceLocation
//
// `levelName` is what FullDataSourceRequest.levelName carries and what ServerNetwork::findLevelByName compares
// byte-for-byte. The client learns the key from LevelInit (answer to RequestLevelInit(dimensionResourceLocation)),
// so with sendLevelKeys on it sends the key; with it off it never gets a key and sends the dimension resource
// location (e.g. "minecraft:overworld"). See the internal porting notes (GLUE-3 follow-ups).

#include <string>

namespace dh::bedrock {

inline std::string sanitizeLevelKey(const std::string& in) {
    std::string out;
    for (char c : in) {
        const bool ok =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.size() > 150) out.resize(150);
    return out;
}

inline std::string
makeLevelKey(const std::string& prefix, const std::string& worldName, const std::string& dimensionResource) {
    return sanitizeLevelKey(prefix + worldName + "_" + dimensionResource);
}

inline std::string
makeLevelName(bool sendLevelKeys, const std::string& levelKey, const std::string& dimensionResource) {
    return sendLevelKeys ? levelKey : dimensionResource;
}

} // namespace dh::bedrock
