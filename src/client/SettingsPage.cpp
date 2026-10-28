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

/// An optional header followed by the options whose path equals `prefix` or starts with `prefix` + '.'. Options
/// already placed by an earlier group (sections are filled in order) are skipped, which is how "General" takes the
/// render distance out of the quality group.
struct Group {
    const char* header;
    const char* subtitle;
    const char* prefix;
};

/// One titled section of the mod's settings page.
struct SectionSpec {
    const char*        id;
    const char*        title;
    std::vector<Group> groups;
};

const std::vector<SectionSpec>& sections() {
    static const std::vector<SectionSpec> kSections = {
        {"general",
         "General",
         {
             {nullptr, nullptr, "client.enableRendering"},
             {nullptr, nullptr, "client.graphics.quality.lodChunkRenderDistanceRadius"},
             {nullptr, nullptr, "client.keys"},
         }},
        {"graphics",
         "Graphics",
         {
             {"Quality", "Detail of the LODs (the quality preset above sets most of these)", "client.graphics.quality"},
             {nullptr, nullptr, "client.graphics.enableSsao"},
             {nullptr, nullptr, "client.graphics.enableAntiAliasing"},
             {nullptr, nullptr, "client.graphics.overrideVanillaGraphicsSettings"},
             {"Fog", "DH fog and the hand-over from the vanilla fog", "client.graphics.fog"},
             {"Clouds", "The LOD cloud layer", "client.graphics.clouds"},
         }},
        {"culling", "Culling", {{nullptr, nullptr, "client.graphics.culling"}}},
        {"worldgen", "World generation", {{nullptr, nullptr, "common.worldGenerator"}}},
        {"threads", "Threads", {{nullptr, nullptr, "common.multiThreading"}}},
        {"advanced",
         "Advanced",
         {
             {"Rendering internals", "The verified defaults; change only for troubleshooting", "client.advanced"},
             {"Debug", "Overlay, debug keys and extra log lines", "client.debug"},
             {"LOD building", nullptr, "common.lodBuilding"},
         }},
    };
    return kSections;
}

/// Rows that only matter while a switch is on: `prefix` rows (other than the switch itself) are greyed out, or
/// hidden when `hide`, while the switch `gate` is off.
struct Gate {
    const char* gate;
    const char* prefix;
    bool        hide;
};

const Gate kGates[] = {
    {"client.graphics.fog.enableDhFog", "client.graphics.fog", false},
    {"client.graphics.clouds.renderDhClouds", "client.graphics.clouds", false},
    {"client.graphics.culling.enableCaveCulling", "client.graphics.culling.caveCullingHeight", true},
    {"client.graphics.quality.increaseQualityWhenZoomedIn", "client.graphics.quality.maxZoomQualityIncrease", true},
    {"client.debug.enabled", "client.debug", false},
};

const OptionMeta* findMeta(std::string_view path) {
    const OptionMeta* meta = optionMeta();
    for (std::size_t i = 0; i < optionMetaCount(); ++i)
        if (meta[i].path == path) return &meta[i];
    return nullptr;
}

/// UI-only: show the manual thread values instead of the preset dropdown. Starts as "manual" when the stored values
/// match no preset.
bool gManualThreads = false;

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
std::string sectionOf(std::string_view path);

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

/// Adds the row for one option. Numbers with a usable range become sliders, unbounded ones text fields.
lever::Widget& addOption(lever::Section& sec, const OptionMeta& m) {
    const std::string id    = settingId(m.path);
    const std::string label = labelOf(m.path);
    const std::string desc  = m.comment ? m.comment : "";
    switch (m.type) {
    case OptionType::Bool:
        return sec.toggle(id, label, [&m] { return readOption<bool>(m); }, [&m](bool v) { writeOption<bool>(m, v); })
            .description(desc);
    case OptionType::Enum: {
        std::vector<std::string> names;
        for (int i = 0; i < m.enumCount; ++i) names.push_back(enumLabel(m.enumNames[i]));
        return sec
            .dropdown(
                id,
                label,
                std::move(names),
                [&m] { return static_cast<int>(readOption<std::uint8_t>(m)); },
                [&m](int v) {
                    if (v >= 0 && v < m.enumCount) writeOption<std::uint8_t>(m, static_cast<std::uint8_t>(v));
                }
            )
            .description(desc);
    }
    case OptionType::String:
        return sec
            .textField(
                id,
                label,
                [&m] { return readOption<std::string>(m); },
                [&m](const std::string& v) { writeOption<std::string>(m, v); },
                1024
            )
            .description(desc);
    case OptionType::Int:
    case OptionType::Float:
    case OptionType::Double: {
        double min = m.minValue, max = m.maxValue;
        if (m.maxIsCoreCount) max = store().cores();
        const bool   bounded = std::isfinite(min) && std::isfinite(max) && min >= -1.0e6 && max <= 1.0e6 && max > min;
        const double step    = bounded ? niceStep(max - min, m.type == OptionType::Int) : 0;
        if (step > 0) {
            return sec.slider(id, label, min, max, step, [&m] { return readNumber(m); }, [&m](double v) { writeNumber(m, v); })
                .description(desc);
        }
        const std::string sectionId = sectionOf(m.path);
        return sec
            .textField(
                id,
                label,
                [&m] { return formatNumber(m, readNumber(m)); },
                [&m, id, sectionId](const std::string& v) {
                    char*        end = nullptr;
                    const double d   = std::strtod(v.c_str(), &end);
                    if (end != v.c_str() && std::isfinite(d)) writeNumber(m, d);
                    else lever::notifyChanged(kModId, sectionId, id); // invalid text: show the stored value again
                },
                32
            )
            .description(desc.empty() ? std::string("A number.") : desc + " (a number)");
    }
    }
    return sec.header(id, label, desc); // unreachable: every OptionType is handled above
}

/// Greys out or hides a row whose switch (kGates) is off; the switch row itself is left alone.
void applyGates(lever::Widget& w, const OptionMeta& m) {
    for (const Gate& g : kGates) {
        if (!matches(m.path, g.prefix) || std::strcmp(m.path, g.gate) == 0) continue;
        const OptionMeta* gate = findMeta(g.gate);
        if (!gate || gate->type != OptionType::Bool) continue;
        lever::Condition on = [gate] { return readOption<bool>(*gate); };
        if (g.hide) w.visibleWhen(std::move(on));
        else w.enabledWhen(std::move(on));
    }
}

/// The preset dropdowns are not options: they derive from and write several options at once. A preset touches rows
/// in other sections, so the whole mod page is told to re-read.
void addQualityPreset(lever::Section& sec) {
    static const char* const kNames[] = {"Custom", "Minimum", "Low", "Medium", "High", "Extreme"};
    sec.dropdown(
           "qualityPreset",
           "Quality preset",
           std::vector<std::string>(std::begin(kNames), std::end(kNames)),
           [] { return static_cast<int>(detectQualityPreset(store().options())); },
           [](int v) {
               if (v <= 0 || v > static_cast<int>(QualityPreset::Extreme)) return;
               store().set([v](Options& o) { applyQualityPreset(o, static_cast<QualityPreset>(v)); });
               lever::notifyChanged(kModId);
           }
    )
        .description(
            "Sets the resolution, vertical quality, transparency, SSAO, anti-aliasing, fade, cave culling and biome "
            "blending options in the Graphics and Culling sections together. Custom = the current values match no preset."
        );
}

void addThreadPreset(lever::Section& sec) {
    static const char* const kNames[] =
        {"Custom", "Minimal impact", "Low impact", "Balanced", "Aggressive", "I paid for the whole CPU"};
    sec.toggle("manualThreads", "Manual thread settings", [] { return gManualThreads; }, [](bool v) { gManualThreads = v; })
        .description("Off: pick a preset. On: set the number of worker threads and their run-time ratio yourself.");
    sec.dropdown(
           "threadPreset",
           "Thread preset",
           std::vector<std::string>(std::begin(kNames), std::end(kNames)),
           [] { return static_cast<int>(detectThreadPreset(store().options(), store().cores())); },
           [](int v) {
               if (v <= 0 || v > static_cast<int>(ThreadPreset::IPaidForTheWholeCpu)) return;
               store().set([v](Options& o) { applyThreadPreset(o, static_cast<ThreadPreset>(v), store().cores()); });
               lever::notifyChanged(kModId);
           }
    )
        .description("Sets the number of worker threads and their run-time ratio together. Custom = the current values match no preset.")
        .visibleWhen([] { return !gManualThreads; });
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

std::string sectionOf(std::string_view path) {
    for (const SectionSpec& spec : sections())
        for (const Group& g : spec.groups)
            if (matches(path, g.prefix)) return spec.id;
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
    lever::Mod& mod = lever::registerMod(info);
    if (!mod.valid()) {
        DH_LOG_W("settings: Lever refused the mod registration; no settings page");
        return;
    }
    gManualThreads = detectThreadPreset(store().options(), store().cores()) == ThreadPreset::Custom;
    const OptionMeta*               meta  = optionMeta();
    const std::size_t               count = optionMetaCount();
    std::unordered_set<std::string> placed;
    int                             widgets = 0;
    for (const SectionSpec& spec : sections()) {
        lever::Section& sec       = mod.section(spec.id, spec.title);
        const bool      isGeneral = std::strcmp(spec.id, "general") == 0;
        const bool      isThreads = std::strcmp(spec.id, "threads") == 0;
        if (isGeneral) {
            sec.banner(
                "apply",
                "Rendering options apply at once; quality, culling and generation options apply when you next join a world.",
                lever::BannerKind::Info
            );
        }
        if (isThreads) {
            addThreadPreset(sec);
            widgets += 2;
        }
        for (const Group& g : spec.groups) {
            if (g.header) sec.header(std::string("h-") + g.prefix, g.header, g.subtitle ? g.subtitle : "");
            for (std::size_t i = 0; i < count; ++i) {
                const OptionMeta& m = meta[i];
                if (!matches(m.path, g.prefix) || placed.count(m.path)) continue;
                placed.insert(m.path);
                lever::Widget& w = addOption(sec, m);
                applyGates(w, m);
                if (isThreads) w.visibleWhen([] { return gManualThreads; });
                ++widgets;
            }
        }
        if (isGeneral) {
            addQualityPreset(sec);
            ++widgets;
        }
    }
    int unplaced = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (!placed.count(meta[i].path) && std::strncmp(meta[i].path, "server.", 7) != 0) ++unplaced;
    DH_LOG_I(
        "settings: %d widgets in %zu Lever sections (%d client/common options without a section), Lever %s",
        widgets,
        sections().size(),
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
    for (const Gate& g : kGates)
        if (std::strcmp(optionPath, g.gate) == 0) { // a switch: its dependent rows re-evaluate too
            lever::notifyChanged(kModId);
            return;
        }
    const std::string section = sectionOf(optionPath);
    if (!section.empty()) lever::notifyChanged(kModId, section, settingId(optionPath));
}

} // namespace dh::settings
