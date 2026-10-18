# FullDataSource storage / wire format (codec v1)

Implemented in `src/core/dh/storage/Codec.{h,cpp}`. Not byte compatible with Java
Distant Horizons; it keeps Java's columnar streams. All multi-byte integers inside
blobs are little-endian; `varint` is the LEB128 of `dh/util/Varint.h` (max 5 bytes,
values <= INT32_MAX); `zigzag` maps signed to unsigned as in Java `VarintUtil`.

## Record parts

A section is a `StoreRecord` of 8 blobs plus `StoreMeta`:

| part | LevelDB key | content |
|------|-------------|---------|
| `data` | `D` | interior columns x,z in [1,62] (62x62), x outer / z inner |
| `adjacent[0..3]` | `A` | edge strips N, S, W, E (index = `compassIndex`) |
| `genSteps` | `G` | RLE of 4096 `columnGenerationStep` int8 |
| `compressionModes` | `G` | RLE of 4096 `columnWorldCompressionMode` int8 |
| `mapping` | `M` | id -> (block serial, biome serial) |
| `meta` | `M` | version, compression, flags, checksum, timestamps |

Strips (columns index = x*64+z, iterated x outer, z inner, ranges from `adjRange`):
North x 0..63 z=0; South x 0..63 z=63; West x=0 z 0..63; East x=63 z 0..63 (64 columns each;
the four corner columns appear in two strips, so interior + strips cover all 4096 columns).
On full decode later strips overwrite corners (identical in valid data).

## Blob framing (every blob)

```
u8  formatVersion   (= 1)
u8  compression     (0 = None, 1 = Zstd [reserved])
payload...
```
`None`: payload is the raw payload described below. `Zstd` (TODO WP-D1b, currently
`Unsupported` on encode and decode): `varint rawSize` followed by one zstd frame that
decompresses to the raw payload. Every blob (also the RLE ones) uses the framing and
`CodecOptions.compression`. Any blob or decompressed payload > 64 MiB is rejected.

## Column payload (`data` and each `adjacent` blob)

Five streams, each `varint byteLength` + bytes, in this order; nothing may follow:

1. **lengths**: per column `varint pointCount` (<= 4096).
2. **ids**: per point `varint((id << 2) | hasLight << 1 | hasDiscontinuity)`.
3. **heights**: per point `varint height` (<= 4095).
4. **bottoms**: per point with `hasDiscontinuity` only: `varint zigzag(bottomY - expected)`,
   `expected = prevBottomY - height`.
5. **light**: per point with `hasLight` only: `u8 (blockLight << 4) | skyLight`.

`hasLight` = blockLight|skyLight != 0. `prevBottomY` starts at 0 in each blob and carries
across columns (no reset), points are in column order top-down. Streams 2-5 are
per-point in column order. Limits on decode: total points <= 2^22 and <= ids stream
length, bottomY must stay in 0..4095, and every non-zero data point must have
`id < mapping size` (`EMPTY_DATA_POINT == 0` is exempt); all streams must be fully consumed.

## RLE payload (`genSteps`, `compressionModes`)

```
varint runCount
runCount x { varint runLength (>=1), i8 value }
```
Run lengths must sum to exactly 4096 (index = x*64+z). An all-`Light` section is
2 (framing) + 1 (runCount) + 2 (`varint 4096`) + 1 (value) = 6 bytes.

## Mapping payload

```
varint stringCount
stringCount x { varint byteLength, UTF-8 bytes }     // deduped; block and biome serials share the table
varint pairCount
pairCount x { varint blockStringIndex, varint biomeStringIndex }   // pair i = id i
```
String table order = first use while walking ids 0.., block before biome. Unknown serials
resolve through `resolveBlockOrAir` / `resolveBiomeOrDefault` with a warning; ids stay aligned
even if two serials resolve to the same object. Counts are bounded by the remaining payload
size; `stringCount <= 2^25`, `pairCount <= 2^24`, and `stringCount > 0` requires `pairCount > 0`.

## Meta

`StoreMeta`: `dataFormatVersion` (=1), `compressionMode` (the `Compression` id), `flags`
(bit0 ApplyToParent, bit1 ApplyToChildren, bit2 Regenerate; `nullopt` -> 0), `dataChecksum`,
`createdMillis` / `lastModifiedMillis` from `createdUnixMs` / `lastModifiedUnixMs`.
Decoded flags are always concrete (`true`/`false`, never `nullopt`).

`dataChecksum` = FNV-1a 32 (offset 2166136261, prime 16777619) over, for the eight blobs
`data, adj N, S, W, E, genSteps, compressionModes, mapping` (in that order), their uncompressed
payloads (after the 2 framing bytes), each preceded by its payload length as 4 bytes LE.
Full `decode` verifies it; `decodeAdjacentOnly` does not (it may run without `data`).

Decode trusts the per-blob header (`formatVersion`, `compression`) and ignores
`meta.dataFormatVersion` / `meta.compressionMode`, which are informational copies.

## Wire envelope (`writeRecord` / `readRecord`, big-endian)

```
u64 pos
u8  dataFormatVersion
u8  compressionMode
u8  flags
u32 dataChecksum
i64 createdMillis
i64 lastModifiedMillis
8 x { u32 byteLength (<= 64 MiB, <= bytes remaining), bytes }   // data, adj N,S,W,E, genSteps, compressionModes, mapping
```
Blobs are the same bytes as on disk. K1 may wrap this in its own message framing.

## Derived on decode

`isEmpty` = no column holds a non-`EMPTY_DATA_POINT`. `adjacent-only` decode fills every column
outside the requested strip with `{EMPTY_DATA_POINT}` and still decodes gen steps, modes, mapping, flags.
