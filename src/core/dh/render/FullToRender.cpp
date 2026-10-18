#include "dh/render/FullToRender.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dh/fulldata/FullDataPoint.h"
#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"
#include "dh/render/ReducingList.h"
#include "dh/render/RenderDataPoint.h"
#include "dh/util/Bits.h"
#include "dh/util/Color.h"

namespace dh {

namespace {

constexpr int           BLOCK_FULLY_OPAQUE = 16;          // LodUtil.BLOCK_FULLY_OPAQUE
constexpr int           MIN_MC_LIGHT       = 0;           // LodUtil.MIN_MC_LIGHT
constexpr std::uint32_t NO_COLOR           = 0xFFFFFFFFu; // Java uses int -1 as the "no carried colour" sentinel

/// Java BROKEN_POS_SET (static, shared by all threads). True the first time a position is seen.
bool markBrokenPosOnce(std::uint64_t pos) {
    static std::mutex                        mutex;
    static std::unordered_set<std::uint64_t> seen;
    std::lock_guard<std::mutex>              lock(mutex);
    return seen.insert(pos).second;
}

} // namespace

bool isSnowLayerSerial(std::string_view serial) {
    // base name = everything before the state suffix
    std::size_t end = serial.size();
    for (std::string_view sep : {std::string_view("_STATE_"), std::string_view("[")}) {
        const std::size_t p = serial.find(sep);
        if (p != std::string_view::npos && p < end) end = p;
    }
    const std::string_view base = serial.substr(0, end);
    if (base != "minecraft:snow" && base != "minecraft:snow_layer") return false;

    const std::string_view props   = serial.substr(end);
    auto                   readInt = [&](std::string_view key) -> int {
        const std::size_t p = props.find(key);
        if (p == std::string_view::npos) return -1;
        std::size_t i = p + key.size();
        if (i >= props.size() || (props[i] != ':' && props[i] != '=')) return -1;
        ++i;
        int  v      = 0;
        bool digits = false;
        while (i < props.size() && props[i] >= '0' && props[i] <= '9' && v < 1000) {
            v      = v * 10 + (props[i] - '0');
            digits = true;
            ++i;
        }
        return digits ? v : -1;
    };

    int layers = readInt("layers");
    if (layers < 0 && base == "minecraft:snow_layer") {
        const int h = readInt("height"); // Bedrock: 0-based
        if (h >= 0) layers = h + 1;
    }
    return layers >= 1 && layers <= 3;
}

BlockSet resolveCsv(std::string_view csv, IBlockRegistry& registry, bool addAirForUnresolvable) {
    BlockSet out;
    while (!csv.empty()) {
        const std::size_t comma = csv.find(',');
        std::string_view  tok   = csv.substr(0, comma);
        csv                     = comma == std::string_view::npos ? std::string_view{} : csv.substr(comma + 1);
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.remove_prefix(1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t' || tok.back() == '\r')) tok.remove_suffix(1);
        if (tok.empty()) continue;
        const std::vector<const BlockInfo*> states = registry.resolveAllStates(tok);
        if (states.empty()) {
            if (addAirForUnresolvable) out.insert(registry.air()); // Java getAllBlockWrappers :1355-1359
        } else {
            out.insert(states.begin(), states.end());
        }
    }
    return out;
}

namespace {

struct Ctx {
    IClientLevel&                              level;
    IBlockRegistry&                            registry;
    const TransformOptions&                    opt;
    std::unordered_map<const BlockInfo*, bool> snowCache;

    static bool contains(const BlockSet* s, const BlockInfo* b) { return s != nullptr && s->count(b) != 0; }

    bool isSnowLayer(const BlockInfo* b) {
        auto it = snowCache.find(b);
        if (it != snowCache.end()) return it->second;
        const bool r = isSnowLayerSerial(b->serial);
        snowCache.emplace(b, r);
        return r;
    }
};

/// Java setVoidAir: lighting for the void below a column.
void setVoidAir(
    ColumnRenderView& view,
    int               renderDataIndex,
    int               bottomY,
    int               blockHeight,
    int               skyLight,
    int               blockLight
) {
    std::uint64_t lastDatapoint = rdp::EMPTY_DATA;
    if (renderDataIndex > 0) lastDatapoint = view.get(renderDataIndex - 1);

    int maxY = bottomY + blockHeight;
    if (lastDatapoint != rdp::EMPTY_DATA) maxY = rdp::yMin(lastDatapoint);

    view.setVoid(rdp::create(maxY, 0, color::INVISIBLE, skyLight, blockLight, BlockMaterial::Air));
}

void setRenderColumnView(
    Ctx&                              ctx,
    const FullDataSource&             src,
    int                               blockX,
    int                               blockZ,
    ColumnRenderView&                 view,
    const std::vector<std::uint64_t>& col
) {
    const TransformOptions& opt                         = ctx.opt;
    const bool              ignoreNonCollidingBlocks    = noCollision(opt.blocksToAvoid);
    const bool              colorBelowWithAvoidedBlocks = opt.tintWithAvoidedBlocks;
    const BlockInfo*        water                       = ctx.registry.water();

    const int  minHeight          = ctx.level.minHeight();
    const int  caveCullingMaxY    = opt.caveCullingHeight - minHeight;
    const bool caveCullingEnabled = opt.enableCaveCulling && (!ctx.level.hasCeiling() && !ctx.level.isEnd());

    bool isColumnVoid = true;

    std::uint32_t    lastColor  = 0;
    int              lastBottom = -10000;
    const BlockInfo* lastBlock  = nullptr;

    // there are several instances where the top datapoint is copied down onto the one below it
    std::uint32_t    colorToApplyToNextBlock      = NO_COLOR;
    const BlockInfo* blockToApplyToNextBlock      = nullptr;
    int              skylightToApplyToNextBlock   = -1;
    int              blocklightToApplyToNextBlock = -1;

    int renderDataIndex = 0;

    const int colSize = static_cast<int>(col.size());
    // goes from the top down
    for (int fullDataIndex = 0; fullDataIndex < colSize; fullDataIndex++) {
        const std::uint64_t fullData = col[static_cast<std::size_t>(fullDataIndex)];

        const int bottomY     = fdp::minY(fullData);
        int       blockHeight = fdp::height(fullData);
        const int topY        = bottomY + blockHeight;
        const int id          = fdp::id(fullData);
        int       blockLight  = fdp::blockLight(fullData);
        int       skyLight    = fdp::skyLight(fullData);

        // using the top Y provides better sampling for biomes (surface biome instead of cave biome)
        const int sampleY = topY - 1 + minHeight;

        const BiomeInfo* biome = src.mapping.biome(id);
        const BlockInfo* block = src.mapping.block(id);
        if (biome == nullptr || block == nullptr) {
            if (markBrokenPosOnce(src.mapping.sectionPos)) {
                log::logf(
                    LogLevel::Warn,
                    "Unable to get data point with id [%d] (Max possible ID: [%d]) for pos [%llu] in level [%.*s]. "
                    "Further errors for this position won't be logged.",
                    id,
                    src.mapping.maxValidId(),
                    static_cast<unsigned long long>(src.mapping.sectionPos),
                    static_cast<int>(ctx.level.dhIdentifier().size()),
                    ctx.level.dhIdentifier().data()
                );
            }
            continue; // don't render broken data
        }

        // special case: air gap leak fix (Java :425)
        if (block->isAir) {
            colorToApplyToNextBlock      = NO_COLOR;
            blockToApplyToNextBlock      = nullptr;
            skylightToApplyToNextBlock   = -1;
            blocklightToApplyToNextBlock = -1;
        }

        // special case: water under ice takes the ice (Java :443)
        if (block->material == BlockMaterial::Water && lastBlock != nullptr && lastBlock->isIce) {
            block = lastBlock;
        }

        // ignored block and cave culling check
        if (Ctx::contains(opt.waterSubSurfaceReplace, block) && (lastBlock == nullptr || lastBlock->isAir)) {
            block = water;
        }

        // Java pre-seeds both ignore sets with air (BlockStateWrapper.java:1154-1170)
        const bool ignoreBlock = block->isAir || Ctx::contains(opt.ignoredBlocks, block);
        const bool caveBlock   = block->isAir || Ctx::contains(opt.ignoredCaveBlocks, block);
        if (caveBlock || !block->isSolid || block->isLiquid || block->opacity < BLOCK_FULLY_OPAQUE) {
            if (caveCullingEnabled && skyLight == MIN_MC_LIGHT && topY < caveCullingMaxY && renderDataIndex != 0
                && fullDataIndex != 0 && (fullDataIndex + 1) < colSize) {
                // the air here always has light 0/0, so look at the next point's light
                const std::uint64_t nextFullData = col[static_cast<std::size_t>(fullDataIndex + 1)];
                const int           nextSkyLight = fdp::skyLight(nextFullData);

                if (nextSkyLight == MIN_MC_LIGHT && color::getAlpha(lastColor) == 255) {
                    view.set(renderDataIndex - 1, rdp::setYMin(view.get(renderDataIndex - 1), bottomY));
                }
                continue;
            }

            if (ignoreBlock) {
                // bottom datapoint: keep lighting for floating islands over the void
                if (fullDataIndex == colSize - 1) {
                    setVoidAir(view, renderDataIndex, bottomY, blockHeight, skyLight, blockLight);
                }
                continue;
            }
        } else if (ignoreBlock) {
            // ignored but not merged like a cave block; the sky light prevents black spots below
            skylightToApplyToNextBlock = skyLight;
            continue;
        }

        // non-solid block check
        bool ignoreNonSolidBlock =
            ignoreNonCollidingBlocks && !block->isSolid && !block->isLiquid && block->opacity != BLOCK_FULLY_OPAQUE;

        // height reduction (Java :310 detection, :557 blockHeight -= 1)
        const bool isSnowLayer               = ctx.isSnowLayer(block);
        const bool isWaterSurfaceReplacement = Ctx::contains(opt.waterSurfaceReplace, block);
        if (isSnowLayer || isWaterSurfaceReplacement) {
            if (isWaterSurfaceReplacement) block = water;

            blockHeight -= 1;
            if (blockHeight == 0) {
                // entirely removed: just colour the block below it
                ignoreNonSolidBlock = true;
                if (isSnowLayer) {
                    // snow always tints the block below, otherwise grass appears gray
                    const std::uint32_t snowColor = ctx.level.blockColor(blockX, sampleY, blockZ, biome, block);
                    colorToApplyToNextBlock       = color::setAlpha(snowColor, 255);
                    blockToApplyToNextBlock       = block;
                } else {
                    colorToApplyToNextBlock = ctx.level.blockColor(blockX, sampleY, blockZ, biome, block);
                }
            }
        }

        if (ignoreNonSolidBlock) {
            const std::uint32_t ignoredColor = ctx.level.blockColor(blockX, sampleY, blockZ, biome, block);
            const int           ignoredAlpha = color::getAlpha(ignoredColor);

            if (colorBelowWithAvoidedBlocks && ignoredAlpha != 0) {
                colorToApplyToNextBlock = color::setAlpha(ignoredColor, 255);
                blockToApplyToNextBlock = block;
            }
            // lighting is carried even without tinting, unless alpha is 0
            if (ignoredAlpha != 0) {
                skylightToApplyToNextBlock   = skyLight;
                blocklightToApplyToNextBlock = blockLight;
            }

            if (fullDataIndex == colSize - 1) {
                setVoidAir(view, renderDataIndex, bottomY, blockHeight, skyLight, blockLight);
            }
            continue; // skip this non-colliding block
        }

        std::uint32_t color;
        if (colorToApplyToNextBlock == NO_COLOR) {
            color = ctx.level.blockColor(blockX, sampleY, blockZ, biome, block);
        } else {
            color                   = colorToApplyToNextBlock;
            colorToApplyToNextBlock = NO_COLOR;
        }
        // Java applies the overrides identically in both branches
        if (skylightToApplyToNextBlock != -1) {
            skyLight                   = skylightToApplyToNextBlock;
            skylightToApplyToNextBlock = -1;
        }
        if (blocklightToApplyToNextBlock != -1) {
            blockLight                   = blocklightToApplyToNextBlock;
            blocklightToApplyToNextBlock = -1;
        }
        if (blockToApplyToNextBlock != nullptr) {
            block                   = blockToApplyToNextBlock;
            blockToApplyToNextBlock = nullptr;
        }

        // merge same-coloured adjacent datapoints (Java :691)
        if (color == lastColor && bottomY + blockHeight == lastBottom && renderDataIndex > 0) {
            view.set(renderDataIndex - 1, rdp::setYMin(view.get(renderDataIndex - 1), bottomY));
        } else {
            isColumnVoid = false;
            view.set(
                renderDataIndex,
                rdp::create(bottomY + blockHeight, bottomY, color, skyLight, blockLight, block->material)
            );
            renderDataIndex++;
        }
        lastBottom = bottomY;
        lastColor  = color;
        lastBlock  = block;
    }

    if (isColumnVoid) view.set(0, rdp::EMPTY_DATA);
}

} // namespace

std::optional<ColumnRenderSource> transformFullDataToRenderSource(
    const FullDataSource&   source,
    IClientLevel&           level,
    IBlockRegistry&         registry,
    const TransformOptions& options
) {
    const int dataDetail = sec::detail(source.pos) - sec::SECTION_MINIMUM_DETAIL_LEVEL;
    const int maxSlices  = maxVerticalData(options.verticalQuality, dataDetail);

    ColumnRenderSource out = ColumnRenderSource::createEmpty(source.pos, maxSlices, level.minHeight());
    if (source.isEmpty) return out;
    out.markNotEmpty();

    const int baseX = sec::minCornerBlockX(source.pos);
    const int baseZ = sec::minCornerBlockZ(source.pos);

    Ctx ctx{level, registry, options, {}};

    ColumnRenderView           columnView;
    ColumnRenderView           tempView;
    ReducingList               reducingList;
    std::vector<std::uint64_t> tempData, tempVoid(1);

    for (int x = 0; x < FullDataSource::WIDTH; x++) {
        for (int z = 0; z < FullDataSource::WIDTH; z++) {
            if (!out.populateColumnView(columnView, x, z)) return std::nullopt;
            const std::vector<std::uint64_t>& col = source.column(x, z);
            if (col.empty()) continue; // nothing to do for missing/empty full data

            // bit shift accounts for LODs with a detail level greater than 0
            const int blockX = baseX + jshl32(x, dataDetail);
            const int blockZ = baseZ + jshl32(z, dataDetail);

            const int fullLen = static_cast<int>(col.size());
            if (fullLen <= columnView.maxVerticalSliceCount) {
                setRenderColumnView(ctx, source, blockX, blockZ, columnView, col);
            } else {
                // expand to fit the full column, then reduce down into the real column
                tempData.assign(static_cast<std::size_t>(fullLen), rdp::EMPTY_DATA);
                tempVoid[0] = rdp::EMPTY_DATA;
                tempView.populate(tempData, tempVoid, fullLen, 0, 0, fullLen);
                setRenderColumnView(ctx, source, blockX, blockZ, tempView, col);
                if (!columnView.changeVerticalSizeFrom(tempView, reducingList)) return std::nullopt;
            }
            // TODO: applyTextureSetIds when the render source stores texture ids.
        }
    }
    return out;
}

} // namespace dh
