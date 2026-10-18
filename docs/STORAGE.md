# LOD storage (LevelDB)

`ILodStore` (core contract) is implemented three ways:

| Class | Where | Use |
|---|---|---|
| `MemoryLodStore` | `src/core/dh/storage` | reference implementation, tests |
| `KvLodStore` | `src/core/dh/storage` | all store logic over an `IKeyValue` (pure core, host-tested) |
| `LevelDbLodStore` | `src/bedrock_common` | `KvLodStore` over `ll::data::KeyValueDB` via `LeviKeyValue` |

Distance/ordering/range helpers are shared through `StoreQueryUtil.h`
(64-bit min-corner Manhattan distance, ties by ascending pos, timestamp range
end-exclusive), so the stores cannot drift apart.

## Key layout

Multi-byte fields are big-endian; signed coordinates are stored offset-binary
(`v ^ 0x80000000`) so unsigned byte order equals numeric order. `pos = (det, x, z)`
with `det` one byte.

| Key | Bytes | Value |
|---|---|---|
| `D det x z` | 10 | data blob |
| `A dir det x z` | 11 | adjacent strip, `dir` 0 N, 1 S, 2 W, 3 E |
| `G det x z` | 10 | genSteps blob |
| `C det x z` | 10 | compressionModes blob |
| `P det x z` | 10 | mapping blob |
| `M det x z` | 10 | meta, 24 bytes: `u8 layoutVersion(1) u8 dataFormatVersion u8 compressionMode u8 flags u32 checksum i64 created i64 lastModified` |

(`C` and `P` are additions to the plan's D/A/G/M so every `StorePart` is
selectively loadable.) Keys sort by (tag, detail, x, z). `M` is the record's
existence marker.

## Atomicity

`put`, `erase` and `setFlag` are each one `KeyValueDB::write(WriteBatch)`
(a LevelDB write batch: all-or-nothing, replayed from the log after a crash).
A record exists iff its `M` key exists; `M` is the last op of every batch, so
even without batch atomicity a half-written record would read as absent. A put
that creates a record also deletes the part keys it does not supply, so no
orphan blob can resurface. `get` reads only the keys of the requested parts.
`setFlag` rewrites only `M`.

Durability: LevelDB writes go to its log without fsync (`KeyValueDB` exposes no
sync option), so a process crash loses nothing but an OS crash/power loss may
lose the last writes. `flush()` is therefore a no-op for `LeviKeyValue`.

## Reopen

`KvLodStore::load()` (called by `open`) iterates the `M` prefix once and builds:
`pos -> lastModified` (serves `exists`, `timestamp`, `timestamps`,
`allPositions`) and one `std::set<pos>` per flag (serves the distance queries).
`KeyValueDB::iter()` has no seek, so the scan walks the `A/C/D/G/P` keys that
sort before `M` (values are not copied into the store, but LevelDB still pages
them). A malformed `M` value is logged and the record treated as absent; a later
`put` overwrites it. Memory is ~48 bytes per record plus flag sets.

## IKeyValue / KeyValueDB facts

LeviLamina 26.51.6 `ll::data::KeyValueDB`: `get`, `has`, `set`, `del`,
`write(WriteBatch)`, `iter()` (ordered coroutine generator over the whole DB),
`empty()`. Failures surface only as `bool`/`nullopt`; the constructor can throw.
`LeviKeyValue` wraps every call in try/catch and returns `dh::Error`.

## Running the conformance suite

Host (always, CI-able): `scripts/test-host.sh -tc="*conformance*"` runs
`runStoreConformance` against `MemoryLodStore` and `KvLodStore` over
`MapKeyValue` (`tests/unit/storage_kv_store_test.cpp`).

LevelDB glue (`LeviKeyValue`): `tests/win/store_conformance_win.cpp`, built only
with `--store_test=y`:

    xmake f -P . -p windows -a x64 -m release --target_type=server --store_test=y \
        --toolchain=clang-cl --sdk=$HOME/.local/share/amtool/winsysroot -y
    xmake -P . -y dh_store_conformance_win

The exe imports `LeviLamina.dll`, which in turn needs `PreLoader.dll` and the
game process; it does not load under plain Windows or wine, so it can only be
run inside the game environment.
