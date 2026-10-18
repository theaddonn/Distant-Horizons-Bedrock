# Chunk ingress smoke test (GLUE-1): user checklist

Status: cross-compiled, **not yet run in game**. Verifies that a real Bedrock `LevelChunk` can be wrapped in
`BedrockChunk`, lit, and turned into a DH `FullDataSource`.

## Setup

1. Build + deploy: `scripts/build.sh client && scripts/deploy.sh client`.
2. The block tables are read at runtime from `<modDir>/data/{block,biome}_table.json` (copied by `deploy.sh`).
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

## Things to check on this run

a. Chunks with `columns=0`: client `onChunkLoaded` fires before sub-chunks arrive; we would need `onSubChunkLoaded`.
b. SP double-fire: log the thread id and `lc.mDimension.mId` to tell server-Level chunks from client-Level chunks.
c. `getBlock` on chunks with missing sub-chunks must not crash.
d. Y-offset sanity: bedrock at y=-64 and a known surface block in a flat world.
e. Biome name format from the `biome not in biome_table.json` log lines (expect `minecraft:plains`).
f. Per-chunk ms (target: a few ms).
g. Border sky light 14 vs 15 (the no-neighbour quirk of `lightChunk`).

## Known limitations

* `TableClientLevel::shade` uses the Overworld constants (Nether 0.9 is a TODO).
* `tintFaces` is parsed but not used yet (the whole block colour is tinted).
* Any exception inside the hook is swallowed and logged as `chunk ingress: exception swallowed`.
