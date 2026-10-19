#include "client/render/MeshUploader.h"

#include "bedrock_common/FileLogger.h"

#include "dh/pos/SectionPos.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/MeshData.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <string>

namespace dh::render {

namespace {

/// Largest vertex count of one mce::Mesh part. Stays below 65536 so a 16-bit shared quad index buffer (if the engine
/// uses one for QuadList) can never overflow.
constexpr std::uint32_t kMaxPartVertices = 65000;

/// mce::MeshData::mFieldEnabled indices (static RE, 1.26.51, 2026-10-19): the Tessellator setters store
/// mNextUV[0] / [1] / [2] (+0x18C / +0x198 / +0x1A4) and enable byte +0x15C / +0x15D / +0x15E of the Tessellator,
/// i.e. mFieldEnabled[4] / [5] / [6] (mFieldEnabled at Tessellator +0x158); colour (+0x1B0) enables [1].
/// Tessellator::tex2 (RVA 0x59CA3D0) clamps to [0, 1] and writes uv set 2 -> field 6.
constexpr std::size_t kFieldUV0 = 4;
constexpr std::size_t kFieldUV2 = 6;

template <class V>
void replicate(V& v, std::size_t n) {
    if (v.size() == 1) v.assign(n, v.front());
}

} // namespace

/// Packed colour channel order, discovered from four real Tessellator colours (port of Animated-Mojang's
/// ColorPacking: no assumption about ARGB/ABGR), plus one-vertex MeshData templates per primitive that carry the
/// engine's native vertex field set (mFieldEnabled) and default single-element streams.
struct MeshUploader::Format {
    unsigned                       red = 0, green = 8, blue = 16;
    std::uint32_t                  alphaMask = 0xff000000u;
    std::array<bool, 15>           fields{};
    std::unique_ptr<mce::MeshData> tmpl[2]; // indexed by dh::Primitive
    std::string                    fieldString;

    static bool fromSamples(std::vector<std::uint32_t> const& c, Format& f) {
        if (c.size() != 4) return false;
        const auto black = c[3];
        const auto r = c[0] ^ black, g = c[1] ^ black, b = c[2] ^ black;
        if (!r || !g || !b || (r & g) || (r & b) || (g & b) || ((r | g | b) & black)
            || (r >> std::countr_zero(r)) != 255 || (g >> std::countr_zero(g)) != 255
            || (b >> std::countr_zero(b)) != 255 || (r | g | b | black) != 0xffffffffu)
            return false;
        f.red       = static_cast<unsigned>(std::countr_zero(r));
        f.green     = static_cast<unsigned>(std::countr_zero(g));
        f.blue      = static_cast<unsigned>(std::countr_zero(b));
        f.alphaMask = black;
        return true;
    }

    std::uint32_t pack(std::uint32_t dhRgba) const {
        const std::uint32_t r = dhRgba & 0xFF, g = (dhRgba >> 8) & 0xFF, b = (dhRgba >> 16) & 0xFF, a = dhRgba >> 24;
        return (a << std::countr_zero(alphaMask)) | (r << red) | (g << green) | (b << blue);
    }
};

MeshUploader::Section::Section()                                            = default;
MeshUploader::Section::Section(Section&&) noexcept                          = default;
MeshUploader::Section& MeshUploader::Section::operator=(Section&&) noexcept = default;
MeshUploader::Section::~Section()                                           = default;

MeshUploader::MeshUploader() = default;

MeshUploader::~MeshUploader() {
    // May run off the render thread or after the buffer service died: leak GPU meshes instead of destroying them.
    destroyAll(true);
    std::vector<UploadCallback> cbs;
    {
        std::lock_guard lock(mutex_);
        for (auto& [k, op] : latest_)
            if (op.onDone) cbs.push_back(std::move(op.onDone));
        latest_.clear();
        newestId_.clear();
        order_.clear();
    }
    for (auto& cb : cbs) cb(false);
}

void MeshUploader::setSettings(MeshSettings const& s) {
    {
        std::lock_guard lock(mutex_);
        settings_ = s;
        ++gen_;
    }
    rebuildAll_ = true;
}

MeshSettings MeshUploader::settings() const {
    std::lock_guard lock(mutex_);
    return settings_;
}

void MeshUploader::convertInto(MeshBuffers const& b, std::uint64_t pos, MeshSettings const& s, Converted& out) const {
    const int detail = sec::detail(pos);
    for (auto const& buf : b.opaque) convertDhBuffer(buf, detail, s.yOffsetBlocks, out.opaque, s.prim, s.params);
    for (auto const& buf : b.transparent)
        convertDhBuffer(buf, detail, s.yOffsetBlocks, out.transparent, s.prim, s.params);
    if (!out.opaque.empty()) {
        float       vmin = 1e9f, vmax = -1e9f;
        std::size_t zeros = 0;
        for (auto const& v : out.opaque) {
            vmin = std::min(vmin, v.v);
            vmax = std::max(vmax, v.v);
            if (v.v == 0.0f) ++zeros;
        }
        lsMin_.store(vmin);
        lsMax_.store(vmax);
        lsZero_.store(static_cast<float>(zeros) / static_cast<float>(out.opaque.size()));
        lsValid_.store(true);
    }
}

MeshUploader::LightStats MeshUploader::lightStats() const {
    LightStats l;
    l.valid    = lsValid_.load();
    l.vMin     = lsMin_.load();
    l.vMax     = lsMax_.load();
    l.zeroFrac = lsZero_.load();
    return l;
}

void MeshUploader::upload(std::uint64_t sectionPos, MeshBuffers&& buffers, UploadCallback onDone) {
    (void)uploadWithId(sectionPos, std::move(buffers), std::move(onDone));
}

MeshUploader::UploadId
MeshUploader::uploadWithId(std::uint64_t sectionPos, MeshBuffers&& buffers, UploadCallback onDone) {
    const UploadId id = ++lastId_;
    MeshSettings   s;
    std::uint32_t  gen;
    {
        std::lock_guard lock(mutex_);
        s   = settings_;
        gen = gen_;
    }
    Op op;
    op.pos    = sectionPos;
    op.source = std::make_shared<const MeshBuffers>(std::move(buffers));
    op.onDone = std::move(onDone);
    op.gen    = gen;
    convertInto(*op.source, sectionPos, s, op.conv); // calling thread
    op.converted = true;

    UploadCallback dropped;
    {
        std::lock_guard lock(mutex_);
        auto [it, inserted] = latest_.try_emplace(sectionPos);
        if (!inserted) dropped = std::move(it->second.onDone);
        else order_.push_back(sectionPos);
        it->second            = std::move(op);
        newestId_[sectionPos] = id;
        pending_              = latest_.size();
    }
    if (dropped) dropped(false);
    return id;
}

void MeshUploader::remove(std::uint64_t sectionPos) {
    UploadCallback dropped;
    {
        std::lock_guard lock(mutex_);
        dropped = removeLocked(sectionPos);
    }
    if (dropped) dropped(false);
}

void MeshUploader::removeIf(std::uint64_t sectionPos, UploadId id) {
    UploadCallback dropped;
    {
        std::lock_guard lock(mutex_);
        auto            n = newestId_.find(sectionPos);
        if (id != 0 && n != newestId_.end() && n->second == id) {
            dropped = removeLocked(sectionPos);
        } else {
            ++staleRemoves_; // a newer upload exists (or the section is already gone): never free the newer mesh
            return;
        }
    }
    if (dropped) dropped(false);
}

/// Queues an unconditional remove (caller holds mutex_). Returns the superseded op's callback; fire it unlocked.
MeshUploader::UploadCallback MeshUploader::removeLocked(std::uint64_t sectionPos) {
    UploadCallback dropped;
    newestId_.erase(sectionPos);
    auto [it, inserted] = latest_.try_emplace(sectionPos);
    if (!inserted) dropped = std::move(it->second.onDone);
    else order_.push_back(sectionPos);
    Op op;
    op.isRemove = true;
    op.pos      = sectionPos;
    it->second  = std::move(op);
    pending_    = latest_.size();
    return dropped;
}

void MeshUploader::requestClear() { clearRequested_ = true; }

MeshUploader::Stats MeshUploader::stats() const {
    Stats s;
    s.pending      = pending_;
    s.liveSections = liveSections_;
    s.liveBytes    = liveBytes_;
    s.liveVertices = liveVertices_;
    s.uploadsTotal = uploadsTotal_;
    s.staleRemoves = staleRemoves_;
    return s;
}

void MeshUploader::destroyAll(bool leak) {
    for (auto& [k, sec] : sections_) {
        for (auto* layer : {&sec.opaque, &sec.transparent})
            for (auto& p : *layer)
                if (leak) (void)p.mesh.release();
                else p.mesh.reset();
    }
    sections_.clear();
    liveSections_ = liveBytes_ = liveVertices_ = 0;
}

bool MeshUploader::ensureFormat(ScreenContext& ctx) {
    if (formatReady_) return true;
    if (formatFailed_) return false;
    auto f = std::make_unique<Format>();

    // Colour packing from four real colours.
    {
        Tessellator seed{ctx.tessellator.mBufferResourceService};
        seed.resetTransform(true);
        seed.begin({}, mce::PrimitiveMode::QuadList, 4, false);
        constexpr float samples[4][3] = {
            {1, 0, 0},
            {0, 1, 0},
            {0, 0, 1},
            {0, 0, 0}
        };
        constexpr float corners[4][2] = {
            {0, 0},
            {1, 0},
            {1, 1},
            {0, 1}
        };
        for (int i = 0; i < 4; ++i) {
            seed.color(samples[i][0], samples[i][1], samples[i][2], 1.0f);
            seed.tex2(Vec2{corners[i][0], corners[i][1]});
            seed.vertex(corners[i][0], corners[i][1], 0.0f);
        }
        std::vector<std::uint32_t> colors = seed.mMeshData->mColors.get();
        if (!Format::fromSamples(colors, *f)) {
            formatFailed_ = true;
            DH_LOG_E(
                "render: native colour packing not recognised (%zu samples); Stage-A meshes disabled",
                colors.size()
            );
            return false;
        }
        f->fields = seed.mMeshData->mFieldEnabled.get();
    }
    for (std::string::size_type i = 0; i < f->fields.size(); ++i) f->fieldString += f->fields[i] ? '1' : '0';

    // One-vertex templates (native field set + single-element default streams), one per primitive.
    for (int p = 0; p < 2; ++p) {
        auto        prim = p == 0 ? mce::PrimitiveMode::QuadList : mce::PrimitiveMode::TriangleList;
        Tessellator seed{ctx.tessellator.mBufferResourceService};
        seed.resetTransform(true);
        seed.begin({}, prim, 4, false);
        seed.color(1.0f, 1.0f, 1.0f, 1.0f);
        seed.tex2(Vec2{0.5f, 0.5f});
        seed.mNextNormal = glm::vec4{0.f, 1.f, 0.f, 0.f};
        seed.vertex(0.f, 0.f, 0.f);
        f->tmpl[p] = std::make_unique<mce::MeshData>(seed.mMeshData.get());
    }
    DH_LOG_I(
        "render: native vertex format ok: mFieldEnabled=%s (DH meshes also enable uv0=[4], uv2=[6]) colour shifts r=%u "
        "g=%u b=%u alphaMask=0x%08x",
        f->fieldString.c_str(),
        f->red,
        f->green,
        f->blue,
        f->alphaMask
    );
    format_      = std::move(f);
    formatReady_ = true;
    return true;
}

namespace {

/// Builds the parts of one layer (opaque or transparent) from converted vertices. `quadVerts` = vertices per quad.
template <class Fmt>
void buildLayer(
    std::vector<MeshUploader::Part>&             parts,
    std::vector<StageAVertex> const&             verts,
    std::size_t                                  quadVerts,
    Primitive                                    prim,
    Fmt const&                                   fmt,
    std::shared_ptr<mce::BufferResourceService>& svc,
    glm::vec3&                                   bmin,
    glm::vec3&                                   bmax,
    bool&                                        any
) {
    const std::size_t perPart = (kMaxPartVertices / quadVerts) * quadVerts;
    for (std::size_t begin = 0; begin < verts.size(); begin += perPart) {
        const std::size_t n = std::min(perPart, verts.size() - begin);
        mce::MeshData     md(*fmt.tmpl[prim == Primitive::QuadList ? 0 : 1]);

        std::vector<glm::vec3>     pos(n);
        std::vector<std::uint32_t> col(n);
        std::vector<glm::vec2>     uv(n);
        glm::vec3                  lo{1e30f}, hi{-1e30f};
        for (std::size_t i = 0; i < n; ++i) {
            StageAVertex const& v = verts[begin + i];
            pos[i]                = glm::vec3{v.x, v.y, v.z};
            col[i]                = fmt.pack(v.rgba);
            uv[i]                 = glm::vec2{dh::encodeFaceU(v.u), v.v};
            lo                    = glm::min(lo, pos[i]);
            hi                    = glm::max(hi, pos[i]);
        }
        // uv0 carries (encoded face index, packed light). The template comes from Tessellator::tex2, which writes the
        // THIRD uv set (mNextUV[2], field kFieldUV2) and leaves uv0 disabled and empty, so uv0 has to be switched on
        // explicitly: without it the mesh has no TEXCOORD0 stream and the DH_Terrain VS reads (0, 0) = light 0 =
        // pitch-black LODs (Run 13). uv2 gets the same values so the data is present whichever uv set the engine
        // binds.
        md.mTextureUVs[2]                 = uv;
        md.mTextureUVs[0]                 = std::move(uv);
        md.mFieldEnabled.get()[kFieldUV0] = true;
        md.mFieldEnabled.get()[kFieldUV2] = true;
        md.mPositions                     = std::move(pos);
        md.mColors                        = std::move(col);
        replicate(md.mNormals.get(), n);
        replicate(md.mTangents.get(), n);
        replicate(md.mBoneId0s.get(), n);
        replicate(md.mTextureUVs[1].get(), n);
        replicate(md.mPBRTextureIndices.get(), n);
        replicate(md.mMERS.get(), n);
        replicate(md.mGeoType.get(), n);
        md.mAABB   = std::pair{lo, hi};
        md.mUVAABB = std::pair{glm::vec2{0}, glm::vec2{1}};

        MeshUploader::Part part;
        part.mesh  = std::make_unique<mce::Mesh>(svc, std::move(md), false, "dh_lod");
        part.verts = static_cast<std::uint32_t>(n);
        parts.push_back(std::move(part));
        bmin = any ? glm::min(bmin, lo) : lo;
        bmax = any ? glm::max(bmax, hi) : hi;
        any  = true;
    }
}

} // namespace

bool MeshUploader::applyUpload(
    Op&                                          op,
    MeshSettings const&                          s,
    std::uint32_t                                curGen,
    std::shared_ptr<mce::BufferResourceService>& svc
) {
    if (!op.converted || op.gen != curGen) {
        op.conv = {};
        convertInto(*op.source, op.pos, s, op.conv); // settings changed since queued: redo on the render thread
    }
    const std::size_t qv = verticesPerQuad(s.prim, s.params.doubleSided);

    Section sec;
    sec.pos    = op.pos;
    sec.minX   = sec::minCornerBlockX(op.pos);
    sec.minZ   = sec::minCornerBlockZ(op.pos);
    sec.source = op.source;
    bool any   = false;
    buildLayer(sec.opaque, op.conv.opaque, qv, s.prim, *format_, svc, sec.bmin, sec.bmax, any);
    buildLayer(sec.transparent, op.conv.transparent, qv, s.prim, *format_, svc, sec.bmin, sec.bmax, any);
    const std::size_t verts = op.conv.opaque.size() + op.conv.transparent.size();
    sec.bytes               = verts * sizeof(StageAVertex);

    auto it = sections_.find(op.pos);
    if (it != sections_.end()) {
        liveBytes_ -= it->second.bytes;
        for (auto* layer : {&it->second.opaque, &it->second.transparent})
            for (auto& p : *layer) liveVertices_ -= p.verts;
        sections_.erase(it); // old meshes destroyed here, on the render thread
    }
    for (auto* layer : {&sec.opaque, &sec.transparent})
        for (auto& p : *layer) liveVertices_ += p.verts;
    liveBytes_ += sec.bytes;
    if (any) sections_.emplace(op.pos, std::move(sec));
    liveSections_ = sections_.size();
    ++uploadsTotal_;
    return true;
}

std::size_t MeshUploader::pump(ScreenContext& ctx, std::size_t maxUploads) {
    auto svc = ctx.tessellator.mBufferResourceService.lock();
    if (!svc) return 0;
    if (service_ != svc.get()) {
        if (service_) {
            DH_LOG_W("render: buffer service changed; leaking %zu old sections' meshes", sections_.size());
            destroyAll(true);
            std::lock_guard lock(mutex_);
            for (auto& [k, op] : latest_) op.onDone = nullptr; // sections will be re-uploaded by the core
            latest_.clear();
            newestId_.clear();
            order_.clear();
            pending_ = 0;
        }
        service_      = svc.get();
        formatReady_  = false;
        formatFailed_ = false;
        format_.reset();
    }
    if (clearRequested_.exchange(false)) {
        destroyAll(false);
        std::vector<UploadCallback> cbs;
        {
            std::lock_guard lock(mutex_);
            for (auto& [k, op] : latest_)
                if (op.onDone) cbs.push_back(std::move(op.onDone));
            latest_.clear();
            newestId_.clear();
            order_.clear();
            pending_ = 0;
        }
        for (auto& cb : cbs) cb(false);
    }
    if (!ensureFormat(ctx)) return 0;

    MeshSettings  s;
    std::uint32_t curGen;
    {
        std::lock_guard lock(mutex_);
        s      = settings_;
        curGen = gen_;
    }
    if (rebuildAll_.exchange(false)) {
        // Re-convert every live section from the retained source (unless a newer op is already queued).
        std::lock_guard lock(mutex_);
        for (auto& [pos, sec] : sections_) {
            if (!sec.source || latest_.count(pos)) continue;
            Op op;
            op.pos       = pos;
            op.source    = sec.source;
            op.converted = false;
            latest_.emplace(pos, std::move(op));
            order_.push_back(pos);
        }
        pending_ = latest_.size();
    }

    std::size_t done = 0;
    while (true) {
        Op op;
        {
            std::lock_guard lock(mutex_);
            bool            found = false;
            while (!order_.empty()) {
                auto pos = order_.front();
                auto it  = latest_.find(pos);
                if (it == latest_.end()) {
                    order_.pop_front();
                    continue; // duplicate entry already served
                }
                if (!it->second.isRemove && done >= maxUploads) break;
                order_.pop_front();
                op = std::move(it->second);
                latest_.erase(it);
                found = true;
                break;
            }
            pending_ = latest_.size();
            if (!found) break;
        }
        if (op.isRemove) {
            auto it = sections_.find(op.pos);
            if (it != sections_.end()) {
                liveBytes_ -= it->second.bytes;
                for (auto* layer : {&it->second.opaque, &it->second.transparent})
                    for (auto& p : *layer) liveVertices_ -= p.verts;
                sections_.erase(it);
                liveSections_ = sections_.size();
            }
            continue;
        }
        bool ok = applyUpload(op, s, curGen, svc);
        ++done;
        if (op.onDone) op.onDone(ok);
    }
    return done;
}

} // namespace dh::render
