#pragma once

// BDS server-side driver: hosts the DH server for one Level (Java: DhServerWorld + DhServerLevel).
//
// Per dimension (created lazily on the first chunk / player of that dimension):
//   LevelDbLodStore  <dataDir>/worlds/<level>/<dimension>/lods      (ILodStore, docs/STORAGE.md)
//   chunk hash DB    <dataDir>/worlds/<level>/<dimension>/chunkhash (IChunkHashStore over LevelDB)
//   GeneratedProvider + UpdatePropagator + BdsWorldGenerator -> WorldGenerator -> GenQueue (LodRequestModule)
//   ServerRequestHandler (registered with NetServer's ServerNetwork)
// Shared: BlockTable/BlockSerializer (data/*.json from the mod dir), ThreadPools (options), SystemClock,
// PacketTransport + NetServer.
//
// Chunk ingestion (docs/PORTING.md, ChunkHashStore.h recipe):
//   hooks Level::$onChunkLoaded and DBChunkStorage::$saveLiveChunk only note (dimension, chunk) in a pending
//   set (any thread). The server-thread tick drains it under a time budget: re-fetch the chunk from the
//   dimension's ChunkSource (shared_ptr keeps it alive), BedrockChunk -> unchanged-hash skip -> block light
//   over the 3x3 neighbourhood -> createFromChunk -> merge chunks of one section -> provider.updateAsync ->
//   commit the hashes when the update future succeeded.
//
// Ticks: onServerTick() every server tick (20 Hz): config, ServerNetwork::tick(), transport flush;
// every >= 100 ms additionally: player positions, ingestion, LodRequestModule::tick(), UpdatePropagator::tick().
//
// Shutdown order (stop()): hooks off -> transport listener detached -> request handlers closed -> world-gen
// modules/queues closed -> thread pools shut down and joined -> updaters quiesced -> providers closed ->
// stores/DBs destroyed -> pools destroyed.

#include "bedrock_common/BlockSerial.h"
#include "bedrock_common/PacketTransport.h"
#include "bedrock_common/Platform.h"

#include "dh/config/ConfigStore.h"
#include "dh/platform/BlockTable.h"
#include "dh/platform/IClock.h"
#include "dh/thread/SingleThreadExecutor.h"
#include "dh/thread/ThreadPools.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

class Level;
class LevelChunk;

namespace dh::bedrock {

class NetServer;

struct ServerDriverStats {
    std::uint64_t chunksNoted           = 0; ///< chunk load / save events seen
    std::uint64_t chunksIngested        = 0; ///< built and handed to the provider
    std::uint64_t chunksUnchanged       = 0; ///< skipped by the chunk hash
    std::uint64_t chunksSkipped         = 0; ///< not loaded / unsupported height / build failed
    std::uint64_t sectionsUpdated       = 0; ///< provider updates finished successfully
    std::uint64_t sectionsFailed        = 0;
    std::size_t   pendingChunks         = 0;
    std::size_t   dimensions            = 0;
    std::size_t   players               = 0;
    std::uint64_t framesSent            = 0;
    std::uint64_t framesReceived        = 0;
    std::uint64_t generatedChunks       = 0;
    std::uint64_t failedGeneratedChunks = 0;
};

class ServerWorldDriver {
public:
    ServerWorldDriver(Level& level, ModPaths paths, std::filesystem::path modDir);
    ~ServerWorldDriver();
    ServerWorldDriver(const ServerWorldDriver&)            = delete;
    ServerWorldDriver& operator=(const ServerWorldDriver&) = delete;

    /// Loads the block tables, builds pools / network, installs the hooks. False (logged) on failure;
    /// the driver is then inert.
    bool start();
    /// See "Shutdown order" above. Idempotent.
    void stop();
    bool running() const { return running_.load(); }

    /// Server thread, once per server tick.
    void onServerTick();

    /// Hook entry (any thread): remembers that `chunk` was loaded / saved.
    void noteChunk(LevelChunk& chunk);
    /// Server thread: notes every loaded chunk of every dimension again (the `/dh sweep` command; same sweep as at start).
    void sweepAll();

    ServerDriverStats stats() const;
    std::string       statusText() const;

private:
    struct Dim;
    struct Impl;

    Level&                level_;
    ModPaths              paths_;
    std::filesystem::path modDir_;
    std::atomic<bool>     running_{false};
    std::unique_ptr<Impl> impl_;
};

} // namespace dh::bedrock
