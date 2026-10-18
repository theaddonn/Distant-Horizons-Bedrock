# Chunk ingress smoke test (GLUE-1): user checklist

Status: cross-compiled, **not yet run in game**. Verifies that a real Bedrock `LevelChunk` can be wrapped in
`BedrockChunk`, lit, and turned into a DH `FullDataSource`.

## Setup

1. Build + deploy: `scripts/build.sh client && scripts/deploy.sh client`.
2. The block tables are read at runtime from `<modDir>/data/block_table.json` and `<modDir>/data/biome_table.json`
   (modDir = `.../mods/DistantHorizons/`). `deploy.sh` does not copy them yet, so once do:
   `mkdir -p <modDir>/data && cp data/*.json <modDir>/data/`.
3. Launch the game, load a world (fresh chunks must load AFTER enabling, so enable first, then walk / fly or change
   render distance / re-enter the world).

## Use

* Press **F6** on the HUD screen (no menu/chat open). Log: `ingress: on (F6)`. Press again for `ingress: off`.
  Off by default; startup line: `ingress installed (off; F6 toggles chunk -> LOD smoke test)`.
* HUD (F7 toggles it, F8 dumps to log) shows `ingress.on`, `ingress.chunks` (count processed), `ingress.lastMs`.

## Expected log (`logs/dh.log`)

    ingress: block table loaded (1531 blocks, 89 biomes)          <- once, on the first chunk after F6
    chunk 12,-4: 256 columns, 9 ids, 3.40 ms                      <- one per loaded chunk

Meaning: `columns` = non-empty columns the builder wrote for that chunk (max 256), `ids` = size of the block/biome id
map of the produced section, `ms` = wrap + light + build time (block materialisation dominates).

Possible findings to report back:

* `columns = 0` for most chunks: on the client `onChunkLoaded` may fire before sub-chunks arrive (chunk still
  empty). Then the hook point must move to a sub-chunk-loaded hook; tell me.
* `block not in block_table.json, treated as air: <serial>` warnings: names the table lacks.
* `biome not in biome_table.json`: shows the biome-name format actually returned (`Biome::mHash`); if it is not
  `minecraft:<name>` / `<name>`, tell me the logged value.
* `(build failed)` suffix: `createFromChunk` returned nothing (column-order corruption).
* A crash right after F6: most likely a layout assumption (`Block::mSerializationId`, `LevelChunk::mDimension`,
  `Dimension::mHeightRange`); send the crash log.
