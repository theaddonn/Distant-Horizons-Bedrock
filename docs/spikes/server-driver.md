# Server driver (GLUE-3): BDS DLL, packet transport, world-gen adapter

Status: compile-only. `scripts/build.sh client` and `server` both build; nothing here has run against a real BDS.
This page says what was built, what to expect in the log, and what is still a guess.

## What exists

| File | Role |
|---|---|
| `src/bedrock_common/PacketTransport.{h,cpp}` | `dh::ITransport` over a LeviLamina custom packet, compiled into both DLLs |
| `src/server/NetServer.{h,cpp}` | `PacketTransport` -> `ServerNetwork`/`ServerPlayers`/`ServerSettings`, join/leave -> sessions, config change -> `broadcastConfig()` |
| `src/server/ServerWorldDriver.{h,cpp}` | per-dimension LOD store, provider, propagator, request handler, world gen; chunk ingestion; ticks; shutdown order |
| `src/server/BdsWorldGenerator.{h,cpp}` | `dh::IChunkGenerator` for chunks that are not loaded |
| `src/server/DistantHorizonsServer.{h,cpp}` | mod entry: enable/disable, `/dh status`, own log file |
| `src/bedrock_common/BedrockChunk.{h,cpp}` | additive: no longer `final`, new `snapshot()` (materialises blocks, biomes, height maps, light list so the wrapper can leave the thread that owns the `LevelChunk`) |
| `scripts/deploy.sh server` | additive: `BDS_DIR=... scripts/deploy.sh server` |

No core (`src/core`) file was touched.

## Transport path: `ll::network::PacketRegistrar` (not `ScriptMessagePacket`)

* `PacketRegistrar`, `RuntimePacket` and `Packet::sendToServer/sendToClient` are declared in both header sets and
  exported by both `LeviLamina.dll` (client and server package; checked with `strings` on both DLLs), so the same
  packet class serves both roles and no fallback carrier was needed. `ScriptMessagePacket` is therefore unused.
* Packet: `DhFramePacket : ll::network::PacketBase<DhFramePacket>`, registered at static-init under
  `hash(type_unprefix_name)`; payload = one length-prefixed byte string (`BinaryStream::writeString` /
  `ReadOnlyBinaryStream::getString`, binary safe). Reliable-ordered, medium priority, marked incompressible.
  Receive side is `PacketHandlerBase<DhFrameHandler, DhFramePacket>::handlePacket(netId, NetEventCallback&, packet)`.
* `maxFrameBytes() = 32 KiB`. RakNet fragments reliable messages above the MTU (~1.4 KB) and reassembles them; the
  practical ceiling is the Bedrock batch (about 1 MiB compressed). 32 KiB keeps one frame far below that, bounds
  head-of-line blocking against vanilla traffic and costs nothing at the default 500 KB/s player limit. Raise
  `PacketTransport::kMaxFrameBytes` if a measurement shows per-frame overhead matters.
* Inbound events hop to a private `SingleThreadExecutor` ("DH Net Dispatch"): ordered, one at a time, never inline.
  Outbound frames are queued by `send()` (any thread) and sent by `flushOutbound()` on the game thread once per tick.
* Server: `SessionId` per `NetworkIdentifier`, opened by `PlayerJoinEvent`, closed by `PlayerDisconnectEvent`.
  `disconnect(session)` closes only the DH session (the player stays connected; the core sends `CloseReason`
  first and it is still flushed). Client: one session, `clientConnect()` / `clientDisconnect()` (the client driver,
  GLUE-4, must call them and pump `flushOutbound()` from the client tick).

## BDS setup under wine (1.26.51)

1. Download Bedrock Dedicated Server 1.26.51 for Windows (`bedrock-server-1.26.51.x.zip`) into e.g. `~/bds`.
   Install LeviLamina 26.51.6 for the server (`lip install github.com/LiteLDev/LeviLamina#server@26.51.6` or unpack the
   release zip over the BDS directory; it adds `PreLoader.dll`, `LeviLamina.dll`, `plugins/`).
2. `scripts/build.sh server`, then `BDS_DIR=~/bds scripts/deploy.sh server`. Result:
   `plugins/DistantHorizonsServer/{DistantHorizonsServer.dll,manifest.json,data/block_table.json,data/biome_table.json}`.
   (`tooth.json` already places `DistantHorizonsServer/` into `plugins/DistantHorizonsServer/`; the release zip must
   additionally contain `data/*.json`, same as for the client.)
3. Run: `cd ~/bds && wine bedrock_server_mod.exe` (LeviLamina's launcher; plain `bedrock_server.exe` does not load
   plugins). Use `WINEDEBUG=-all` and a 64-bit prefix. Create the world once with the console if needed.
4. Join with the GDK client over LAN/direct IP (`Retail-1.26.5101-Levi` with the client DLL) or any client; the
   client DLL is only needed to receive LODs.
5. Logs: `plugins/DistantHorizonsServer/logs/dh.log` (our file) and the console (LeviLamina logger).
   Config: `plugins/DistantHorizonsServer/config/config.json` (same options as the client, `server.*` section matters).
   Data: `plugins/DistantHorizonsServer/data/worlds/<level>/<dimension>/{lods,chunkhash}`.

## Log lines to expect

```
server: loading
server: enabling (git <sha>)
server driver: block table loaded (N blocks, M biomes)
net: server network started (max frame 32768 bytes)
server driver: started for world '<level name>' (T worker threads, data ...)
server driver: dimension minecraft:overworld ready (level '<level>@minecraft:overworld', -64..320, sky=1, store ...)
net: player '<name>' joined, session 1
server driver: dims=1 players=1 chunks: noted=... ingested=... unchanged=... skipped=... pending=... | sections: ok=... failed=0 | worldgen: chunks=... failed=0 | net: tx=... rx=...
net: player '<name>' left
server driver: stopped
```
`/dh status` (game director permission) prints the same status line. Healthy signs: `ingested` grows while chunks load,
a second run of the same world shows `unchanged` instead (hash store), `sections ok` grows, `failed=0`, and with a mod
client connected `rx` grows (its config message), then `tx` (config + responses).
Bad signs: `block table`/`LOD store` errors at start (path or LevelDB), `exception swallowed` lines (each hook is wrapped),
`world gen: ... returned null`, `failed` counters growing.

## Hooks and APIs used

* `Level::$onChunkLoaded(ChunkSource&, LevelChunk&)` (same hook as the client ingress) and
  `DBChunkStorage::$saveLiveChunk(LevelChunk&)` only add `(dimension, x, z)` to a mutex-guarded pending map.
* Events: `ServerStartedEvent` (create the driver), `ServerStoppingEvent` (stop), `ServerLevelTickEvent` (drive
  `onServerTick()`, 20 Hz), `PlayerJoinEvent` / `PlayerDisconnectEvent`.
* Data: `Level::getLevelData().mLevelName`, `Level::forEachDimension`, `Level::forEachPlayer`,
  `Player::getNetworkIdentifier / getClientSubId / getRealName / getPosition / getDimensionId`,
  `Dimension::getChunkSource()`, `ChunkSource::getAvailableChunk(ChunkPos)` (LLAPI, returns `shared_ptr<LevelChunk>`),
  `LevelChunk::mLoadState`, `mPosition`, `mDimension`.
* World gen: `Dimension::mWorldGenerator` (is a `ChunkSource`): `createNewChunk`, `LevelChunk::tryChangeState`,
  `loadChunk`; fallback `LevelChunk` constructor; `ChunkSource::getOrLoadChunk` for `InternalServer` mode;
  `ll::thread::ServerThreadExecutor::getDefault().execute` for server-thread hops.
* Command: `ll::command::CommandRegistrar::getInstance(false).getOrCreateCommand("dh", ...).overload().text("status")`.

## Tick structure

Every server tick (20 Hz): apply a pending config change (`broadcastConfig()`), `ServerNetwork::tick()` (payload
senders; the bandwidth maths assume 50 ms), `PacketTransport::flushOutbound()`.
Every >= 100 ms: player positions -> `ServerPlayers::setPosition`, chunk ingestion (budget 10 ms / 64 chunks),
`LodRequestModule::tick()` and `UpdatePropagator::tick()` per dimension.
World gen runs while a ready player is in the dimension (`DimListener::shouldDoWorldGen`) at the nearest player.

## Deviations and decisions

* Codec compression is `None` for stored and wire data: `Codec.cpp` still returns `Unsupported` for Zstd.
  Switch `CodecOptions.compression` in `createDim` once zstd is vendored.
* A section is updated once per tick with all its changed chunks merged (`updateFromDataSource`), and a second update
  of the same section waits until the first finished: `ChunkHashTracker` commits per section position, so overlapping
  updates would persist a hash before its data (ChunkHashStore.h recipe, step 3).
* Block light only (`lightChunk`, 3x3 neighbours from `getAvailableChunk`, neighbours that are not loaded are absent);
  sky light is baked by the updater. Native light is not read (see chunk-ingress.md).
* `serverId` is randomised and persisted on first start (option default 0 = not chosen).
* Level name sent to clients: `<world>@<dimension resource>`; level key `[a-zA-Z0-9-_]` sanitised, <= 150 chars.

## What could not be verified offline (all of it must be run)

1. That the dh:frame packet round-trips: registration through `PacketBase`'s static anchor in the DLL, the handler being
   invoked on the client for `sendToClient` and on the server for `sendToServer`, and the packet id not clashing.
   First thing to test: connect a client, look for `net: player ... joined` and `rx`/`tx` counters.
2. `Player::getClientSubId()` / `sendToClient(netId, subId)` for the primary client (sub id 0).
3. `Level::$onChunkLoaded` firing on BDS with the chunk already `Loaded`; `DBChunkStorage::$saveLiveChunk` call
   frequency (if it is rare, edits reach the LODs late; alternative: `BlockChangedEvent`). The ingest retries a chunk up to
   20 x 100 ms until `mLoadState >= Loaded`.
4. World gen: whether `WorldGenerator::createNewChunk` (base `ChunkSource`) produces a usable scratch chunk or returns
   null, whether `loadChunk` expects the `Generating` state (we call `tryChangeState(Unloaded->Generating)` and
   `Generating->Generated`), thread safety of `loadChunk` on a pool worker, and destruction of the scratch chunk.
5. `Features` mode is NOT implemented (structures + decoration need a `ChunkViewSource` over the 3x3 neighbourhood:
   `postProcessStructures`, `decorationPostProcessChunk`); it degrades to Surface with a warning. `InternalServer` and
   `PreExistingOnly` use the real chunk source and block a worker while waiting for the server thread.
6. `BedrockChunk::snapshot()` cost (about 100k `getBlock` + 100k `getBiome` calls per chunk) on the server thread.
7. Shutdown: `ServerStoppingEvent` ordering vs worker threads, `UpdatePropagator` tasks capturing the provider, hooks
   unhooked while chunks are still loading.
8. LevelDB handles: two DBs per dimension (`lods`, `chunkhash`) opened through `ll::data::KeyValueDB`.

## Open questions

* Should a config change of `numberOfThreads` rebuild the pools (core note: fixed at construction)? Currently needs a restart.
* Persist which sections were generated by DH vs the vanilla world (so `PreExistingOnly` can be answered from disk without loading)?
* Disconnect semantics: `ITransport::disconnect` does not tell the client; add a `CloseReason` frame wait or a Bedrock kick?
* Where should the client driver call `clientConnect()`: on level join (before the server joins the player) is safe because the
  client speaks first with its `SessionConfig`.
