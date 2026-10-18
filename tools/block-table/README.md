# block-table

Offline tool that precomputes the block and biome tables the Bedrock port needs. The Java mod derives these at
runtime from block models; Bedrock has no baked models, so we do it once here and ship `data/*.json`.

    python3 tools/block-table/generate.py --samples <bedrock-samples checkout>   # writes data/block_table.json, data/biome_table.json
    python3 tools/block-table/validate.py --samples <bedrock-samples checkout>   # coverage + sanity checks, non-zero exit on failure

Requires Python 3 and Pillow (`python3 -c "import PIL"`). Everything else is stdlib.

## Input source

The installed game's `resource_packs/vanilla` does NOT contain the block PNGs (`textures.brarchive` holds only
JSON/UI data, and the on-disk `blocks.json` is the legacy 341-entry file). So the tables are generated from
Mojang's public samples repo:

    git clone --depth 1 --branch v1.26.50.4 https://github.com/Mojang/bedrock-samples <dir>

`v1.26.50.4` is the newest non-preview tag at the time of writing; there is no `1.26.51` tag. Block textures, colormaps,
`blocks.json` (1353 flattened ids), `terrain_texture.json`, `biomes_client.json`, `resource_pack/biomes/*.client_biome.json`,
`behavior_pack/biomes/*.biome.json` and `metadata/vanilladata_modules/mojang-{blocks,biomes}.json` (the id lists) are read from it.
The output records the tag in `source.pack`.

## block_table.json

One entry per Bedrock block id (`minecraft:` prefix). The universe is `mojang-blocks.json` (1463 ids) plus the
legacy keys still present in `blocks.json` (`wool`, `planks`, ... flagged `"legacy": true`).

| field | how it is derived |
|---|---|
| `color`, `faceColors` | `blocks.json` -> `terrain_texture.json` (variant 0) -> PNG. Port of Java `calculateColorFromTexture`: linear-space, alpha-weighted average; Leaves mode forces alpha 255; Flower mode weights saturated pixels x5; Glass mode counts alpha-0 pixels with zero weight; flipbooks use frame 0; magenta missing-texture -> `#FFFFFF00`. `color` uses Java face order UP, N, E, W, S, DOWN, but pillars (logs, stems, basalt...) skip UP so the bark colour wins. `side` = north face. |
| `tint` | name globs in `overrides.json` (`grass`, `foliage`, `dry_foliage`, `birch`, `evergreen`, `water`, else `none`). `color` is the **untinted** (grey) average; multiply by the biome tint at runtime on the faces in `tintFaces` (grass_block: top only). `birch`/`evergreen` are extensions of the brief: Java uses fixed colours, Bedrock ships colormaps, so the biome table carries `birchTint`/`evergreenTint`. |
| `material` | Mirrors Java `BlockStateWrapper.calculateEDhApiBlockMaterialId`, same rule order. Java's `SoundType` checks use the Bedrock `sound` field of `blocks.json` (wood/cherry_wood -> Wood; metal/iron/copper* -> Metal; deepslate* -> DeepSlate). Name rules (`contains("sand")`, `"stone"`, `"ore"`, `"mud"`, `"root"`, `"bamboo"`...) are copied verbatim, quirks included (e.g. `bamboo_planks` is Leaves, `beetroot` is Wood). `Illuminated` = any other block with light > 0. |
| `opacity` | Mirrors Java `calculateOpacity`: air 0, liquids 1, blocks that do not occlude and let skylight through (glass, doors, panes, lanterns, plants, ...) 0, everything else 16. NOTE: leaves are **16**, not 1: Java leaves have a full collision shape so `propagatesSkyLightDown` is false. Ice is 16 for the same reason (`isIce` marks it separately). |
| `lightEmission` | hand map `light` in `overrides.json` (no vanilla JSON). State-dependent emitters (candles, copper bulbs, respawn anchor, lit/unlit redstone variants share ids only partly) use the default-state value, usually 0. |
| `isSolid` | Java semantics = non-empty collision shape. `nonSolid` glob list (air, liquids, plants, torches, rails, buttons, signs, banners, fire, portals...). `snow_layer` is non-solid as requested (opacity stays 16). Carpets are solid. |
| `isLiquid`, `isIce` | exact name lists (`liquids`, `ice`). |
| `colorSource` | `texture`, `alias` (id missing from blocks.json, textures taken from another id), `override` (hand colour), `invisible`, `default` (grey fallback, currently 0 blocks). |

Blocks absent from `blocks.json` (171 ids: Education Edition `element_*`/lab blocks, `hard_*` glass, `grass_block`,
`sea_lantern`, `iron_chain`, ...) are resolved through `aliases` / `aliasPrefixStrip`, then `colors` / `colorGlobs`.

## biome_table.json

Per biome in `mojang-biomes.json` + `behavior_pack/biomes`: `temperature`/`downfall` (behaviour pack `minecraft:climate`),
`waterColor`/`waterOpacity` (client biome `surface_color`, falling back to `biomes_client.json`; opacity default 0.65),
`fogId`, and `grassTint`, `foliageTint`, `birchTint`, `evergreenTint`, `dryFoliageTint`.
Tints are fixed colours when the client biome sets `grass_appearance`/`foliage_appearance`/`dry_foliage_color`
(mesa, swamp-likes, etc.), else a sample of `textures/colormap/{grass,foliage,birch,evergreen,dry_foliage}.png`
at `x=(1-t)*255, y=(1-d*t)*255` (t, d clamped to 0..1). `*TintMode` records which path was used (`fixed`,
`colormap:<name>`). Biomes whose client entry names a colormap (`swamp_grass`, `swamp_foliage`,
`mangrove_swamp_foliage`) are sampled from that map.

## Known gaps

* Dark-forest grass darkening, swamp noise-based grass variation and per-block-state tints (melon/pumpkin stems by age) are not modelled.
* Lily pad, bush and leaf_litter tints are guesses (grey textures); verify in game.
* Light/solid/opacity lists are hand-maintained heuristics; state-dependent behaviour (open doors, lit candles, snow layer height) is not represented because the table is per block id.
* `Unknown` is the largest material class (~57%): that is what the Java name rules produce for wool, concrete, glass, plants, etc.
* Textures for 1.26.51 may differ slightly from the 1.26.50.4 samples.
