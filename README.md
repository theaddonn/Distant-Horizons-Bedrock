# Distant Horizons (Bedrock)

A port of [Distant Horizons](https://gitlab.com/distant-horizons-team/distant-horizons) to Minecraft Bedrock Edition as a [LeviLamina](https://github.com/LiteLDev/LeviLamina) mod. It renders level-of-detail terrain far beyond the normal render distance: LOD terrain with water and ice, LOD clouds, fog hand-over to the vanilla chunks, LOD lighting, and generation of terrain that has not been visited yet.

Work in progress. The client mod is the supported part; the server package is experimental.

## Compatibility

| Component | Version |
| --- | --- |
| Minecraft Bedrock for Windows | 1.26.51.01 |
| LeviLamina | 26.51.6 (client variant for the client mod, server variant for the server plugin) |

## Install

Download the zips from the GitHub release page.

**Client** (`DistantHorizons-client-<version>.zip`): install LeviLamina 26.51.6 for the Bedrock client, then copy the zip's `mods/DistantHorizons/` folder into the game's `mods/` folder (next to the LeviLamina files). It contains `DistantHorizons.dll`, `manifest.json`, `data/` (block and biome tables) and `materials/DH_Terrain.material.bin` (the LOD shader). Start the game; the LOD pipeline starts with the world (`client.enableRendering`). Do not copy anything into the game's own data folders.

**Server, experimental** (`DistantHorizons-server-<version>.zip`): install LeviLamina 26.51.6 on the Bedrock Dedicated Server and copy `plugins/DistantHorizonsServer/` into the server's `plugins/` directory. Multiplayer LOD streaming is under development and has had far less testing than the single-player client.

The mod writes its configuration on first start, so no `config.json` is shipped.

## Configuration

File: `<game>/mods/DistantHorizons/config/config.json` (LeviLamina's config directory of the mod), written with defaults when missing. The file is nested JSON by the dotted path below. Rules:

- Changes need a game restart.
- Unknown keys are ignored. Every saved option has a sibling `"<name>_doc"` key with its description; `_doc` keys are ignored on load.
- Enum values are matched case-insensitively, `-` and `_` are equal (`"OVERDRAW"`, `"beyond_vanilla"` work). The file is saved with the canonical name.
- Many Java-derived options are parsed and saved but not all are wired to the Bedrock renderer yet.

Options new in 0.1.0 (Bedrock specific) are marked **new**. Full list with ranges: `src/core/dh/config/Options.cpp`.

### Rendering (`client`)

| Option | Default | Meaning |
|---|---|---|
| `client.enableRendering` | `true` | Master switch; the pipeline starts with the world. |
| `client.keys.toggleRendering` **new** | `"none"` | Optional key that toggles the LOD pipeline while playing (see below). |
| `client.graphics.fog.vanillaFog` **new** | `all` | Push Minecraft's distance fog out to the LOD distance: `off` / `distance` / `fog-control` / `all`. Replaces `disableVanillaFog`. |
| `client.graphics.fog.enableDhFog` | `true` | Fog on the LODs. |
| `client.graphics.clouds.renderDhClouds` | `true` | Draw the DH cloud layer out to the LOD distance. |
| `client.graphics.clouds.hideVanillaClouds` | `true` | Hide Minecraft's clouds while DH clouds are drawn. |
| `client.graphics.clouds.backFaceCull` **new** | `true` | Cull back faces of the DH clouds. |
| `client.graphics.quality.lodChunkRenderDistanceRadius` | `256` | LOD render distance in chunks (32..4096). |
| `client.graphics.quality.horizontalQuality` / `verticalQuality` / `maxHorizontalResolution` | `MEDIUM` / `MEDIUM` / `BLOCK` | Detail settings, as in the Java mod. |
| `client.graphics.quality.transparency` | `COMPLETE` | `DISABLED` hides the water/ice layer. |
| `client.graphics.quality.lodBiomeBlending` | `3` | Biome blending radius (0 = off). |
| `client.graphics.quality.brightnessMultiplier` / `saturationMultiplier` | `1.0` | LOD colour adjustments. |
| `client.graphics.culling.*` | see file | Overdraw prevention, cave culling, ignored blocks, frustum culling. |
| `client.advanced.skyOrder` **new** | `overdraw` | Pass the opaque LODs are drawn in: `overdraw` / `late` / `opaque`. `overdraw` keeps stars, sun and moon behind LODs. |
| `client.advanced.lateScope` **new** | `beyond-vanilla` | Where the overdraw repeat runs: `beyond-vanilla` / `full`. |
| `client.advanced.lightmapMode` **new** | `dynamic` | LOD lighting source: `dynamic` / `shader` / `dynamic-swapped` / `unlit-test` / `light-debug` (last two are diagnostics). |
| `client.advanced.nearClip` **new** | `true` | Skip LODs inside the vanilla render distance. |
| `client.advanced.farPlane` **new** | `local-proj-push` | How the far plane is extended: `local-proj-push` / `zfar-override` / `off`. |
| `client.advanced.material` **new** | `dh` | `dh` / `mesh-fallback` / `selection`. Falls back automatically if `DH_Terrain.material.bin` is missing. |
| `client.advanced.drawHook` **new** | `camera-after` | Where the LOD draw is hooked: `camera-after` / `level-render-before` / `level-render-after` / `post-level-render`. Leave as is. |
| `client.advanced.coreFrustum` **new** | `true` | Frustum culling while building the render list. |
| `client.advanced.drawTransparent` **new** | `true` | Draw the translucent (water/ice) layer. |
| `client.debug.*` **new** | off | Debug overlay and keys, see [docs/DEBUG.md](docs/DEBUG.md). |

### Generation and storage (`common`)

| Option | Default | Meaning |
|---|---|---|
| `common.worldGenerator.generatorPlan` | `SURFACE_THEN_CHUNKS` | How LODs are generated outside the vanilla render distance (`DISABLED` turns it off). |
| `common.worldGenerator.allowUnvalidatedGenerator` | `true` | Safety switch (default flipped in 0.1.0): `false` forces distant generation off. |
| `common.worldGenerator.chunkGeneratorMode` | `FEATURES` | `FEATURES` = decorated terrain, `SURFACE` = base terrain only (faster). |
| `common.worldGenerator.scratchThreads` | `0` | Worker threads for distant generation (0 = auto). |
| `common.worldGenerator.compareMode` **new** | `false` | Debug: compare generated chunks with real ones. |
| `common.lodBuilding.*`, `common.multiThreading.*` | see file | Compression and thread settings. |

`server.*` options configure the experimental multiplayer side.

### Optional key

`client.keys.toggleRendering` takes one key name (case-insensitive): `none` (default), `f1`..`f24`, `numpad0`..`numpad9`, `numpad-decimal`, `numpad-divide`, `numpad-multiply`, `numpad-subtract`, `numpad-add`, `a`..`z`, `0`..`9`, `insert`, `delete`, `home`, `end`, `pageup`, `pagedown`, `pause`, `scrolllock`. An invalid name is logged and ignored. Avoid F1, F2, F3, F5, F11 (Minecraft uses them; the mod logs a warning).

## Logs and bug reports

Logs are in `<game>/mods/DistantHorizons/logs/`:

- `dh.log` (rotates to `dh.log.1` at 5 MB): the mod log. Includes `render: perf` lines every 10 s and `world gen:` lines.
- `dh-freeze.log`: written only if an engine-facing hook hangs (freeze watch).
- LeviLamina's own `logs/` folder and, after a crash, its crash dump / crash log.

When reporting a bug, attach `dh.log` (and `dh.log.1`), `dh-freeze.log` if it exists, the LeviLamina crash log, your `config.json`, and say what you were doing. For visual problems, enable `client.debug.enabled` (see [docs/DEBUG.md](docs/DEBUG.md)) and press Numpad . once to put a `hud-dump:` line in the log, plus a screenshot.

## Known limitations

- No structures (villages, temples, ...) in distant generation; trees and ores are generated with `FEATURES`.
- Water colour and biome tint are approximations.
- Late-pass water tint can differ slightly in the 115..144 block overlap band.
- Faint stars can show through LOD water at night.
- A freeze at pipeline start was seen once and is not root-caused; `dh-freeze.log` exists to capture it.
- No in-game settings screen; edit `config.json` and restart.
- Server and multiplayer support is experimental.

## Building

See [docs/BUILDING.md](docs/BUILDING.md). Debugging: [docs/DEBUG.md](docs/DEBUG.md). Porting notes: [docs/PORTING.md](docs/PORTING.md).

## License

LGPL-3.0, like the original mod. See `LICENSE` and `LICENSE.LESSER`.
