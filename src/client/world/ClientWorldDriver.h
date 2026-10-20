#pragma once

// GLUE-4: the client world driver. Bedrock hooks -> dh::DhClientServerWorld / dh::DhClientWorld -> LodQuadTree ->
// MeshUploader (Stage-A renderer). Everything here is untested in game (see docs/spikes/m1-lods.md).
//
// Modes (decided once per session in startSession()):
//   singleplayer  a server Level lives in this process (ll::service::getLevel() / getMinecraft(false)->getLevel()
//                 non-null and !isClientSide()). One DhClientServerWorld; the SERVER dimension is the authoritative
//                 source of chunks and of world generation (BdsWorldGenerator over the server Dimension); the client
//                 dimension only renders.
//   remote        otherwise (HUD mode: "pending" -> "server" when the DH server answers, "client-only" when it stays silent
//                 for ClientNetworkState::kServerReplyTimeoutMs = 5000 ms; a late reply switches to "server" and the loaded
//                 level is reloaded under the LevelInit key; store folder worlds/mp_<host>_<port>). One DhClientWorld over a PacketTransport session ("dh:frame" packets); chunks come from
//                 the ClientLevel (sub-chunk events) and from the DH server (LevelInit / full-data responses).
//
// Threads:
//   game thread     ClientLevelTickEvent (fallback: LodRenderer::renderFrame via onFrame()): refreshes the player
//                   cache, creates level handles, PacketTransport::flushOutbound(), remote chunk ingestion, posts
//                   the worker step. Join/Exit events, key handler.
//   worker thread   "DH Client Driver" (SingleThreadExecutor): level load/unload, WorldTicker::poll() -> world tick
//                   (tree tick, propagator, request modules, delayed saves). Never touches live game objects.
//   server thread   (singleplayer) chunk ingestion drain: chunk fetch + BedrockChunk::snapshot must run on the thread
//                   that owns the chunks; posted through ll::thread::ServerThreadExecutor.
//   render thread   LodRenderer::renderFrame: collectRenderList() under a try-shared lock.
//   pool threads    everything inside the core (ThreadPools).
//
// Shutdown order (stopSession, PORTING.md): ingestion hooks off + drained -> worker stopped -> render lock taken
// exclusively -> world.close() (levels: request modules/trees -> save cache flush -> executors drained -> provider)
// -> world destroyed (network state detaches the transport listener) -> transport shut down -> thread pools shut
// down and joined -> handles/serializer destroyed.
//
// Keys (render hooks): F5 whole pipeline on/off, Shift+F5 synthetic test feed, Shift+F7 chunk ingestion on/off.

#include "dh/render/RenderList.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dh::bedrock {

enum class RenderListStatus {
    NoSession,   ///< pipeline off / no session: the caller may draw whatever the uploader holds (test feed)
    Unavailable, ///< session exists but no level is renderable right now (loading, shutting down): draw nothing
    Ready,       ///< `positions` is the near-to-far list of sections to draw
};

struct RenderListResult {
    std::vector<std::uint64_t> positions;              ///< sec::Pos, near to far
    std::int32_t               minY = -64, maxY = 320; ///< level height range (maxY exclusive)
};

class ClientWorldDriver {
public:
    static ClientWorldDriver& instance();

    /// Registers events and hooks (all passthrough until the pipeline is on). Idempotent.
    void install();
    /// Stops the session and removes everything.
    void uninstall();

    /// F5. Turning it on starts a session as soon as a local player exists; off tears the session down.
    void setPipeline(bool on);
    bool pipeline() const;

    /// Shift+F7.
    void setIngestion(bool on);
    bool ingestion() const;

    /// Render thread, once per frame, before drawing. Builds the render list of the active level.
    /// `params.options`, `worldMinY` and `worldMaxY` are filled in here; the caller supplies matrices and camera.
    RenderListStatus collectRenderList(dh::RenderListParams params, bool coreFrustum, RenderListResult& out);

    /// Render thread: fallback pump when ClientLevelTickEvent is not delivered.
    void onFrame();

    /// Level key from the network layer for a remote client dimension (LevelNaming.h).
    void onLevelKey(const std::string& dimensionResource, const std::string& levelKey);

    struct Impl;

private:
    ClientWorldDriver();
    ~ClientWorldDriver();
    std::unique_ptr<Impl> impl_;
};

} // namespace dh::bedrock
