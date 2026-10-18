# Stage-A LOD renderer: user checklist

Status: code written and cross-compiled, **not yet run in game**. Prerequisite: the render spikes (`render-spikes.md`) give the
answers to R0-R3; this renderer is built so each of those unknowns is a runtime toggle.

What it is: `dh::IVertexSink` (`MeshUploader`) -> `mce::Mesh` per section (opaque + transparent) -> drawn each frame from a
`LevelRendererCamera::$render` hook with a vanilla material, per-section world-matrix translate, CPU-lit vertex colours.
The core is not wired yet (G2), so a **test feed** (Shift+F5) pushes synthetic sections through the same sink.

Files: `src/core/dh/mesh/StageAVertex.*` (pure, host-tested), `src/client/render/{MeshUploader,LodRenderer,TestFeed,RenderHooks}.*`.

## Setup

Same as the spikes: launch `mc-run.sh Retail-1.26.5101-Levi`, load the world, creative + fly, stand on open ground.
Keep the spike toggles (F10-F12) **off**: both hook `LevelRendererCamera::$render` and `setupCamera`; the hook chain runs both,
but the spike quads and the LOD geometry would then be drawn together and confuse the result.

Startup log line: `render: Stage-A renderer installed (off; F5 on/off, ...)`. Everything is passthrough until F5.
HUD lines: `render.enabled/material/origin/prim/far/feed`, `render.sections` (live), `render.pending`, `render.drawn`,
`render.culled` (frustum), `render.cutoff` (near cut-off), `render.bytes`, `render.ms` (CPU ms of the whole `renderFrame`).

## Keys (hud screen only, window focused)

| Key | Action |
|---|---|
| F5 | Stage-A renderer on/off (off at start) |
| Shift+F5 | test feed on/off (needs one rendered frame first, so press F5 first; Shift+F5 turns the renderer on if it is off) |
| F4 | primitive/winding cycle: QuadList, TriangleList, TriangleList-flipped, TriangleList-double-sided, QuadList-double-sided (live sections are re-converted) |
| Shift+F4 | far-plane mode: off, local-proj-push (projection pushed around our draws), zfar-override (`setupCamera` hook, `mZFar`=20000) |
| Ctrl+F4 | near cut-off on/off (default off) |
| F6 | material: selection (`selectionOpaqueMaterial`), meshFallback (`MeshFallbackPosUVNormalColor`), dh (not implemented: falls back to meshFallback) |
| Shift+F6 | origin mode: camera-relative / absolute (per-section translate = `sectionMin - cameraPos` vs `sectionMin`) |
| Ctrl+F6 | transparent pass on/off |

## Expected log lines (`dh.log`)

    render: Stage-A renderer installed (off; ...)
    render: Stage-A renderer on (enabled by test feed)          (if you used Shift+F5 first) or: render: Stage-A renderer on (F5)
    render: native vertex format ok: mFieldEnabled=<15 bits> colour shifts r=.. g=.. b=.. alphaMask=0x........
    render: test feed uploaded 9 sections, N quads (centre section sx,sz min corner block (x,z), sheet y=Y)
    render: first draw: V/S sections visible, C draw calls, material=... origin=... prim=... far=... proj=<kind>

`first draw` is logged once. `native vertex format ok` is logged once per `BufferResourceService`. Warnings to report:
`native colour packing not recognised`, `material '...' invalid`, `projection layout not recognised`, `buffer service changed`.

## What PASS looks like

1. F5, then Shift+F5. HUD: `render.sections=9`, `render.drawn` > 0 and `render.pending` back to 0 within a second
   (uploads are budgeted to 8 per frame).
2. Test feed layout: the player's 64x64 section (the centre one) gets a **blue/yellow checkerboard** of 1-block columns
   floating **4 blocks above your feet** (the yellow columns 1 block higher), a **red 4x4 pillar, 40 blocks tall** at
   (8..12, 8..12) from the section's min corner, and the 8 neighbouring sections each get a flat green/teal sheet at
   the same height; the +X neighbour also has a **translucent blue sheet** 6 blocks higher. Fly up and look down at it.
3. World-fixed: the geometry stays put as you walk and fly (no sliding, no popping at the edges), is depth-tested
   against terrain (the pillar disappears into hills), and the checkerboard is visible from both above and below.
4. Colour: checker colours and the red pillar roughly right (not swapped red/blue). Shading is the CPU stand-in
   (`lightFactor`): everything is sky-light 15, so it should look at full brightness.
5. Shift+F5 again removes everything (`test feed removed`, `render.sections` back to 0).
6. Look away and back: `render.culled` rises when sections are off-screen (frustum culling works).
7. `render.ms` stays well below 1 ms with the feed (9 sections, ~8-9k quads).

## If nothing shows (try in this order, one change at a time)

1. `render.enabled` on? `render.sections` > 0? If sections stay 0 and `render.pending` > 0, the pump never ran: look for
   `native colour packing` / `material invalid` warnings.
2. Shift+F6: origin mode (the default assumes camera-relative rendering).
3. F4: cycle the primitive/winding; `...double-sided` rules out face culling. QuadList vs TriangleList decides whether the
   engine expands quads for this material.
4. F6: material (selection vs meshFallback). The fallback material is fogged.
5. Shift+F4: far-plane mode. Only matters for geometry beyond vanilla's far plane (the feed is within ~100 blocks).
6. Ctrl+F4 must be **off** for the feed (the near cut-off hides everything inside the vanilla render distance).
7. If geometry is visible but broken/flickering with the transparent sheet: Ctrl+F6 (transparent pass off).

## Unknowns R0-R3 this must settle (and where the toggle is)

* **R0/R1 material + vertex format**: does a `MeshData` with positions + packed colour + UV0 + single-value normal draw
  with `selectionOpaqueMaterial` / `MeshFallbackPosUVNormalColor`? (F6; vertex-format log line.)
* **R1 matrix semantics**: does a pushed world matrix with only a translate place section-local geometry correctly, and is the
  base world matrix really identity at this hook (origin mode, Shift+F6)? Culling uses `P*V` (or `V*P` for the
  row-vector layout detected by `analyzeProjection`) applied to the same draw-space points, so it is wrong exactly when
  the origin mode is wrong: if sections disappear at screen edges only in one origin mode, that is the answer.
* **Winding / culling** (F4): which of QuadList / TriangleList / flipped is front-facing for the materials.
* **Transparency**: whether the opaque material blends alpha at all (the translucent sheet); a dedicated transparent
  material is a Stage-B item.
* **R2 hook point**: `$render` after `origin` (this hook) vs `LevelRenderer::renderLevel` / `_postLevelRender` (spike result decides;
  moving the hook is a one-line change in `RenderHooks.cpp`).
* **R3 depth / far plane** (Shift+F4): local-proj-push vs zfar-override; reversed-Z or not (the `first draw` log prints the
  detected projection kind). Frustum culling intentionally uses only left/right/bottom/top plus "in front of the camera"
  (w > 0) so it is independent of this.
* **Vanilla render distance units** (`ViewRenderData::mRenderDistance`: chunks or blocks): the near cut-off logs the radius
  it used (`near cut-off radius ... blocks`); values below 64 are multiplied by 16.
* **Off-thread safety**: `upload()` only converts floats on the calling thread; all `mce` objects are created in `pump()` on
  the render thread (R4).

## Notes

* Meshes are split into parts of at most 65000 vertices (16-bit index safety for QuadList), so a draw-call count above the
  section count is normal for big sections.
* DH `y` is relative to the level min height; `LodRendererConfig::minYBlocks` (-64) is added in the conversion.
* When the renderer is off, queued uploads wait (the render thread never pumps); turning it on drains them.
* On level exit the live meshes are dropped on the next pump; on mod disable they are leaked on purpose (not on the render thread).
* CPU light: `lightFactor(sky, block, skyBrightness) = 0.04 + 0.96 * 0.8^(15 - max(sky*skyBrightness, block))`; `skyBrightness`
  is a constant 1.0 for now (`Dimension::getSkyDarken` not wired).
