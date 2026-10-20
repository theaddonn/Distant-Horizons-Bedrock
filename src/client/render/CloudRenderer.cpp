#include "client/render/CloudRenderer.h"

#include "bedrock_common/FileLogger.h"
#include "client/third_party/TinyPng.h"

#include "dh/pos/SectionPos.h"

#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/MeshContext.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <variant>

namespace dh::render {

namespace {

using Texture = std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture>;
using Clock   = std::chrono::steady_clock;

constexpr char kCloudTexture[] = "data/resource_packs/vanilla/textures/environment/clouds.png";
constexpr char kCloudArchive[] = "data/resource_packs/vanilla/__brarchive/textures/environment.brarchive";

std::string exeDirectory() {
    wchar_t w[MAX_PATH * 2];
    DWORD   n = GetModuleFileNameW(nullptr, w, static_cast<DWORD>(std::size(w)));
    if (!n || n >= std::size(w)) return {};
    std::wstring ws(w, n);
    auto         p = ws.find_last_of(L"\\/");
    if (p == std::wstring::npos) return {};
    ws.resize(p + 1);
    std::string out;
    for (wchar_t c : ws) out.push_back(c < 128 ? static_cast<char>(c) : '?'); // game dirs are ASCII; '?' fails to open
    return out;
}

std::uint32_t to8(float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

} // namespace

CloudRenderer::CloudRenderer(MeshUploader& sink) : sink_(sink), t0_(Clock::now()) {}

CloudRenderer::~CloudRenderer() {
    if (jobRunning_) job_.wait();
}

bool CloudRenderer::isCloudSection(std::uint64_t pos) { return sec::detail(pos) == 0; }

double CloudRenderer::offsetX() const {
    const double t = std::chrono::duration<double>(Clock::now() - t0_).count();
    return -static_cast<double>(kDriftBlocksSec) * t; // vanilla drifts towards -X
}

void CloudRenderer::loadPattern() {
    patternTried_ = true;
    // The vanilla pack is read-only game data: <game dir>/data/resource_packs/vanilla/textures/environment/clouds.png
    // 1.26.51 ships no loose file: the texture lives in __brarchive/textures/environment.brarchive (Run 13).
    const std::string dir = exeDirectory();
    for (std::string path :
         {dir + kCloudArchive, std::string(kCloudArchive), dir + kCloudTexture, std::string(kCloudTexture)}) {
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::uint8_t const*       png = bytes.data();
        std::size_t               len = bytes.size();
        if (path.size() >= 10 && path.compare(path.size() - 10, 10, ".brarchive") == 0) {
            std::size_t off = 0;
            if (!cloud::findInBrArchive(bytes.data(), bytes.size(), "clouds.png", off, len)) {
                DH_LOG_W("clouds: %s has no readable clouds.png entry", path.c_str());
                continue;
            }
            png = bytes.data() + off;
            path += ":clouds.png";
        }
        tinypng::Image img;
        if (!tinypng::decode(png, len, img)) {
            DH_LOG_W("clouds: %s is not a supported PNG; trying the next source", path.c_str());
            continue;
        }
        auto pat = cloud::patternFromRgba(img.rgba.data(), img.width, img.height);
        DH_LOG_I(
            "clouds: pattern from %s (%dx%d, %zu/%zu cloud texels)",
            path.c_str(),
            pat.w,
            pat.h,
            pat.count(),
            pat.solid.size()
        );
        pattern_ = std::make_shared<const cloud::Pattern>(std::move(pat));
        return;
    }
    DH_LOG_W("clouds: clouds.png not found under '%s'; using the procedural pattern", dir.c_str());
    pattern_               = std::make_shared<const cloud::Pattern>(cloud::proceduralPattern());
    stats_.fallbackPattern = true;
}

namespace {
std::uint64_t tileKey(int ti, int tj) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ti)) << 32) | static_cast<std::uint32_t>(tj);
}
} // namespace

void CloudRenderer::dropTile(Tile const& t) {
    for (auto const& g : t.g)
        if (g.quads) sink_.removeIf(g.pos, g.id);
}

void CloudRenderer::release() {
    for (auto const& [k, t] : tiles_) dropTile(t);
    tiles_.clear();
    haveY_       = false;
    stats_.drawn = false;
    stats_.tiles = stats_.quads = stats_.tilesDrawn = stats_.drawCalls = stats_.quadsDrawn = 0;
}

bool CloudRenderer::update(Inputs const& in) {
    const auto  now  = Clock::now();
    auto const& secs = sink_.sections();
    stats_.tilesDrawn = stats_.drawCalls = stats_.quadsDrawn = 0; // refilled by draw()

    if (jobRunning_ && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        jobRunning_ = false; // before get(): a throwing job must not leave an invalid future marked running
        try {
            Batch b = job_.get();
            for (auto& t : b.tiles) {
                t.since = now;
                auto k  = tileKey(t.ti, t.tj);
                auto it = tiles_.find(k);
                if (it != tiles_.end()) dropTile(it->second); // cannot happen (only missing tiles are built)
                tiles_[k] = std::move(t);
            }
            stats_.lastBuildMs  = b.ms;
            totalBuildMs_      += b.ms;
            stats_.builds      += b.tiles.size();
            if (b.failed) {
                retryAfter_ = now + std::chrono::seconds(5);
                DH_LOG_E("clouds: tile build failed; retrying in 5 s");
            }
        } catch (...) {
            retryAfter_ = now + std::chrono::seconds(5);
            DH_LOG_E("clouds: build job threw; retrying in 5 s");
        }
    }

    if (!in.enable || in.farBlocks <= 0.0f) {
        release();
        return false;
    }

    if (!patternTried_) loadPattern();
    if (!pattern_ || pattern_->empty()) return false;

    const double radius = std::min<double>(in.farBlocks, 15000.0);
    const double offX   = offsetX();
    const double camX = in.camera.x - offX, camZ = in.camera.z; // camera in cloud space
    const int    wantY  = static_cast<int>(std::lround(in.cloudY - in.minY));
    tint_               = in.tint; // per draw, continuous (no rebuild)
    minY_               = in.minY;
    const double tb     = static_cast<double>(kTileBlocks);

    // classify the live tiles: promote finished ones, drop lost / stale / out-of-range ones
    std::size_t ready = 0, quads = 0;
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        Tile& t    = it->second;
        bool  drop = t.yBottom != wantY;
        if (!drop) {
            const double d = cloud::rectDistance(t.ti * tb, t.tj * tb, (t.ti + 1) * tb, (t.tj + 1) * tb, camX, camZ);
            drop           = d > radius + tb; // hysteresis: one tile beyond the build radius
        }
        bool allReady = true;
        for (auto const& g : t.g) {
            if (!g.quads || drop) continue;
            const int st = g.state->load();
            if (st == 2 || (st == 1 && !secs.count(g.pos))) drop = true; // dropped, or cleared (world exit)
            else if (st == 0) allReady = false;
        }
        if (!drop && !allReady && now - t.since > std::chrono::seconds(10)) {
            drop        = true; // upload never completed
            retryAfter_ = now + std::chrono::seconds(1);
        }
        if (drop) {
            dropTile(t);
            it = tiles_.erase(it);
            continue;
        }
        t.ready = allReady;
        if (allReady) {
            ++ready;
            quads += t.quads;
            haveY_ = true;
            liveY_ = t.yBottom;
        }
        ++it;
    }
    if (ready == 0) haveY_ = false;
    stats_.tiles = ready;
    stats_.quads = quads;

    // missing tiles within the radius, nearest first
    struct Want {
        int    ti, tj;
        double d;
    };
    std::vector<Want> missing;
    {
        const int i0 = static_cast<int>(std::floor((camX - radius) / tb));
        const int i1 = static_cast<int>(std::floor((camX + radius) / tb));
        const int j0 = static_cast<int>(std::floor((camZ - radius) / tb));
        const int j1 = static_cast<int>(std::floor((camZ + radius) / tb));
        for (int tj = j0; tj <= j1; ++tj)
            for (int ti = i0; ti <= i1; ++ti) {
                const double d = cloud::rectDistance(ti * tb, tj * tb, (ti + 1) * tb, (tj + 1) * tb, camX, camZ);
                if (d <= radius && !tiles_.count(tileKey(ti, tj))) missing.push_back({ti, tj, d});
            }
        std::sort(missing.begin(), missing.end(), [](Want const& a, Want const& b) { return a.d < b.d; });
    }

    if (!missing.empty() && !jobRunning_ && now >= retryAfter_) {
        if (missing.size() > static_cast<std::size_t>(kTilesPerJob)) missing.resize(kTilesPerJob);
        cloud::TileParams tp;
        tp.cell      = kCellBlocks;
        tp.yBottom   = static_cast<float>(wantY);
        tp.thickness = kThickness;
        tp.fancy     = true;
        // white mesh (face shading only): the sky tint is applied per draw by the VS (draw(), u_model[0][3][0])
        tp.r = tp.g = tp.b = 255;
        tp.a               = static_cast<std::uint8_t>(std::lround(kAlpha * 255.0f));
        auto          pat  = pattern_;
        MeshUploader* sink = &sink_;
        job_               = std::async(std::launch::async, [pat, tp, missing, sink, wantY]() {
            Batch      batch;
            const auto t0 = Clock::now();
            for (auto const& w : missing) {
                Tile t;
                t.ti      = w.ti;
                t.tj      = w.tj;
                t.yBottom = wantY;
                try {
                    cloud::TileParams p = tp;
                    p.ti                = w.ti;
                    p.tj                = w.tj;
                    auto built          = cloud::buildTile(*pat, p);
                    try {
                        for (int f = 0; f < cloud::kFaceGroups; ++f) {
                            auto& bf = built[static_cast<std::size_t>(f)];
                            if (bf.quads == 0) continue;
                            Group& g = t.g[static_cast<std::size_t>(f)];
                            g.quads  = bf.quads;
                            g.state  = std::make_shared<std::atomic<int>>(0);
                            // ti * 8 + group: tiny and signed-safe in the 28-bit x field
                            g.pos = sec::encode(0, static_cast<std::int32_t>(w.ti * 8 + f), static_cast<std::int32_t>(w.tj));
                            MeshBuffers mb;
                            for (auto& v : bf.buffers) mb.transparent.push_back(std::move(v));
                            auto st = g.state;
                            g.id = sink->uploadWithId(g.pos, std::move(mb), [st](bool ok) { st->store(ok ? 1 : 2); });
                            t.quads += bf.quads;
                        }
                    } catch (...) {
                        for (auto const& g : t.g)
                            if (g.quads && g.id) sink->removeIf(g.pos, g.id);
                        throw;
                    }
                } catch (...) {
                    batch.failed = true;
                    break;
                }
                batch.tiles.push_back(std::move(t));
            }
            batch.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            return batch;
        });
        jobRunning_        = true;
    } else if (missing.empty() && !jobRunning_ && !loggedFirst_ && ready > 0 && ready == tiles_.size()) {
        loggedFirst_ = true;
        DH_LOG_I(
            "clouds: first tiles live: %zu tiles, %zu quads, built in %.1f ms (tile %d blocks, radius %.0f)",
            ready,
            quads,
            totalBuildMs_,
            kTileBlocks,
            radius
        );
    }
    stats_.drawn = ready > 0;
    return ready > 0;
}

std::size_t CloudRenderer::draw(
    mce::MeshContext&       mctx,
    mce::MaterialPtr const& material,
    glm::dvec3 const&       camera,
    bool                    cameraRelative,
    float                   fogStart,
    float                   fogEnd,
    Frustum5 const&         frustum
) {
    stats_.tilesDrawn = stats_.drawCalls = stats_.quadsDrawn = 0;
    if (!haveY_) return 0;
    auto const&          secs = sink_.sections();
    static Texture const none{std::monostate{}};

    auto&        cam  = mctx.camera;
    auto         wref = cam.worldMatrixStack->push(false);
    const double cx = cameraRelative ? camera.x : 0.0, cy = cameraRelative ? camera.y : 0.0,
                 cz = cameraRelative ? camera.z : 0.0; // absolute mode: the view matrix carries the camera
    const bool   cull    = cullsBackFaces(camera.y, cameraRelative);
    const double offX    = offsetX();
    const double slabBot = static_cast<double>(slabBottom()), slabTop = slabBot + static_cast<double>(kThickness);
    const double tb      = static_cast<double>(kTileBlocks);

    std::size_t calls = 0;
    for (auto const& [key, t] : tiles_) {
        if (!t.ready || t.quads == 0) continue;
        const double x0 = t.ti * tb + offX, x1 = x0 + tb, z0 = t.tj * tb, z1 = z0 + tb;
        // fully faded: the shader fades by 3D distance, which is >= the horizontal distance tested here
        if (fogEnd > 0.0f && cloud::rectDistance(x0, z0, x1, z1, camera.x, camera.z) >= static_cast<double>(fogEnd))
            continue;
        if (frustum.valid) {
            const glm::vec3 lo{
                static_cast<float>(x0 - cx) - 0.5f,
                static_cast<float>(slabBot - cy) - 0.5f,
                static_cast<float>(z0 - cz) - 0.5f};
            const glm::vec3 hi{
                static_cast<float>(x1 - cx) + 0.5f,
                static_cast<float>(slabTop - cy) + 0.5f,
                static_cast<float>(z1 - cz) + 0.5f};
            if (!frustum.visible(lo, hi)) continue;
        }
        const unsigned skip =
            cull ? cloud::backFaceGroupMask(camera.x, camera.y, camera.z, x0, x1, z0, z1, slabBot, slabTop) : 0u;

        Matrix m = Matrix::IDENTITY();
        m.translate(static_cast<float>(x0 - cx), static_cast<float>(-cy), static_cast<float>(z0 - cz));
        // DH_Terrain per-draw parameters in the projective row: no near clip / fade, DH far fog
        auto& g = m._m.get();
        // [0][3]: with the cloud marker the VS reads the tint here (r + g * 256 + b * 65536) instead of clip/fade
        g[0][3] = static_cast<float>(to8(tint_.r) | (to8(tint_.g) << 8) | (to8(tint_.b) << 16));
        g[1][3] = fogStart;
        g[2][3] = fogEnd;
        // cloud marker 3: unlit for the light helper (> 1.5) and, distinct from the terrain unlit-test flag 2, makes
        // the VS decode [0][3] as the tint (> 2.5). Marker 4 (> 3.5) additionally drops back faces (Run 14): only when
        // the VS positions are camera-relative (the test puts the camera at the origin) and the camera is outside the
        // slab; inside it every face stays, as vanilla shows the clouds around a camera inside the layer.
        g[3][3]                        = cull ? 4.0f : 3.0f;
        cam.worldMatrixStack->getTop() = m;

        std::size_t tileCalls = 0;
        for (int f = 0; f < cloud::kFaceGroups; ++f) {
            Group const& grp = t.g[static_cast<std::size_t>(f)];
            if (!grp.quads || (skip & (1u << f))) continue;
            auto it = secs.find(grp.pos);
            if (it == secs.end()) continue;
            for (auto const* layer : {&it->second.opaque, &it->second.transparent})
                for (auto const& part : *layer) {
                    part.mesh->renderMesh(mctx, material, none, 0, part.verts, OffscreenCaptureDescription{}, nullptr);
                    ++tileCalls;
                }
            stats_.quadsDrawn += grp.quads;
        }
        if (tileCalls) ++stats_.tilesDrawn;
        calls += tileCalls;
    }
    stats_.drawCalls = calls;
    return calls;
}

} // namespace dh::render

namespace dh::render {

namespace {
bool gBackFaceCull = true; // Numpad 6; render thread only
} // namespace

bool CloudRenderer::backFaceCullEnabled() { return gBackFaceCull; }

void CloudRenderer::toggleBackFaceCull() {
    gBackFaceCull = !gBackFaceCull;
    DH_LOG_I("clouds: back-face cull -> %s (Numpad 6)", gBackFaceCull ? "on" : "off");
}

bool CloudRenderer::cullsBackFaces(double cameraY, bool cameraRelative) const {
    if (!gBackFaceCull || !cameraRelative) return false;
    const double bottom = static_cast<double>(slabBottom());
    return cameraY <= bottom || cameraY >= bottom + static_cast<double>(kThickness);
}

std::size_t CloudRenderer::liveBytes() const {
    std::size_t n = 0;
    for (auto const& [k, t] : tiles_)
        for (auto const& g : t.g) {
            if (!g.quads) continue;
            auto it = sink_.sections().find(g.pos);
            if (it != sink_.sections().end()) n += it->second.bytes;
        }
    return n;
}

std::size_t CloudRenderer::liveSectionCount() const {
    std::size_t n = 0;
    for (auto const& [k, t] : tiles_)
        for (auto const& g : t.g)
            if (g.quads && sink_.sections().count(g.pos)) ++n;
    return n;
}

} // namespace dh::render
