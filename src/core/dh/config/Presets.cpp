#include "dh/config/Presets.h"

#include <cstddef>

namespace dh {

namespace {

// Index = QualityPreset ordinal; index 0 (Custom) is unused.
struct QualityRow {
    MaxHorizontalResolution maxRes;
    VerticalQuality         vertical;
    HorizontalQuality       horizontal;
    Transparency            transparency;
    bool                    ssao;
    bool                    antiAliasing;
    VanillaFadeMode         fade;
    bool                    dither;
    bool                    caveCulling;
    int                     biomeBlending;
};

constexpr QualityRow kQuality[] = {
    /* Custom  */ {
                   MaxHorizontalResolution::Block,
                   VerticalQuality::Medium,
                   HorizontalQuality::Medium,
                   Transparency::Complete,
                   true,  true,
                   VanillaFadeMode::DoublePass,
                   true,  true,
                   3
    },
    /* MINIMUM */
    {MaxHorizontalResolution::TwoBlocks,
                   VerticalQuality::HeightMap,
                   HorizontalQuality::Lowest,
                   Transparency::Disabled,
                   false, false,
                   VanillaFadeMode::None,
                   false, true,
                   0},
    /* LOW     */
    {MaxHorizontalResolution::Block,
                   VerticalQuality::Low,
                   HorizontalQuality::Low,
                   Transparency::Disabled,
                   false, true,
                   VanillaFadeMode::SinglePass,
                   true,  true,
                   1},
    /* MEDIUM  */
    {MaxHorizontalResolution::Block,
                   VerticalQuality::Medium,
                   HorizontalQuality::Medium,
                   Transparency::Complete,
                   true,  true,
                   VanillaFadeMode::DoublePass,
                   true,  true,
                   3},
    /* HIGH    */
    {MaxHorizontalResolution::Block,
                   VerticalQuality::High,
                   HorizontalQuality::High,
                   Transparency::Complete,
                   true,  true,
                   VanillaFadeMode::DoublePass,
                   true,  false,
                   3},
    /* EXTREME */
    {MaxHorizontalResolution::Block,
                   VerticalQuality::Extreme,
                   HorizontalQuality::Extreme,
                   Transparency::Complete,
                   true,  true,
                   VanillaFadeMode::DoublePass,
                   true,  false,
                   3},
};

constexpr double kThreadPercent[] = {0.0, 0.1, 0.25, 0.5, 0.75, 1.0};
constexpr double kThreadRatio[]   = {0.0, 0.5, 1.0, 1.0, 1.0, 1.0};

bool matches(const Options& o, const QualityRow& r) {
    const QualityOptions& q = o.client.graphics.quality;
    return q.maxHorizontalResolution == r.maxRes && q.verticalQuality == r.vertical
        && q.horizontalQuality == r.horizontal && q.transparency == r.transparency
        && o.client.graphics.enableSsao == r.ssao && o.client.graphics.enableAntiAliasing == r.antiAliasing
        && q.vanillaFadeMode == r.fade && q.ditherDhFade == r.dither
        && o.client.graphics.culling.enableCaveCulling == r.caveCulling && q.lodBiomeBlending == r.biomeBlending;
}

} // namespace

void applyQualityPreset(Options& o, QualityPreset preset) {
    if (preset == QualityPreset::Custom) return;
    const QualityRow& r                         = kQuality[static_cast<std::size_t>(preset)];
    QualityOptions&   q                         = o.client.graphics.quality;
    q.maxHorizontalResolution                   = r.maxRes;
    q.verticalQuality                           = r.vertical;
    q.horizontalQuality                         = r.horizontal;
    q.transparency                              = r.transparency;
    o.client.graphics.enableSsao                = r.ssao;
    o.client.graphics.enableAntiAliasing        = r.antiAliasing;
    q.vanillaFadeMode                           = r.fade;
    q.ditherDhFade                              = r.dither;
    o.client.graphics.culling.enableCaveCulling = r.caveCulling;
    q.lodBiomeBlending                          = r.biomeBlending;
}

QualityPreset detectQualityPreset(const Options& o) {
    for (std::size_t i = 1; i < std::size(kQuality); ++i) {
        if (matches(o, kQuality[i])) return static_cast<QualityPreset>(i);
    }
    return QualityPreset::Custom;
}

int threadPresetThreadCount(ThreadPreset preset, int cores) {
    if (preset == ThreadPreset::Custom) return 0;
    return threadCountByPercent(kThreadPercent[static_cast<std::size_t>(preset)], cores);
}

double threadPresetRunTimeRatio(ThreadPreset preset) { return kThreadRatio[static_cast<std::size_t>(preset)]; }

void applyThreadPreset(Options& o, ThreadPreset preset, int cores) {
    if (preset == ThreadPreset::Custom) return;
    o.common.multiThreading.numberOfThreads    = threadPresetThreadCount(preset, cores);
    o.common.multiThreading.threadRunTimeRatio = threadPresetRunTimeRatio(preset);
}

ThreadPreset detectThreadPreset(const Options& o, int cores) {
    for (std::size_t i = 1; i <= static_cast<std::size_t>(ThreadPreset::IPaidForTheWholeCpu); ++i) {
        const auto p = static_cast<ThreadPreset>(i);
        if (o.common.multiThreading.numberOfThreads == threadPresetThreadCount(p, cores)
            && o.common.multiThreading.threadRunTimeRatio == threadPresetRunTimeRatio(p)) {
            return p;
        }
    }
    return ThreadPreset::Custom;
}

} // namespace dh
