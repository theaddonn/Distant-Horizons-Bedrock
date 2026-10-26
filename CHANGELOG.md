# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- In-game settings: Settings -> Mods -> Distant Horizons, five pages generated from the option table (General with the quality and thread presets, Graphics, Culling, World generation, Advanced), saved to `config.json` at once. Built with [Lever](https://github.com/theaddonn/Lever), fetched by xmake at a pinned commit.

### Changed

- Rendering options (`client.advanced.*`, `client.graphics.fog.vanillaFog`, `client.graphics.clouds.backFaceCull`, `client.debug.*` log switches, `client.keys.toggleRendering`) and the master switch apply immediately when the store changes.
- The toggle key now writes `client.enableRendering` (as Java's quick-enable key does), so the state persists and the settings page shows it.

## [0.1.0-beta.1-mc26.51] - 2026-10-24

First public beta: the client mod for Minecraft Bedrock 1.26.51 with LeviLamina 26.51.6. The server package is experimental.

### Added

- Level-of-detail rendering for Minecraft Bedrock 1.26.51 as a LeviLamina 26.51.6 client mod (`DistantHorizons`): LOD terrain, water/ice transparency, LOD clouds, fog hand-over, LOD lighting, distant chunk generation.
- Experimental server DLL (`DistantHorizonsServer`) for Bedrock Dedicated Server with LeviLamina (multiplayer LOD streaming).
- Prebuilt `DH_Terrain.material.bin` in `dist/materials/` (with sha256), shipped in the client package.
- GitHub Actions build both DLLs and run the host tests on every push; publishing a release attaches `Distant-Horizons-Bedrock-client-windows-x64.zip` and `Distant-Horizons-Bedrock-server-windows-x64.zip` with the release notes taken from this changelog.
- Config options for the Bedrock-specific rendering settings under `client.graphics.*`, `client.advanced.*`, `client.keys.toggleRendering`, `client.debug.*` and `common.worldGenerator.compareMode`; every saved option gets a `<name>_doc` sibling key. See README.md.
- `docs/DEBUG.md` documenting the debug overlay, debug keys and environment switches.
- Pack icon (the Distant Horizons logo by Pankakes, CC BY-SA 4.0), used as the tooth avatar.

### Changed

- The F-key spike/test hotkeys are gone. Rendering toggles are config options; the numpad debug keys only work with `client.debug.enabled = true`.
- `client.graphics.fog.disableVanillaFog` (bool) is replaced by `client.graphics.fog.vanillaFog` (`off|distance|fog-control|all`).
- `common.worldGenerator.allowUnvalidatedGenerator` now defaults to `true` (distant generation was validated in game).
- Enum values in `config.json` are matched case-insensitively, `-` and `_` being equivalent.
- The mod version has one source of truth (`xmake.lua`).
- Distant generation builds a batch's scratch chunks in one server-thread task and takes one snapshot per delivered chunk instead of three (headless measurement: 21.7 to 82.6 chunks/s with the tick drain emulated); generation waits while the game is paused instead of failing batches after 15 s.
- `DH_GEN_PROBE=1` (server DLL) times vanilla and DH chunk generation phases for diagnostics.

### Removed

- The R0/R1/R1b/R2/R3 spike module and its hotkeys; the primitive and origin cycles (fixed at QuadList, camera-relative).

