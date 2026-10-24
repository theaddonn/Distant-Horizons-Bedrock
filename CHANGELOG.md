# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Level-of-detail rendering for Minecraft Bedrock 1.26.51 as a LeviLamina 26.51.6 client mod (`DistantHorizons`): LOD terrain, water/ice transparency, LOD clouds, fog hand-over, LOD lighting, distant chunk generation.
- Experimental server DLL (`DistantHorizonsServer`) for Bedrock Dedicated Server with LeviLamina (multiplayer LOD streaming).
- Prebuilt `DH_Terrain.material.bin` in `dist/materials/` (with sha256), shipped in the client package.
- `scripts/package.sh` and `scripts/check-version.sh`; CI builds both DLLs, runs the host tests and publishes `DistantHorizons-client-<version>.zip` and `DistantHorizons-server-<version>.zip` on `v*` tags.
- Config options for the Bedrock-specific rendering settings under `client.graphics.*`, `client.advanced.*`, `client.keys.toggleRendering`, `client.debug.*` and `common.worldGenerator.compareMode`; every saved option gets a `<name>_doc` sibling key. See README.md.
- `docs/DEBUG.md` documenting the debug overlay, debug keys and environment switches.
- Pack icon (the Distant Horizons logo by Pankakes, CC BY-SA 4.0), used as the tooth avatar.

### Changed

- The F-key spike/test hotkeys are gone. Rendering toggles are config options; the numpad debug keys only work with `client.debug.enabled = true`.
- `client.graphics.fog.disableVanillaFog` (bool) is replaced by `client.graphics.fog.vanillaFog` (`off|distance|fog-control|all`).
- `common.worldGenerator.allowUnvalidatedGenerator` now defaults to `true` (distant generation was validated in game).
- Enum values in `config.json` are matched case-insensitively, `-` and `_` being equivalent.
- The mod version has one source of truth (`xmake.lua`).

### Removed

- The R0/R1/R1b/R2/R3 spike module and its hotkeys; the primitive and origin cycles (fixed at QuadList, camera-relative).

