# Server stream notes

## Test server (scripts/bds.sh)

Headless Bedrock Dedicated Server + LeviLamina under wine, for loading `DistantHorizonsServer.dll` without the game.
Everything lives under `BDS_ROOT` (default `~/.cache/dh-bds/1.26.51.1`); downloads are cached in `~/.cache/dh-bds/downloads/`.

```
scripts/build.sh server
scripts/bds.sh setup         # download + sha256 check + unpack + PeEditor + install mod (idempotent)
scripts/bds.sh start         # wine bedrock_server_mod.exe, stdin from FIFO $BDS_ROOT/console.in, console in console.log
scripts/bds.sh cmd "dh status"
scripts/bds.sh log [-f]      # console.log + plugins/DistantHorizonsServer/logs/dh.log
scripts/bds.sh stop          # sends "stop", waits up to 40 s, then kills; status / install-mod also exist
```

Versions (all sha256 verified by the script):

| Part | Source | sha256 |
|---|---|---|
| BDS 1.26.51.1 win | `https://www.minecraft.net/bedrockdedicatedserver/bin-win/bedrock-server-1.26.51.1.zip` (browser User-Agent needed) | a1c1ebefde3036223234179f7c1eab4b445b4727f0482a0554818c9bcaf8e4a5 |
| LeviLamina 26.51.6 server | `https://github.com/LiteLDev/LeviLamina/releases/download/v26.51.6/levilamina-v26.51.6-server-release-windows-x64.zip` | d2277ac4ec042e3498f00160ce1aa0e2dcec23f2b9a902925ac9b27a3f0feb40 |
| PreLoader 1.16.3 | `https://github.com/LiteLDev/PreLoader/releases/download/v1.16.3/preloader-v1.16.3-windows-x64.zip` | 31dc9419d4a8c1c382d6c2629243757a28141c878b636d17b92b1b2fd0c3af1a |
| bedrock-runtime-data 26.51.1-server.7 | `https://github.com/LiteLDev/bedrock-runtime-data/releases/download/v26.51.1-server.7/bedrock-runtime-data-v26.51.1-server.7-windows-x64.zip` | 44e548f693b2eb1bd5ccd8b56169cdbf2b9f01e7d1833717c6351bd8f7a2e385 |
| PeEditor 3.9.2 | `https://github.com/LiteLDev/PeEditor/releases/download/v3.9.2/peeditor-windows-x64.zip` | cfeba1e9c40fc65a1310853d5cd3d5431d7bc3855c68e23b3d9c7a26f1fe42c2 |

Install layout (from LeviLamina's tooth.json, server variant):

```
<BDS_ROOT>/bedrock_server_mod.exe      (made by `wine PeEditor.exe -mb`; bedrock_server.exe is renamed to .bak)
<BDS_ROOT>/PreLoader.dll
<BDS_ROOT>/bedrock_runtime_data
<BDS_ROOT>/plugins/LeviLamina/{LeviLamina.dll,manifest.json}
<BDS_ROOT>/plugins/DistantHorizonsServer/{DistantHorizonsServer.dll,.pdb,manifest.json,data/block_table.json,data/biome_table.json}
<BDS_ROOT>/server.properties           (level-name=dhtest, creative, cheats, operator, LAN visibility, transport=nethernet)
```

Native mods are discovered in `plugins/<name>/manifest.json` (`type: native`, `platform: server`). Not installed:
CrashLogger (LeviLamina logs `Couldn't Create CrashLogger Daemon Process ... File not found`, harmless) and
levilamina-loc (logs `i18n load failed ... plugins/LeviLamina\lang`, harmless). Without `transport=nethernet`
1.26.51 prints a TRANSPORT TYPE ERROR (no player could join); the script sets it.

Observed (first start, then `dh status`, then `stop`; ANSI colours stripped):

```
[LeviLamina] Loading DistantHorizonsServer v0.1.0
[DistantHorizonsServer] server: loading
[LeviLamina] DistantHorizonsServer loaded
[LeviLamina] 1 mod(s) loaded
[Server] Version: 1.26.51.1(ProtocolVersion 2193) with LeviLamina-26.51.6
[Server] Server started in (30.7s)! For help, type "help" or "?"
[LeviLamina] Enabling DistantHorizonsServer v0.1.0
[DistantHorizonsServer] server: enabling (git 70ea5fb)
[DistantHorizonsServer] server driver: block table loaded (1531 blocks, 89 biomes)
[DistantHorizonsServer] net: server network started (max frame 32768 bytes)
[DistantHorizonsServer] world gen: configured 14 scratch worker thread(s) (started on first use)
[DistantHorizonsServer] server driver: dimension minecraft:overworld ready (level 'dhtest_minecraft_overworld', -64..320, sky=1, store ./plugins/DistantHorizonsServer\data\worlds\dhtest\minecraft_overworld)
[DistantHorizonsServer] server driver: started for world 'dhtest' (8 worker threads, data ./plugins/DistantHorizonsServer\data)
[LeviLamina] 1 mod(s) enabled in (1.0s)
[Server] Distant Horizons: dims=1 players=0 chunks: noted=0 ingested=0 unchanged=0 skipped=0 pending=0 | sections: ok=0 failed=0 | worldgen: chunks=0 failed=0 | net: tx=0 rx=0 frames
[Server] Server stop requested.
[DistantHorizonsServer] server driver: stopped
[LeviLamina] Disabling DistantHorizonsServer v0.1.0
[DistantHorizonsServer] server: disabling
Quit correctly
```

Also logged (expected, not errors in our code): `config: generatorPlan needs a rough surface generator ... using ChunksOnly` and
`config: distant generation forced to Disabled until scratch chunk generation is validated in game`.
Not verified: a real client joining, chunk ingest/LOD streaming with a player, and the online-mode login path.
