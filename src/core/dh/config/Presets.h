#pragma once

// Ports of RenderQualityPresetConfigEventHandler and ThreadPresetConfigEventHandler
// (preset -> value tables) and AbstractPresetConfigEventHandler.getCurrentPreset (detection).

#include "dh/config/Options.h"

namespace dh {

/// Set the 10 options controlled by the quality preset (maxHorizontalResolution, verticalQuality,
/// horizontalQuality, transparency, enableSsao, enableAntiAliasing, vanillaFadeMode, ditherDhFade,
/// enableCaveCulling, lodBiomeBlending). Custom is a no-op.
void applyQualityPreset(Options& o, QualityPreset preset);

/// The preset whose table matches every controlled option, else Custom. Java iterates a HashSet
/// when several match (never the case for the quality table); here the first in enum order wins.
QualityPreset detectQualityPreset(const Options& o);

/// Set numberOfThreads and threadRunTimeRatio. `cores` = logical processor count
/// (Runtime.availableProcessors / std::thread::hardware_concurrency). Custom is a no-op.
void applyThreadPreset(Options& o, ThreadPreset preset, int cores);

/// The preset matching both options, else Custom. With few cores several presets can map to the
/// same values (e.g. 1 core); the first in enum order wins.
ThreadPreset detectThreadPreset(const Options& o, int cores);

/// Java ThreadPresetConfigEventHandler value tables, exposed for tests / UI.
int    threadPresetThreadCount(ThreadPreset preset, int cores); ///< Custom -> 0
double threadPresetRunTimeRatio(ThreadPreset preset);           ///< Custom -> 0.0

} // namespace dh
