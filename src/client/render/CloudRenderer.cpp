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

void CloudRenderer::release() {
    if (haveCur_) sink_.removeIf(cur_.pos, cur_.id);
    if (havePending_) sink_.removeIf(pending_.pos, pending_.id);
    haveCur_     = false;
    havePending_ = false;
    stats_.drawn = false;
}

bool CloudRenderer::update(Inputs const& in) {
    const auto  now  = Clock::now();
    auto const& secs = sink_.sections();

    if (jobRunning_ && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        jobRunning_ = false; // before get(): a throwing job must not leave an invalid future marked running
        try {
            pending_ = job_.get();
        } catch (...) {
            pending_       = Built{};
            pending_.empty = true;
            retryAfter_    = now + std::chrono::seconds(5);
            DH_LOG_E("clouds: build job threw; retrying in 5 s");
        }
        havePending_  = !pending_.empty;
        pendingSince_ = now;
    }

    if (!in.enable || in.farBlocks <= 0.0f) {
        release();
        return false;
    }

    if (havePending_) {
        const int st = pending_.state->load();
        if (st == 1 && secs.count(pending_.pos)) {
            if (haveCur_ && cur_.pos != pending_.pos) sink_.removeIf(cur_.pos, cur_.id);
            cur_         = pending_;
            haveCur_     = true;
            havePending_ = false;
            stats_.quads = cur_.quads;
            ++stats_.builds;
            stats_.lastBuildMs = cur_.ms;
            if (!loggedFirst_) {
                loggedFirst_ = true;
                DH_LOG_I(
                    "clouds: first section live: %zu quads, built in %.1f ms, half extent %d, anchor (%ld,%ld)",
                    cur_.quads,
                    cur_.ms,
                    cur_.key.half,
                    cur_.key.anchorX,
                    cur_.key.anchorZ
                );
            }
        } else if (st == 2 || now - pendingSince_ > std::chrono::seconds(5)) {
            havePending_ = false;
            retryAfter_  = now + std::chrono::seconds(1);
        }
    }
    if (haveCur_ && !secs.count(cur_.pos)) haveCur_ = false; // cleared (world exit) or never produced geometry

    if (!patternTried_) loadPattern();
    if (!pattern_ || pattern_->empty()) return false;

    const double radius = std::min<double>(in.farBlocks, 15000.0);
    const double offX   = offsetX();
    const double camX = in.camera.x - offX, camZ = in.camera.z; // camera in cloud space
    Key          want;
    want.half         = static_cast<int>(std::ceil((radius + kMarginBlocks) / 256.0)) * 256;
    want.yBottom      = static_cast<int>(std::lround(in.cloudY - in.minY));
    tint_             = in.tint; // per draw, continuous (no rebuild)
    const double snap = static_cast<double>(kCellBlocks) * 16.0;
    want.anchorX      = static_cast<long>(std::llround(camX / snap)) * static_cast<long>(snap);
    want.anchorZ      = static_cast<long>(std::llround(camZ / snap)) * static_cast<long>(snap);

    bool need = !haveCur_;
    if (haveCur_) {
        Key const&   k    = cur_.key;
        const double far_ = std::max(
            std::fabs(camX - static_cast<double>(k.anchorX)),
            std::fabs(camZ - static_cast<double>(k.anchorZ))
        );
        if (k.half != want.half || k.yBottom != want.yBottom || far_ > kMarginBlocks - 48) need = true;
    }
    if (need && !jobRunning_ && !havePending_ && now >= retryAfter_) {
        cloud::BuildParams bp;
        bp.cell           = kCellBlocks;
        bp.centerX        = static_cast<double>(want.anchorX);
        bp.centerZ        = static_cast<double>(want.anchorZ);
        bp.halfExtent     = static_cast<double>(want.half);
        bp.anchorX        = static_cast<int>(want.anchorX);
        bp.anchorZ        = static_cast<int>(want.anchorZ);
        bp.yBottom        = static_cast<float>(want.yBottom);
        bp.thickness      = kThickness;
        bp.fancy          = true;
        // white mesh (face shading only): the sky tint is applied per draw by the VS (draw(), u_model[0][3][0])
        bp.r              = 255;
        bp.g              = 255;
        bp.b              = 255;
        bp.a              = static_cast<std::uint8_t>(std::lround(kAlpha * 255.0f));
        // the mesh is generated in cloud space around the anchor; int16 local coordinates need half + cell < 32768
        auto          pat  = pattern_;
        MeshUploader* sink = &sink_;
        job_               = std::async(std::launch::async, [pat, bp, want, sink]() {
            Built b;
            b.key = want;
            try {
                const auto t = Clock::now();
                auto       r = cloud::build(*pat, bp);
                b.quads      = r.quads;
                b.ms         = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
                b.state      = std::make_shared<std::atomic<int>>(0);
                if (r.quads == 0) {
                    b.empty = true;
                    return b;
                }
                MeshBuffers mb;
                for (auto& v : r.buffers) mb.transparent.push_back(std::move(v));
                b.pos =
                    sec::encode(0, static_cast<std::int32_t>(want.anchorX), static_cast<std::int32_t>(want.anchorZ));
                auto st = b.state;
                b.id    = sink->uploadWithId(b.pos, std::move(mb), [st](bool ok) { st->store(ok ? 1 : 2); });
                return b;
            } catch (...) {
                Built e;
                e.key   = want;
                e.empty = true;
                return e;
            }
        });
        jobRunning_        = true;
    }
    stats_.drawn = haveCur_;
    return haveCur_;
}

std::size_t CloudRenderer::draw(
    mce::MeshContext&       mctx,
    mce::MaterialPtr const& material,
    glm::dvec3 const&       camera,
    bool                    cameraRelative,
    float                   fogStart,
    float                   fogEnd
) {
    if (!haveCur_) return 0;
    auto const& secs = sink_.sections();
    auto        it   = secs.find(cur_.pos);
    if (it == secs.end()) return 0;
    static Texture const none{std::monostate{}};

    auto&        cam  = mctx.camera;
    auto         wref = cam.worldMatrixStack->push(false);
    Matrix       m    = Matrix::IDENTITY();
    const double cx = cameraRelative ? camera.x : 0.0, cy = cameraRelative ? camera.y : 0.0,
                 cz = cameraRelative ? camera.z : 0.0; // absolute mode: the view matrix carries the camera
    m.translate(
        static_cast<float>(static_cast<double>(cur_.key.anchorX) + offsetX() - cx),
        static_cast<float>(-cy),
        static_cast<float>(static_cast<double>(cur_.key.anchorZ) - cz)
    );
    // DH_Terrain per-draw parameters in the projective row: no near clip / fade, DH far fog
    auto& g                        = m._m.get();
    // [0][3]: with the cloud marker the VS reads the tint here (r + g * 256 + b * 65536) instead of clip/fade
    g[0][3] = static_cast<float>(to8(tint_.r) | (to8(tint_.g) << 8) | (to8(tint_.b) << 16));
    g[1][3]                        = fogStart;
    g[2][3]                        = fogEnd;
    // cloud marker 3: unlit for the light helper (> 1.5) and, distinct from the terrain unlit-test flag 2, makes the
    // VS decode [0][3] as the tint (> 2.5)
    g[3][3]                        = 3.0f;
    cam.worldMatrixStack->getTop() = m;

    std::size_t calls = 0;
    for (auto const* layer : {&it->second.opaque, &it->second.transparent})
        for (auto const& part : *layer) {
            part.mesh->renderMesh(mctx, material, none, 0, part.verts, OffscreenCaptureDescription{}, nullptr);
            ++calls;
        }
    return calls;
}

} // namespace dh::render

namespace dh::render {

std::size_t CloudRenderer::liveBytes() const {
    if (!haveCur_) return 0;
    auto it = sink_.sections().find(cur_.pos);
    return it == sink_.sections().end() ? 0 : it->second.bytes;
}

} // namespace dh::render
