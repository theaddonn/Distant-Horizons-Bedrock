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
[DistantHorizonsServer] server: enabling (git 53ed5ee)
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

## Join checklist (user, Run 15)

The agents cannot drive the client, so the join is a user check. Two runs: **A** joins the BDS that has the server mod
(mode `server`). **B** joins a BDS without the mod (mode `client-only`). Use the same client build (the DLL from this branch)
for both.

### Prepare (Linux shell, repo root)

```
scripts/build.sh server
scripts/bds.sh setup            # first time only; afterwards: scripts/bds.sh install-mod
scripts/bds.sh start
scripts/bds.sh log | grep -E "net: server network started|transport: server packet|server driver: started|effective server settings"
```
Optional, for distant terrain beyond what the server has loaded: set `"allowUnvalidatedGenerator": true` in
`~/.cache/dh-bds/1.26.51.1/plugins/DistantHorizonsServer/config/config.json` and restart (`scripts/bds.sh stop`, then `start`).
Without it the server only serves sections it built from its own loaded chunks.

### Join (game)

1. Start the client with the mod as usual.
2. Play -> **Servers** tab -> **Add Server**: name `DH Test`, address `127.0.0.1`, port `19132` -> Save -> join it.
   The server also announces itself on the LAN, so it may show up under **Friends -> LAN Games** as `DH Test Server`. Joining from
   there is fine too.
3. The world is `dhtest` (creative, seed 12345, the player is operator). Stand still for 30 s, then fly about 500 blocks in one
   direction and wait 30 s more.
4. In chat: `/dh status` (server status line). Press F8 for the HUD dump.
5. Leave the world (Save & Quit), then run `scripts/bds.sh log > ~/dh-run15-A-server.log` on Linux.

### Run B: client-only

`scripts/bds.sh stop`, then move the mod folder away (`mv ~/.cache/dh-bds/1.26.51.1/plugins/DistantHorizonsServer ~/.cache/dh-bds/dhs-off`),
`scripts/bds.sh start`, and repeat the join steps 2, 3 and 5. Afterwards move the folder back
(`mv ~/.cache/dh-bds/dhs-off ~/.cache/dh-bds/1.26.51.1/plugins/DistantHorizonsServer`).

### Expected log lines

Server (`scripts/bds.sh log`), run A:
```
transport: server packet 'dh::bedrock::wire::DhFramePacket' runtime id <ID> (0x...), max frame 32768 bytes
net: server network started (max frame 32768 bytes)
net: effective server settings: playerBandwidthLimit=500 KB/s ...
net: player '<gamertag>' joined, session 1
transport: server received its first dh:frame (<n> bytes)
transport: server sent its first dh:frame (<n> bytes)
Distant Horizons: dims=1 players=1 chunks: noted=... ingested=... | sections: ok=... failed=0 | ... | net: tx=<growing> rx=<growing> frames
net: player '<gamertag>' left
```
Client (`dh.log` of the client mod), run A:
```
transport: client packet 'dh::bedrock::wire::DhFramePacket' runtime id <same ID as the server> (0x...), max frame 32768 bytes
client driver: remote world folder 'mp_127.0.0.1_19132' from connection info (host '127.0.0.1', port 19132)
client driver: session started: REMOTE (DhClientWorld over dh:frame), world 'mp_...', ...
client driver: mode = pending (config sent, waiting for a DH reply)
transport: client sent its first dh:frame (<n> bytes)
transport: client received its first dh:frame (<n> bytes)
client driver: mode = server (session config received after <N> ms)
net: rx FullDataSourceResponse pos=<p> bytes=<b> (#1)   ... first 20, then every 100th
```
Client, run B (server without the mod):
```
client driver: mode = pending (config sent, waiting for a DH reply)
client driver: mode = client-only (no DH reply within 5000 ms)
```
and no `net: rx` lines. The folder line may show `from level identity (name '...', id '...')` instead of connection info if the
address is not available. Either is fine; report which one.

### PASS criteria

- A1: both sides log the **same** packet runtime id.
- A2: the client shows `mode = server` within a few seconds of joining, and the HUD `mode` is `server`.
- A3: `net: rx FullDataSourceResponse` lines appear, the HUD `net.sections` grows, and the server status line shows `tx` and `rx`
  growing and `failed=0`.
- A4: LODs are drawn around the loaded area, including terrain the client has not loaded itself (with the generator flag on: far
  beyond the vanilla view distance).
- A5: a second join to the same server logs `store: N sections loaded` with N > 0 for the `mp_...` folder (local cache), and
  LODs show up immediately.
- B1: `mode = client-only` after about 5 s, and the HUD `mode` is `client-only`.
- B2: LODs build from the chunks the client receives (they grow as you fly), with no errors and no crash, and no
  `world gen` / `BdsWorldGenerator` lines in the client log.
- Both: leaving the world does not crash, and re-joining works.

### Send back

- `~/dh-run15-A-server.log` (and the B one if it shows anything unusual).
- The client `dh.log` for both runs, or the lines that match `client driver|net:|transport:|store:|ERR|exception`.
- The HUD dump (F8) or a screenshot with the `mode`, `net.rx`, `net.tx` and `net.sections` keys, plus one screenshot of LODs per run.
- For any failure: what you saw on the screen (disconnect message, kick, freeze).

Note: the `dh:frame` packet id is derived from the packet class name, which moved to `dh::bedrock::wire` in this run. Client and
server DLLs from before and after that change cannot talk to each other: always update both together.
