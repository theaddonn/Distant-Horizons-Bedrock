# In-game checklists — one session, in this order

Agents never launch the game. Everything below is deployed by `scripts/deploy.sh client`
(DLL + `data/*.json` tables + `materials/DH_Terrain.material.bin` when built). Launch with
`~/.steam/steam/steamapps/common/MinecraftBedrockGDK/bin/mc-run.sh Retail-1.26.5101-Levi`,
enter any world, and keep the mod log open:
`~/.steam/steam/steamapps/common/MinecraftBedrockGDK/builds/Retail-1.26.5101-Levi/mods/DistantHorizons/logs/dh.log`.

Keys are only read while the HUD screen is up (no menus open). Bedrock's own F5 (camera) and
F11 (fullscreen) overlap with some spike keys — that is known and harmless for a test session.

| Step | Doc | Keys | What it settles |
|---|---|---|---|
| 0 | — | F7 HUD on, F8 dump | logger + HUD text render at all (`DH loaded`, `level: join`, `hud-dump:` in log) |
| 1 | [render-spikes.md](render-spikes.md) R0 | F9 | material names, D3D11 vs D3D12, viewport depth range, projection matrix, vertex fields |
| 2 | [render-spikes.md](render-spikes.md) R1/R1b/R2 | F10, F11, Shift+F10/F11 | immediate-draw path works; which hook draws in world space; fog on `MeshFallback`; origin/primitive conventions |
| 3 | [render-spikes.md](render-spikes.md) R3 | F12 (cycle) | depth convention, far-plane strategy (local push vs `mZFar`) |
| 4 | [chunk-ingress.md](chunk-ingress.md) | Shift+F7 | `LevelChunk` → `FullDataSource` works: columns>0, biome name format, y-offset, per-chunk ms, SP double-fire |
| 5 | [stage-a-render.md](stage-a-render.md) | F5, Shift+F5 (test feed), F4/F6 + modifiers | persistent `mce::Mesh` upload + draw of DH-format buffers; which toggle combination renders correctly |
| 6 | [stage-b-offline.md](stage-b-offline.md) B3 | (automatic at load) | injector log lines: `materials.index.json spliced`, `DH_Terrain served`, `DH_Terrain resolved: yes/no` |
| 7 | [stage-b-offline.md](stage-b-offline.md) B2 | manual file swap (backup first!) | does the loader accept a modified vanilla `.material.bin` (terrain tinted pink)? restore afterwards |

Turn each spike off again before the next (F10/F11/F12 and F5 share hooks; two active drawers
confuse the picture). Send back: the `dh.log` excerpt and screenshots named per step; the
R0/R3 raw numbers matter most — they decide the render architecture for everything after M1.
