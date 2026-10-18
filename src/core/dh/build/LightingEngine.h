#pragma once

// Port of Java DhLightingEngine (lightChunk + bakeDataSourceSkyLight).
// Debug wireframes and light-pos pooling are intentionally not ported.

#include "dh/fulldata/FullDataSource.h"
#include "dh/platform/IChunk.h"

#include <span>

namespace dh {

struct LightingOptions {
    int  maxSkyLight        = 15; ///< 0..15; 0 disables sky seeds (Java: maxSkyLight > 0)
    bool generateBlockLight = true;
    bool generateSkyLight   = true;
};

/// Java DhLightingEngine.lightChunk: `center` plus up to 8 neighbours (any may be nullptr,
/// `center` may also appear in `nearby`; chunks outside the 3x3 are ignored, the first chunk
/// per position wins). Seeds are written into every chunk of the 3x3 (Java does this too),
/// then the center chunk's DH light is cleared and the light is propagated. Only chunks
/// inside the 3x3 are ever written.
///
/// Faithful quirk: because the center is cleared AFTER the seeds were stored, the center's own
/// seed cells start at 0 and are re-lit by propagation from the queued seeds (so they can end
/// up below the seed value).
void lightChunk(IChunk& center, std::span<IChunk* const> nearby, const LightingOptions& options);

/// Java DhLightingEngine.bakeDataSourceSkyLight.
void bakeDataSourceSkyLight(FullDataSource& dataSource, int maxSkyLight);

} // namespace dh
