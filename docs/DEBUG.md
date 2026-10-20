# Debugging Distant Horizons (Bedrock)

Everything here is for development and bug reports. A normal install needs none of it.

## Options (`config.json`, `client.debug.*`)

| Option | Default | Meaning |
|---|---|---|
| `client.debug.enabled` | `false` | Turns on the debug overlay and the debug keys below. With `false` nothing is drawn and no keys are polled, except `client.keys.toggleRendering` (if bound) and Shift+F5 when `DH_TEST_FEED=1`. |
| `client.debug.hudKey` | `"numpad0"` | Key that toggles the overlay (only used with `debug.enabled`). Key names: README.md. |
| `client.debug.logRenderPerf` | `true` | The `render: perf` line in `dh.log` every 10 s. |
| `client.debug.logLightingDiag` | `false` | The periodic lighting diagnostics line (also on whenever `debug.enabled` is true). |

Changes need a game restart. Toggles made with the keys below are runtime-only and are not saved back to the file.

## Debug keys (only with `client.debug.enabled = true`)

The numpad keys need NumLock on (with NumLock off Windows reports them as arrow/navigation keys).

| Key | Action | Persistent option |
|---|---|---|
| `client.debug.hudKey` (Numpad 0) | overlay on/off | - |
| Numpad . | dump the HUD registry as JSON to `dh.log` (`hud-dump: {...}`) | - |
| Numpad 1 | cycle vanilla fog mode (`off`, `distance`, `fog-control`, `all`) | `client.graphics.fog.vanillaFog` |
| Numpad 2 | DH clouds on/off | `client.graphics.clouds.renderDhClouds` |
| Numpad 3 | cycle lightmap mode (order: `shader`, `dynamic`, `dynamic-swapped`, `unlit-test`, `light-debug`; the first press from the default `dynamic` goes to `dynamic-swapped`) | `client.advanced.lightmapMode` |
| Numpad 4 | cycle sky order (`overdraw`, `late`, `opaque`) | `client.advanced.skyOrder` |
| Numpad 5 | cloud probe (one log line: cloud mesh/pattern state) | - |
| Numpad 6 | cloud back-face cull on/off | `client.graphics.clouds.backFaceCull` |
| Numpad 7 | late-repeat scope (`beyond-vanilla` / `full`) | `client.advanced.lateScope` |
| Numpad 8 | near clip on/off | `client.advanced.nearClip` |
| Numpad 9 | cycle far-plane mode (`local-proj-push`, `zfar-override`, `off`) | `client.advanced.farPlane` |
| Numpad / | cycle material (`dh`, `mesh-fallback`, `selection`) | `client.advanced.material` |
| Numpad * | cycle draw hook (`camera-after`, `level-render-before`, `level-render-after`, `post-level-render`) | `client.advanced.drawHook` |
| Numpad - | chunk ingestion on/off | - |
| Numpad + | core frustum cull on/off | `client.advanced.coreFrustum` |
| `client.keys.toggleRendering` | LOD pipeline on/off (works without debug) | `client.enableRendering` |
| Shift+F5 | synthetic test feed (only with env `DH_TEST_FEED=1`) | - |

Other debugging-relevant options: `client.advanced.drawTransparent` (draw the water/ice layer),
`common.worldGenerator.chunkGeneratorMode` (`FEATURES` or `SURFACE`), `common.worldGenerator.compareMode`.

Warning: if a configured key is one Bedrock uses itself (F1, F2, F3, F5, F11) the mod logs a warning; an invalid key name
is logged and treated as `none`.

## Environment switches (set before starting the game)

| Variable | Effect |
|---|---|
| `DH_TEST_FEED=1` | enables Shift+F5, a synthetic LOD feed that draws every uploaded section (Stage-A testing) |
| `DH_MATERIAL_INJECT=0\|1` | force the custom `DH_Terrain` material off/on (default: on iff `materials/DH_Terrain.material.bin` exists) |
| `DH_BEDROCK_NO_FEATURES=1` | dev override: distant generation produces base terrain only; prefer `chunkGeneratorMode=SURFACE` |
| `DH_BEDROCK_GEN_COMPARE=1` | compare generated chunks with the real ones and log the fidelity (also set by `compareMode=true`) |

## What the overlay and the log contain

- **Overlay / `hud-dump`:** the HUD registry (`dh::hud`): version and git sha, `render.*` (enabled, material, fog,
  vanillaFog, skyOrder, lateScope, lightmap/lighting, hook, far/cutoff/clip, sections listed/drawn/culled, pending
  uploads, `render.ms`, cloud state), `ingest.*` (chunks ingested, drains, skipped, on/off), `gen.queued`, `net.rx/tx`,
  `tree.*` (loading/notReady/culled), `world.levels`, `tick.ms`, `pipeline`, `mode`. The dump is one JSON object on one
  `hud-dump:` line.
- **`render: perf` (every 10 s, per-frame averages over the window):** fps; draw calls and kquads per layer (opaque, late,
  water, clouds); CPU submit ms per layer plus total `render.ms` average and max (GPU time is not measurable from the
  mod); sections visible / late-skipped / clip-skipped.
- **`world gen:` lines:** distant-generation worker threads, mode and errors (for example `world gen: Features mode disabled
  by DH_BEDROCK_NO_FEATURES`).
- **`materials: freeze watch`** reports go to `dh-freeze.log` only when an engine-facing hook is stuck for 3 s (10 s for the
  async material request).

Logs live in `<mod dir>/logs/` (`dh.log`; each game start moves the previous log to `dh.log.1` and `dh.log.2`, and a log also rotates at 8 MB; `dh-freeze.log`, capped at 256 KB).
