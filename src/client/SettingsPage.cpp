#include "client/SettingsPage.h"

#include "bedrock_common/ConfigBridge.h"
#include "bedrock_common/FileLogger.h"
#include "client/Version.h"
#include "client/world/ClientWorldDriver.h"

#include "dh/config/Options.h"
#include "dh/config/Presets.h"

#include "lever/Lever.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dh::settings {

namespace {

constexpr const char* kModId = "distanthorizons";

/// A header on a page followed by the options whose path equals `prefix` or starts with `prefix` + '.'. Options
/// already placed by an earlier section (pages are filled in order) are skipped, which is how "General" takes the
/// render distance out of the quality group.
struct Section {
    const char* header;
    const char* subtitle;
    const char* prefix;
};

struct PageSpec {
    const char*          id;
    const char*          title;
    std::vector<Section> sections;
};

const std::vector<PageSpec>& pages() {
    static const std::vector<PageSpec> kPages = {
        {"general",
         "General",
         {
             {"Rendering", "The master switch and the options that matter most", "client.enableRendering"},
             {nullptr, nullptr, "client.graphics.quality.lodChunkRenderDistanceRadius"},
             {nullptr, nullptr, "client.keys"},
         }},
        {"graphics",
         "Graphics",
         {
             {"Quality", "Detail of the LODs (the quality preset on the General page sets most of these)", "client.graphics.quality"},
             {nullptr, nullptr, "client.graphics.enableSsao"},
             {nullptr, nullptr, "client.graphics.enableAntiAliasing"},
             {nullptr, nullptr, "client.graphics.overrideVanillaGraphicsSettings"},
             {"Fog", "DH fog and the hand-over from the vanilla fog", "client.graphics.fog"},
             {"Clouds", "The LOD cloud layer", "client.graphics.clouds"},
         }},
        {"culling", "Culling", {{"Culling", "What is skipped when LODs are built and drawn", "client.graphics.culling"}}},
        {"worldgen",
         "World generation",
         {{"Distant generation", "Generating chunks beyond the loaded ones (singleplayer)", "common.worldGenerator"}}},
        {"advanced",
         "Advanced",
         {
             {"Rendering internals", "The verified defaults; change only for troubleshooting", "client.advanced"},
             {"Debug", "Overlay, debug keys and extra log lines", "client.debug"},
             {"LOD building", nullptr, "common.lodBuilding"},
             {"Threads", "The thread preset above sets these", "common.multiThreading"},
         }},
    };
    return kPages;
}

bool matches(std::string_view path, std::string_view prefix) {
    if (path == prefix) return true;
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '.';
}

/// Setting id for an option path: the path with dots replaced ("client-graphics-fog-enableDhFog").
std::string settingId(std::string_view path) {
    std::string id(path);
    for (char& c : id)
        if (c == '.') c = '-';
    return id;
}

/// "lodChunkRenderDistanceRadius" -> "LOD chunk render distance radius".
std::string labelOf(std::string_view path) {
    const auto       dot  = path.rfind('.');
    std::string_view last = dot == std::string_view::npos ? path : path.substr(dot + 1);
    std::string      out;
    std::string      word;
    auto             flush = [&] {
        if (word.empty()) return;
        std::string lower, upper;
        for (char c : word) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        static const char* const kAcronyms[] = {"lod", "dh", "ssao", "csv", "hud", "mc", "x", "z"};
        bool                     acronym     = false;
        for (const char* a : kAcronyms)
            if (lower == a) acronym = true;
        if (!out.empty()) out += ' ';
        out += acronym ? upper : lower;
        word.clear();
    };
    for (char c : last) {
        if (std::isupper(static_cast<unsigned char>(c)) && !word.empty()) flush();
        word += c;
    }
    flush();
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

/// "FADE_TO_DIRT" -> "Fade to dirt".
std::string enumLabel(const char* javaName) {
    std::string out;
    for (const char* p = javaName; *p; ++p) {
        if (*p == '_') out += ' ';
        else out += out.empty() ? *p : static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    return out;
}

dh::ConfigStore& store() { return bedrock::ConfigBridge::instance().config(); }

/// The page an option path is placed on ("" when none).
std::string pageOf(std::string_view path);

template <class T>
T readOption(const OptionMeta& m) {
    return *static_cast<const T*>(m.constPtr(store().options()));
}

template <class T>
void writeOption(const OptionMeta& m, T value) {
    store().set([&](Options& o) { *static_cast<T*>(m.mutablePtr(o)) = value; });
}

double readNumber(const OptionMeta& m) {
    switch (m.type) {
    case OptionType::Int: return readOption<int>(m);
    case OptionType::Float: return readOption<float>(m);
    case OptionType::Double: return readOption<double>(m);
    default: return 0;
    }
}

void writeNumber(const OptionMeta& m, double v) {
    switch (m.type) {
    case OptionType::Int:
        v = std::fmax(std::fmin(v, 2147483647.0), -2147483648.0); // lround on a huge text value is unspecified
        writeOption<int>(m, static_cast<int>(std::lround(v)));
        break;
    case OptionType::Float: writeOption<float>(m, static_cast<float>(v)); break;
    case OptionType::Double: writeOption<double>(m, v); break;
    default: break;
    }
}

/// Slider step for a range: the finest round step that divides the range exactly and gives at most 512 positions,
/// so every position is a round value and the ends are reachable. 0 when no such step exists (-> text field).
double niceStep(double range, bool integer) {
    static const double kSteps[] = {0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2, 4, 5, 8, 10, 16, 20, 25, 32, 50, 64, 100, 128, 200, 250, 256, 500, 1000};
    for (double s : kSteps) {
        if (integer && s < 1) continue;
        const double n = range / s;
        if (n > 512.0) continue;
        if (std::fabs(n - std::round(n)) < 1e-6) return s;
    }
    return 0;
}

std::string formatNumber(const OptionMeta& m, double v) {
    char buf[64];
    if (m.type == OptionType::Int) std::snprintf(buf, sizeof buf, "%d", static_cast<int>(std::lround(v)));
    else std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

/// Adds the widget for one option. Numbers with a usable range become sliders, unbounded ones text fields.
void addOption(lever::Page& page, const OptionMeta& m) {
    const std::string id    = settingId(m.path);
    const std::string label = labelOf(m.path);
    const std::string desc  = m.comment ? m.comment : "";
    switch (m.type) {
    case OptionType::Bool:
        page.addToggle(id, label, desc, [&m] { return readOption<bool>(m); }, [&m](bool v) { writeOption<bool>(m, v); });
        break;
    case OptionType::Enum: {
        std::vector<std::string> names;
        for (int i = 0; i < m.enumCount; ++i) names.push_back(enumLabel(m.enumNames[i]));
        page.addDropdown(
            id,
            label,
            desc,
            std::move(names),
            [&m] { return static_cast<int>(readOption<std::uint8_t>(m)); },
            [&m](int v) {
                if (v >= 0 && v < m.enumCount) writeOption<std::uint8_t>(m, static_cast<std::uint8_t>(v));
            }
        );
        break;
    }
    case OptionType::String:
        page.addTextField(
            id,
            label,
            desc,
            [&m] { return readOption<std::string>(m); },
            [&m](const std::string& v) { writeOption<std::string>(m, v); },
            1024
        );
        break;
    case OptionType::Int:
    case OptionType::Float:
    case OptionType::Double: {
        double min = m.minValue, max = m.maxValue;
        if (m.maxIsCoreCount) max = store().cores();
        const bool   bounded = std::isfinite(min) && std::isfinite(max) && min >= -1.0e6 && max <= 1.0e6 && max > min;
        const double step    = bounded ? niceStep(max - min, m.type == OptionType::Int) : 0;
        if (step > 0) {
            page.addSlider(id, label, desc, min, max, step, [&m] { return readNumber(m); }, [&m](double v) { writeNumber(m, v); });
        } else {
            const std::string pageId = pageOf(m.path);
            page.addTextField(
                id,
                label,
                desc.empty() ? std::string("A number.") : desc + " (a number)",
                [&m] { return formatNumber(m, readNumber(m)); },
                [&m, id, pageId](const std::string& v) {
                    char*        end = nullptr;
                    const double d   = std::strtod(v.c_str(), &end);
                    if (end != v.c_str() && std::isfinite(d)) writeNumber(m, d);
                    else lever::notifyChanged(kModId, pageId, id); // invalid text: show the stored value again
                },
                32
            );
        }
        break;
    }
    }
}

/// The preset dropdowns are not options: they derive from and write several options at once.
void addQualityPreset(lever::Page& page) {
    static const char* const kNames[] = {"Custom", "Minimum", "Low", "Medium", "High", "Extreme"};
    page.addDropdown(
        "qualityPreset",
        "Quality preset",
        "Sets the resolution, vertical quality, transparency, SSAO, anti-aliasing, fade, cave culling and biome blending "
        "options together. Custom = the current values match no preset.",
        std::vector<std::string>(std::begin(kNames), std::end(kNames)),
        [] { return static_cast<int>(detectQualityPreset(store().options())); },
        [](int v) {
            if (v <= 0 || v > static_cast<int>(QualityPreset::Extreme)) return;
            store().set([v](Options& o) { applyQualityPreset(o, static_cast<QualityPreset>(v)); });
            lever::notifyChanged(kModId); // touches Graphics, Culling and this dropdown
        }
    );
}

void addThreadPreset(lever::Page& page) {
    static const char* const kNames[] =
        {"Custom", "Minimal impact", "Low impact", "Balanced", "Aggressive", "I paid for the whole CPU"};
    page.addDropdown(
        "threadPreset",
        "Thread preset",
        "Sets the number of worker threads and their run-time ratio together.",
        std::vector<std::string>(std::begin(kNames), std::end(kNames)),
        [] { return static_cast<int>(detectThreadPreset(store().options(), store().cores())); },
        [](int v) {
            if (v <= 0 || v > static_cast<int>(ThreadPreset::IPaidForTheWholeCpu)) return;
            store().set([v](Options& o) { applyThreadPreset(o, static_cast<ThreadPreset>(v), store().cores()); });
            lever::notifyChanged(kModId);
        }
    );
}

/// "0.1.0-beta.1-mc26.51" -> {0, 1, 0}.
lever::Version modVersion() {
    unsigned    parts[3] = {0, 0, 0};
    const char* p        = kModVersion;
    for (unsigned& part : parts) {
        char* end = nullptr;
        part      = static_cast<unsigned>(std::strtoul(p, &end, 10));
        if (end == p || *end != '.') break;
        p = end + 1;
    }
    return {parts[0], parts[1], parts[2]};
}

bool gInstalled = false;

std::string pageOf(std::string_view path) {
    for (const PageSpec& spec : pages())
        for (const Section& s : spec.sections)
            if (matches(path, s.prefix)) return spec.id;
    return {};
}

} // namespace

void install() {
    if (gInstalled) return;
    gInstalled = true;
    lever::ModInfo info;
    info.id          = kModId;
    info.displayName = "Distant Horizons";
    info.version     = modVersion();
    info.author      = "Distant Horizons Bedrock";
    info.description = std::string("Level-of-detail terrain beyond the vanilla render distance. Build ") + kGitSha
                     + ". Rendering options apply at once; quality, culling and generation options apply when you next "
                       "join a world.";
    if (!lever::registerMod(info)) {
        DH_LOG_W("settings: Lever refused the mod registration; no settings pages");
        return;
    }
    const OptionMeta*               meta  = optionMeta();
    const std::size_t               count = optionMetaCount();
    std::unordered_set<std::string> placed;
    int                             widgets = 0;
    for (const PageSpec& spec : pages()) {
        lever::Page& page = lever::addPage(kModId, spec.id, spec.title);
        if (std::strcmp(spec.id, "general") == 0) {
            page.addBanner(
                "apply",
                "Rendering options apply at once; quality, culling and generation options apply when you next join a world.",
                lever::BannerKind::Info
            );
            page.addHeader("presets", "Presets", "Each one sets several options on the other pages");
            addQualityPreset(page);
            addThreadPreset(page);
            widgets += 2;
        }
        for (const Section& s : spec.sections) {
            if (s.header) page.addHeader(std::string("h-") + s.prefix, s.header, s.subtitle ? s.subtitle : "");
            for (std::size_t i = 0; i < count; ++i) {
                const OptionMeta& m = meta[i];
                if (!matches(m.path, s.prefix) || placed.count(m.path)) continue;
                placed.insert(m.path);
                addOption(page, m);
                ++widgets;
            }
        }
    }
    int unplaced = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (!placed.count(meta[i].path) && std::strncmp(meta[i].path, "server.", 7) != 0) ++unplaced;
    DH_LOG_I(
        "settings: %d widgets on %zu Lever pages (%d client/common options without a page), Lever %s",
        widgets,
        pages().size(),
        unplaced,
        std::string(lever::libraryVersion()).c_str()
    );
}

void uninstall() {
    if (!gInstalled) return;
    gInstalled = false;
    lever::unregisterMod(kModId);
}

void notifyChanged(const char* optionPath) {
    if (!gInstalled || !optionPath) return;
    const std::string page = pageOf(optionPath);
    if (!page.empty()) lever::notifyChanged(kModId, page, settingId(optionPath));
}

} // namespace dh::settings
