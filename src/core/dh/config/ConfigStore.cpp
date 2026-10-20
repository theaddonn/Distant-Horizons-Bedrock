// nlohmann is used without exceptions: parse(..., allow_exceptions=false), find() instead of at().
#ifndef JSON_NOEXCEPTION
#define JSON_NOEXCEPTION
#endif

#include "dh/config/ConfigStore.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <nlohmann/json.hpp>

namespace dh {

namespace {

using Json = nlohmann::json;

/// Walks "a.b.c" through nested objects; nullptr if any step is missing or not an object.
const Json* findPath(const Json& root, const char* path) {
    const Json* cur = &root;
    const char* seg = path;
    while (true) {
        if (!cur->is_object()) return nullptr;
        const char*       dot = std::strchr(seg, '.');
        const std::string key = dot ? std::string(seg, static_cast<std::size_t>(dot - seg)) : std::string(seg);
        auto              it  = cur->find(key);
        if (it == cur->end()) return nullptr;
        cur = &*it;
        if (!dot) return cur;
        seg = dot + 1;
    }
}

/// Creates the nested objects for "a.b.c" and returns the slot for the leaf.
Json& slotFor(Json& root, const char* path) {
    Json*       cur = &root;
    const char* seg = path;
    while (true) {
        const char*       dot = std::strchr(seg, '.');
        const std::string key = dot ? std::string(seg, static_cast<std::size_t>(dot - seg)) : std::string(seg);
        cur                   = &(*cur)[key];
        if (!dot) return *cur;
        seg = dot + 1;
    }
}

/// Shortest decimal that round-trips a float, as a double (so the JSON text stays short).
double shortFloat(float f) {
    if (!std::isfinite(f)) return static_cast<double>(f);
    char buf[40];
    for (int prec = 1; prec <= 9; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, static_cast<double>(f));
        if (std::strtof(buf, nullptr) == f) break;
    }
    return std::strtod(buf, nullptr);
}

/// Enum names compare case-insensitively with '-' == '_' ("beyond-vanilla" == "BEYOND_VANILLA").
bool enumNameEquals(const std::string& a, const char* b) {
    const auto norm = [](char c) {
        if (c == '-') return '_';
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    std::size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (norm(a[i]) != norm(b[i])) return false;
    }
    return i == a.size() && b[i] == '\0';
}

void warn(std::vector<std::string>& w, const OptionMeta& m, const char* why) {
    w.push_back(std::string(m.path) + ": " + why + ", using default");
}

} // namespace

ConfigStore::ConfigStore(int cores) : cores_(cores) {
    if (cores_ <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        cores_            = hw > 0 ? static_cast<int>(hw) : 4;
    }
    options_ = defaultOptions(cores_);
}

std::optional<std::string> ConfigStore::load(std::string_view jsonText) {
    Json root = Json::parse(jsonText.begin(), jsonText.end(), nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) return std::string("config is not valid JSON");
    if (!root.is_object()) return std::string("config root must be a JSON object");

    warnings_.clear();
    if (const Json* v = findPath(root, "version"); v != nullptr && v->is_number_integer()) {
        if (v->get<long long>() > kVersion)
            warnings_.push_back("config version is newer than supported; unknown options are ignored");
    }

    Options           loaded = defaultOptions(cores_);
    const OptionMeta* table  = optionMeta();
    for (std::size_t i = 0; i < optionMetaCount(); ++i) {
        const OptionMeta& m = table[i];
        const Json*       j = findPath(root, m.path);
        if (j == nullptr) continue; // missing -> default
        void* p = m.mutablePtr(loaded);
        switch (m.type) {
        case OptionType::Bool:
            if (j->is_boolean()) *static_cast<bool*>(p) = j->get<bool>();
            else warn(warnings_, m, "expected a boolean");
            break;
        case OptionType::Int:
            if (j->is_number()) {
                const double v = std::round(j->get<double>());
                *static_cast<std::int32_t*>(p) =
                    std::isnan(v) ? static_cast<std::int32_t>(m.defaultValue)
                                  : static_cast<std::int32_t>(std::fmax(-2147483648.0, std::fmin(2147483647.0, v)));
            } else warn(warnings_, m, "expected a number");
            break;
        case OptionType::Float:
            if (j->is_number()) {
                const double v = j->get<double>();
                *static_cast<float*>(p) =
                    std::isnan(v)
                        ? static_cast<float>(m.defaultValue)
                        : static_cast<float>(std::fmax(-3.4028234663852886e38, std::fmin(3.4028234663852886e38, v)));
            } else warn(warnings_, m, "expected a number");
            break;
        case OptionType::Double:
            if (j->is_number()) *static_cast<double*>(p) = j->get<double>();
            else warn(warnings_, m, "expected a number");
            break;
        case OptionType::String:
            if (j->is_string()) *static_cast<std::string*>(p) = j->get<std::string>();
            else warn(warnings_, m, "expected a string");
            break;
        case OptionType::Enum: {
            int ordinal = -1;
            if (j->is_string()) {
                const std::string name = j->get<std::string>();
                for (int k = 0; k < m.enumCount; ++k) {
                    if (enumNameEquals(name, m.enumNames[k])) ordinal = k;
                }
            } else if (j->is_number_integer()) {
                const long long v = j->get<long long>();
                if (v >= 0 && v < m.enumCount) ordinal = static_cast<int>(v);
            }
            if (ordinal >= 0) *static_cast<std::uint8_t*>(p) = static_cast<std::uint8_t>(ordinal);
            else warn(warnings_, m, "unknown enum value");
            break;
        }
        }
    }

    // unsupported data compression (LZ4, ...) is reported, then reset by clampToRange
    if (!isSupported(loaded.common.lodBuilding.dataCompression)) {
        warnings_.push_back("common.lodBuilding.dataCompression: unsupported mode, using default");
    }
    clampToRange(loaded, cores_);
    options_ = loaded;
    notify();
    return std::nullopt;
}

std::string ConfigStore::save() const {
    Json root               = Json::object();
    root["version"]         = kVersion;
    const OptionMeta* table = optionMeta();
    for (std::size_t i = 0; i < optionMetaCount(); ++i) {
        const OptionMeta& m = table[i];
        const void*       p = m.constPtr(options_);
        Json&             s = slotFor(root, m.path);
        switch (m.type) {
        case OptionType::Bool:
            s = *static_cast<const bool*>(p);
            break;
        case OptionType::Int:
            s = static_cast<long long>(*static_cast<const std::int32_t*>(p));
            break;
        case OptionType::Float:
            s = shortFloat(*static_cast<const float*>(p));
            break;
        case OptionType::Double:
            s = *static_cast<const double*>(p);
            break;
        case OptionType::String:
            s = *static_cast<const std::string*>(p);
            break;
        case OptionType::Enum: {
            const int e = *static_cast<const std::uint8_t*>(p);
            s           = (e >= 0 && e < m.enumCount) ? std::string(m.enumNames[e])
                                                      : std::string(m.enumNames[static_cast<int>(m.defaultValue)]);
            break;
        }
        }
    }
    // Sibling "<name>_doc" keys carry the option description into the file (ignored on load).
    for (std::size_t i = 0; i < optionMetaCount(); ++i) {
        const OptionMeta& m = table[i];
        if (m.comment == nullptr || m.comment[0] == '\0') continue;
        std::string doc = m.comment;
        if (m.type == OptionType::Enum && m.enumCount > 0) {
            doc += " Values: ";
            for (int k = 0; k < m.enumCount; ++k) {
                if (k > 0) doc += '|';
                doc += m.enumNames[k];
            }
            doc += '.';
        }
        slotFor(root, (std::string(m.path) + "_doc").c_str()) = doc;
    }
    // dump() with replace error handler: never throws on invalid UTF-8 in user strings
    return root.dump(2, ' ', false, Json::error_handler_t::replace);
}

void ConfigStore::onChange(std::function<void(const Options&)> listener) {
    if (listener) listeners_.push_back(std::move(listener));
}

void ConfigStore::set(const std::function<void(Options&)>& mutate) {
    if (mutate) mutate(options_);
    notify();
}

void ConfigStore::set(const Options& value) {
    options_ = value;
    notify();
}

void ConfigStore::notify() {
    clampToRange(options_, cores_);
    for (std::size_t i = 0; i < listeners_.size(); ++i) {
        // copy: a listener may register further listeners (vector reallocation)
        auto fn = listeners_[i];
        fn(options_);
    }
}

} // namespace dh
