# Render spikes R0-R3: user checklist

Status: code written and cross-compiled, **not yet run in game** (the display was not available to the agent). Everything
below is what to do and what to send back. The DLL is already deployed (`scripts/deploy.sh client`).

## Setup

1. Launch `~/.steam/steam/steamapps/common/MinecraftBedrockGDK/bin/mc-run.sh Retail-1.26.5101-Levi`, wait for the world to
   load (it auto-resumes). Use creative/flying; stand on open ground.
2. The HUD (top-left) should show `DH 0.1.0 (<sha>)`, `frame ms`, and `spike.*` lines. F7 hides it, F8 dumps it to the log.
   If the HUD box shows but the text is missing, tell me: the text API (`drawDebugText` + `flushText`) is unverified.
3. Log: `~/.steam/steam/steamapps/common/MinecraftBedrockGDK/builds/Retail-1.26.5101-Levi/mods/DistantHorizons/logs/dh.log`.
   Screenshots: `scripts/screenshot.sh <name>` -> `artifacts/screenshots/` (or any screenshot you like).
4. Startup line to expect: `spikes installed (all off; F9 R0 dump, F10 R1/R2, Ctrl+F10 R1b, F12 R3, Shift+F10 origin, Shift+F12 primitive)`.
   All spikes are off at start; hooks are passthrough until toggled. Keys are read with `GetAsyncKeyState` only while the
   game window is focused and the HUD screen is up (no chat/menu open). Note F12 is also RenderDoc's capture key
   (harmless, it just saves a capture).

Key map: F9 R0 dump | F10 R1(+R2) quads | Ctrl+F10 R1b | F12 cycle R3 (off / local-proj-push / mZFar-override) |
Shift+F10 origin mode (camera-relative <-> absolute) | Shift+F12 primitive (QuadList <-> TriangleList, rebuilds meshes). F11 is Bedrock fullscreen and unused.

Quads are placed when you toggle on: dead ahead along the dominant horizontal axis you are facing, base at your feet
height minus 5. After toggling on, do not move for the first screenshot, then walk sideways for the second.

## R0 inventory (F9)

Press F9 once. Expect (within a frame) between `R0 ===== inventory begin =====` and `... end =====`:

    R0 vro.mIsShadowPass=0 cameraType=<n>
    R0 gfx modules: d3d12=? d3d11=? d3d9=? vulkan-1=? dxgi=?     (expect vulkan-1=1, d3d*=0 under Proton/Vulkan)
    R0 viewport: size=(w,h) offset=(0,0) minDepth=? maxDepth=?
    R0 camera: zNear=? zFar=? fov=? aspect=? pos=(x,y,z)
    R0 proj ... / R0 view top ... / R0 world top ...      (16 floats each, glm m[col][row] printed column by column)
    R0 layout=column-vector|row-vector eyeSign=+1|-1: ndcz(near)=? ... (4 lines)
    R0 tessellator probe prim=QuadList|TriangleList: mFieldEnabled=<15 bits> colour=0x........ streams[pos=1 nrm=? ...]
    R0 materials[0..k]: <comma list>
    R0 materials: count=N hasRenderChunk=1 hasMeshFallbackPosUVNormalColor=1 => PASS
    R0 selectionOpaqueMaterial valid=1 fallback valid=1

PASS = `=> PASS` line (>=150 names incl. `RenderChunk`, `MeshFallbackPosUVNormalColor`). Send: the whole R0 block.

## R1 quad (F10), R2 hook comparison (same toggle)

Press F10. Expect once each:

    spike anchor: cam=(...) groundY=.. dir=(..) plane=XY(faces Z)|ZY(faces X)
    spike mesh built key=... verts=8 fieldEnabled=... probe[...]
    spike draw [R1/R2 LevelRendererCamera::render mat=selectionOpaqueMaterial] ok verts=8 translate=(...)
    spike draw [R1/R2 LevelRenderer::renderLevel mat=...] ok ...
    spike draw [R1/R2 InGamePlayScreen::_postLevelRender mat=...] ok ...

Look for three 10x10 vertical quads ~8 blocks ahead: **magenta** (LevelRendererCamera::render, centre), **green**
(renderLevel, 12 blocks to the side), **yellow** (_postLevelRender, 24 blocks to the side). Each spans ground-5..ground+5.

PASS R1: magenta quad visible, lower half hidden by terrain (depth-tested), and it stays fixed in the world when you walk
around (take 2 screenshots from different positions).
R2: note which of the three colours are visible and world-fixed (a hook that draws in screen space or without depth will
look wrong/stuck to the camera).

If nothing is visible: (a) press Shift+F10 (origin mode -> absolute; the default assumes camera-relative rendering, so the
translate is `world - cameraPos`), screenshot again; (b) press Shift+F12 (TriangleList) and repeat; (c) look for
`spike draw ... skipped: material invalid`. Send: log lines + screenshots (note the mode in the log: `spike origin mode ->`).

## R1b fogged quad (Ctrl+F10)

Press Ctrl+F10 (leave F10 off or on). An orange 40x40 quad appears 160 blocks ahead using `MeshFallbackPosUVNormalColor`:
`spike draw [R1b fogged MeshFallbackPosUVNormalColor @160] ok ...`. PASS = it is visibly fogged (colour blended toward the
fog colour compared with the unfogged magenta R1 quad). Compare with F10 magenta at 8 blocks. Send a screenshot.

## R3 depth / far plane (F12)

The R0 dump already contains the raw projection numbers; the key log lines for interpretation are `R3 layout=...`,
`R3 detected: layout=... eyeZnear=... ndcNear=... ndcFar=... kind=...`, `R3 patchFar(20000) -> [...]` and the four
`R3 ref glm ...` lines. kind is one of `ZO 0..1`, `ZO reversed 1..0`, `NO -1..1`, `NO reversed`, with handedness.

* F12 once = mode 1 `local-proj-push`: a cyan 400x400 quad at 1000 blocks is drawn after pushing a projection with far=20000
  around that draw only. Log: `R3 mode -> local-proj-push (F12)` and `spike draw [R3 mode=local-proj-push quad@1000] ok`.
  PASS = cyan quad visible on the horizon (it should vanish when looking away; it is large, ~22 degrees wide).
  If it is invisible, also try Shift+F10 first (origin mode).
* F12 again = mode 2 `mZFar-override`: `LevelRendererPlayer::setupCamera` is hooked; after the original call `mZFar=20000`, the
  projection top is rebuilt and `updateViewMatrixDependencies()` is called. Log: `R3a setupCamera: cam==ci.camera:1 mZFar
  <old> -> 20000.0 (<kind>) near=...`. The quad is drawn without a local push. PASS = quad visible AND no z-fighting on
  nearby hillsides AND no missing vanilla chunks when turning quickly (look for flickering terrain/holes). Send a
  screenshot looking at nearby slopes and one while turning, and your verdict.
* F12 again = off.

## What to send back

1. `dh.log` excerpt from `DH loaded` to the end (or the R0 block + all `spike`/`R3` lines), plus the F8 `hud-dump:` line.
2. Screenshots (filenames under `artifacts/screenshots/` or wherever you saved them): R1 from two positions, R1b, R3 mode 1,
   R3 mode 2 (nearby slopes + while turning). Mention which origin/primitive mode was active for each.
3. For each spike: PASS / FAIL / PARTIAL in your words, and anything odd (crash on toggle = note which key; the last log
   line before the crash is flushed to disk).
