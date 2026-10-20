# Server driver (GLUE-3): BDS DLL, packet transport, world-gen adapter

Status: loads and enables on a real BDS 1.26.51.1 under wine (scripts/bds.sh); no client has joined yet.
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

Verified 2026-10-20 with `scripts/bds.sh` (see `docs/spikes/server-stream.md`, "Test server"): `scripts/bds.sh setup`
downloads BDS 1.26.51.1, LeviLamina 26.51.6 server, PreLoader 1.16.3, bedrock-runtime-data and PeEditor, runs
`wine PeEditor.exe -mb` (creates `bedrock_server_mod.exe`; plain `bedrock_server.exe` does not load mods), installs the mod to
`plugins/DistantHorizonsServer/{DistantHorizonsServer.dll,manifest.json,data/*.json}`, and `scripts/bds.sh start` runs it headless.
Manual deploy into another BDS: `BDS_DIR=... scripts/deploy.sh server`. Set `transport=nethernet` in server.properties.
Logs: `plugins/DistantHorizonsServer/logs/dh.log` and the console. Config: `plugins/DistantHorizonsServer/config/config.json`.
Data: `plugins/DistantHorizonsServer/data/worlds/<level>/<dimension>/{lods,chunkhash}`. Join over LAN/direct IP with the client DLL.

## Log lines to expect

```
server: loading
server: enabling (git <sha>)
server driver: block table loaded (N blocks, M biomes)
transport: server packet 'dh::bedrock::wire::DhFramePacket' runtime id 17321610017666881597 (0xf062b88988e1d83d), max frame 32768 bytes
net: server network started (max frame 32768 bytes)
net: effective server settings: playerBandwidthLimit=500 KB/s (25001 B/tick) globalBandwidthLimit=0 KB/s (0 B/tick) generationRequestRateLimit=20 ... generatorPlan=3 chunkGeneratorMode=2 ... frameBytes=32768 (0 B/tick = unlimited)
server driver: dimension minecraft:overworld initial sweep: N chunk map entries, M loaded chunks queued
server driver: started for world '<level name>' (T worker threads, data ...)
server driver: dimension minecraft:overworld ready (level '<level>@minecraft:overworld', -64..320, sky=1, store ...)
net: player '<name>' joined, session 1            (or: net: player '<name>' already online, session N, for players present at enable)
transport: server received its first dh:frame (N bytes)   / transport: server sent its first dh:frame (N bytes)
net: session N changed level 'a' -> 'b'            (SessionConfig is re-sent: the generation centre depends on the dimension scale)
server driver: dims=1 players=1 chunks: noted=... ingested=... unchanged=... skipped=... pending=... | sections: ok=... failed=0 | worldgen: chunks=... failed=0 | net: tx=... rx=...
net: player '<name>' left
server driver: final drain: N pending at stop, M left
server driver: stopped
```
`/dh sweep` (game director) re-notes every loaded chunk of every dimension (same sweep as at start; logs the counts).
The client DLL logs the same `transport:` lines with `client` (compare the runtime ids: they must be identical).
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

## Review checklist (GLUE-3 follow-ups; verify in game)

Fixed after review: pool shutdown dropping queued world-gen tasks (RAII `TaskGuard` + `BdsWorldGenerator::close()` called before the pools are joined),
`Loaded` required on every snapshot path, one level-naming rule (`LevelNaming.h`), no double `onDisconnected`, `closing_` snapshot taken with the
outbound swap, hook result checks, all dimensions created at start and on join, 100 ms accumulator, status line only on change, settings
written on the dispatch thread.

Fixed 2026-10-20 (BDS-verified unless stated): P1 (packet name + runtime id logged per side, first sent / received frame logged per side; the
client side is still to be seen with a real join), P2 (initial sweep over `ChunkSource::getChunkMap()` when a dimension is created, plus
`/dh sweep`; verified with a ticking area: `357 chunk map entries, 98 loaded chunks queued`; at start the map is empty because no chunk is loaded
yet), final drain of `pending` (bounded 2 s, then waits up to 3 s for the section saves) before the hooks come off, no hash commit while
stopping (a rejected update completes `Ok(false)`, which used to commit hashes for data that was never written), round-robin ingest cursor (no
starvation of high coordinates), packet class in a named namespace (`dh::bedrock::wire`, the anonymous one put "(anonymous namespace)" into the id text),
players already online at enable get a session, the level name/position is set at join (SessionConfig before the first 100 ms tick had no level), a
level change re-sends the SessionConfig.

Still to check (reviewer items, abridged):

* P12 `ServerPlayers::settings` is read by core worker threads (request handlers); only the dispatch-thread read is synchronised.
* P17 `loadChunk` on a pool worker: `WorldGenerator` keeps a thread-local `ChunkAccessor*` heightmap provider
  (`mThreadLocalHeightmapProvider`); vanilla sets it per generation task. If scratch generation crashes or yields flat terrain on workers, run it on
  vanilla's worker context or the server thread.
* `/dh` command: `getOrCreateCommand` is called once per mod enable; after disable/enable (hot reload) check that it is not registered twice and that
  the lambda does not outlive `this`.
* Chunks saved by the engine after `ServerStoppingEvent` (the final level save) are not ingested; `pending` is drained before unhooking only.
* Hook result convention: `hook()` returning 0 = success is assumed.

## Additional in-game checklist items (from the GLUE-3 review)

- [ ] Packet id identical in both DLLs (derived from the class name text; keep the class in a named namespace). Log the id on the first frame on each side.
- [ ] Registrar has no unregister: factory/handler dangle if the mod DLL is unloaded — never hot-unload the DLL.
- [ ] Sub-client id on `sendToClient`; split-screen players share one session per `NetworkIdentifier`.
- [ ] Frame cap 32 KiB vs `getString(maxLen)` 33 KiB: an oversized frame fails the packet read and may disconnect that client (sender-guarded).
- [ ] `flushOutbound` sends everything queued per tick — watch `tx` counters and the bandwidth limiter under a burst.
- [x] Hot-enable / late start: sessions are opened for players already online (not testable without a client).
- [ ] `saveLiveChunk` fires on unload too; a chunk saved at unload may be `Missing` when re-fetched 100 ms later (self-heals on next load) — consider snapshotting in the hook.
- [ ] Ingest/snapshot cost on the server thread (up to 16 chunks per section group; ~98k block + biome reads each) — measure ms per group.
- [x] `pending` map is coordinate-ordered: ingest now starts at a round-robin cursor.
- [x] Hash recipe: `Ok(false)` from the update future = "already up to date" and keeps its commit (ChunkHashStore.h), but never while stopping.
- [ ] `ServerLevelTickEvent` cadence: once per game tick (20 Hz) or once per dimension?
- [ ] `createNewChunk` on `WorldGenerator` returns non-null? (fallback ctor path logged)
- [ ] `Features` currently falls back to `Surface` (logged once) — it is the DEFAULT mode, so the Surface path is the default path.
- [ ] Scratch `LevelChunk` destructor safety (touches `Dimension`/`Level`?).
- [ ] Core request handlers read `ServerPlayers::settings` on worker threads unsynchronised (documented) — runtime config changes while players are connected.
- [ ] Nether coordinate scale uses dimension id 1 = 8.0.

## Server mod completion (2026-10-20)

* **Java protocol parity (checked against `multiplayer/server/ServerPlayerState`, `ServerPlayerStateManager`):** LevelInit is client driven in Java
  (the client sends `RequestLevelInit(dimension)` after joining / changing level, `ServerPlayerState` answers with `LevelInit`, nothing is sent
  unprompted); the core `ServerNetwork` does the same, so no extra "LevelInit on join" is needed on the server. Java sends no `CloseReason` at
  shutdown or leave either (only for an incompatible protocol, which the core does). Gaps fixed: the SessionConfig of a player is derived from his
  current level (generation centre / coordinate scale): the level is set at join and a dimension change re-sends the config (`ServerNetwork::resendConfig`),
  players present at enable get sessions. Not done: no `CloseReason("server stopping")` at disable (Java has none).
* **Config:** `<modDir>/config/config.json` is created with the full default option tree on first start (verified). The safety overrides in
  `ConfigBridge` (`generatorPlan` -> `ChunksOnly` / `Disabled` while `allowUnvalidatedGenerator=false`) no longer overwrite the user's plan in the file;
  they only apply to the running process. `server.*` options feed `NetServer::settingsFrom`; the core applies them (`PayloadSender`:
  bytes/tick = rate*1000/20+1, 0 = unlimited; share = min(player, global / max(active senders, 1)); chunk size from `ITransport::maxFrameBytes()`;
  request distance/rate limits in `ServerRequestHandler`). One line at start (and after a config change) shows the effective values.
* **Distant generation on the server:** the shipped default is the same as the client's (`allowUnvalidatedGenerator=false` in `Options.h`, nothing in
  PORTING flips it), so the server stays Disabled by default, with the warning in the log. With `allowUnvalidatedGenerator=true` the server runs the
  same `BdsWorldGenerator` (scratch workers + server-thread hops, Features mode) and only generates while a ready player is in the dimension
  (`DimListener::shouldDoWorldGen`). Verified on the BDS with no player: starts and idles, `worldgen: chunks=0 failed=0`, no crash. Generation with a
  player online is not verified on the server.
* **Packaging:** `xmake.lua` `after_build` copies `data/*.json` into `bin/<mod>/data/`, so the CI artifact and the release zip (which zip `bin/`) contain
  the tables the drivers read from `<modDir>/data`; `tooth.json` (server variant) places `DistantHorizonsServer/` at `plugins/DistantHorizonsServer/` and
  preserves `config/config.json`; `scripts/deploy.sh server` and `scripts/bds.sh install-mod` copy the same files.
* **BDS shutdown under wine:** with chunks loaded (ticking area) the BDS process does not exit after "Disabling mods" even with the mod removed
  (reproduced without the mod), so `scripts/bds.sh stop` falls back to a kill; the mod itself logs `final drain` and `stopped` first.
  Before killing, `save hold` / `save resume` flushes the world. Do not leave a ticking area in the shared test world.
