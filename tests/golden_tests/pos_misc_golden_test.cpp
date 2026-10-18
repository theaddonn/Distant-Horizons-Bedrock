// Golden vectors for DhChunkPos / DhBlockPos / DhBlockPos2D / FullDataMinMaxPosUtil / VarintUtil
// (tests/golden/pos_misc.gv.json).
//
// SKIPPED_KEYS:
//   block_N.encode_hex - netty encoding of DhBlockPos has no C++ equivalent.
#include <doctest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "dh/pos/BlockPos.h"
#include "dh/pos/ChunkPos.h"
#include "dh/pos/MinMaxPos.h"
#include "dh/util/Varint.h"
#include "golden_check.h"

using namespace dh;
using namespace dh::test;

namespace {

std::string toHex(const std::vector<std::byte>& v) {
    static const char* d = "0123456789abcdef";
    std::string        s;
    for (std::byte b : v) {
        s.push_back(d[(static_cast<unsigned>(b) >> 4) & 15]);
        s.push_back(d[static_cast<unsigned>(b) & 15]);
    }
    return s;
}

void checkChunk(gv::Ctx& c) {
    const ChunkPos   p(c.inI("x"), c.inI("z"));
    const BlockPos   blk(c.inI("blockX"), c.inI("blockY"), c.inI("blockZ"));
    const BlockPos2D blk2(blk.x, blk.z);
    const ChunkPos   other(c.inI("otherX"), c.inI("otherZ"));

    c.eq("hashCode", p.hashCode());
    const BlockPos cb = p.centerBlockPos();
    c.eqSub("centerBlockPos", "x", cb.x);
    c.eqSub("centerBlockPos", "y", cb.y);
    c.eqSub("centerBlockPos", "z", cb.z);
    const BlockPos mb = p.minCornerBlockPos();
    c.eqSub("minCornerBlockPos", "x", mb.x);
    c.eqSub("minCornerBlockPos", "y", mb.y);
    c.eqSub("minCornerBlockPos", "z", mb.z);
    c.eq("getMinBlockX", p.getMinBlockX());
    c.eq("getMinBlockZ", p.getMinBlockZ());
    c.eq("getMaxBlockX", p.getMaxBlockX());
    c.eq("getMaxBlockZ", p.getMaxBlockZ());
    const BlockPos2D m2 = p.getMinBlockPos();
    c.eqSub("getMinBlockPos", "x", m2.x);
    c.eqSub("getMinBlockPos", "z", m2.z);
    c.eq("contains_block", p.contains(blk));
    c.eq("contains_block2d", p.contains(blk2));
    c.eq("distance", p.distance(other));
    c.eq("squaredDistance", p.squaredDistance(other));
    const ChunkPos fb = ChunkPos(blk);
    c.eqSub("ctor_blockPos", "x", fb.x);
    c.eqSub("ctor_blockPos", "z", fb.z);
    const ChunkPos f2 = ChunkPos(blk2);
    c.eqSub("ctor_blockPos2d", "x", f2.x);
    c.eqSub("ctor_blockPos2d", "z", f2.z);
    const ChunkPos fv = ChunkPos::fromWorldPos(c.inD("vecX"), c.inD("vecZ"));
    c.eqSub("ctor_vec3d", "x", fv.x);
    c.eqSub("ctor_vec3d", "z", fv.z);
}

void checkBlock(gv::Ctx& c) {
    const BlockPos p(c.inI("x"), c.inI("y"), c.inI("z"));
    const BlockPos other(c.inI("otherX"), c.inI("otherY"), c.inI("otherZ"));

    c.eq("hashCode", p.hashCode());
    struct D {
        const char* key;
        Direction   d;
    };
    const D dirs[] = {
        {"createOffset_DOWN",  Direction::Down },
        {"createOffset_UP",    Direction::Up   },
        {"createOffset_NORTH", Direction::North},
        {"createOffset_SOUTH", Direction::South},
        {"createOffset_WEST",  Direction::West },
        {"createOffset_EAST",  Direction::East },
    };
    for (const D& d : dirs) {
        const BlockPos o = p.createOffset(d.d);
        c.eqSub(d.key, "x", o.x);
        c.eqSub(d.key, "y", o.y);
        c.eqSub(d.key, "z", o.z);
    }
    const BlockPos xyz = p.createOffset(c.inI("dx"), c.inI("dy"), c.inI("dz"));
    c.eqSub("createOffset_xyz", "x", xyz.x);
    c.eqSub("createOffset_xyz", "y", xyz.y);
    c.eqSub("createOffset_xyz", "z", xyz.z);
    const BlockPos rel = p.createChunkRelativePos();
    c.eqSub("createChunkRelativePos", "x", rel.x);
    c.eqSub("createChunkRelativePos", "y", rel.y);
    c.eqSub("createChunkRelativePos", "z", rel.z);
    c.eq("getManhattanDistance_3d", p.getManhattanDistance(other));
    c.eq("getManhattanDistance_2d", p.getManhattanDistance(other.x, other.z));
}

void checkBlock2d(gv::Ctx& c) {
    const BlockPos2D p(c.inI("x"), c.inI("z"));
    const BlockPos2D o(c.inI("otherX"), c.inI("otherZ"));

    c.eq("hashCode", p.hashCode());
    c.eq("dist", p.dist(o));
    c.eq("distSquared", p.distSquared(o));
    c.eq("chebyshevDist", p.chebyshevDist(o));
    c.eq("manhattanDist", p.manhattanDist(o));
    const BlockPos2D a = p.add(o);
    c.eqSub("add", "x", a.x);
    c.eqSub("add", "z", a.z);
    const BlockPos2D s = p.subtract(o);
    c.eqSub("subtract", "x", s.x);
    c.eqSub("subtract", "z", s.z);
}

void checkMinMaxDir(gv::Ctx& c) {
    const std::string dn = c.gc().in.at("direction").get<std::string>();
    Direction         d  = Direction::Down;
    if (dn == "UP") d = Direction::Up;
    else if (dn == "NORTH") d = Direction::North;
    else if (dn == "SOUTH") d = Direction::South;
    else if (dn == "WEST") d = Direction::West;
    else if (dn == "EAST") d = Direction::East;

    const char* k = "getEncodedMinMaxPos";
    const auto  r = minmax::getEncodedMinMaxPos(d);
    c.eqRejected(k, !r.has_value());
    if (!c.throws(k) && r) {
        c.eqSub(k, "encoded", *r);
        c.eqSub(k, "minX", minmax::getAdjMinX(*r));
        c.eqSub(k, "maxX", minmax::getAdjMaxX(*r));
        c.eqSub(k, "minZ", minmax::getAdjMinZ(*r));
        c.eqSub(k, "maxZ", minmax::getAdjMaxZ(*r));
    }
}

void checkMinMax(gv::Ctx& c) {
    c.eq(
        "encodeAdjMinMaxPos",
        minmax::encodeAdjMinMaxPos(
            static_cast<std::int16_t>(c.inI("minX")),
            static_cast<std::int16_t>(c.inI("maxX")),
            static_cast<std::int16_t>(c.inI("minZ")),
            static_cast<std::int16_t>(c.inI("maxZ"))
        )
    );
    // Decode Java's encoded value / raw so an encode mismatch does not cascade.
    const auto enc = static_cast<std::int64_t>(c.outHex("encodeAdjMinMaxPos"));
    c.eq("getAdjMinX", minmax::getAdjMinX(enc));
    c.eq("getAdjMaxX", minmax::getAdjMaxX(enc));
    c.eq("getAdjMinZ", minmax::getAdjMinZ(enc));
    c.eq("getAdjMaxZ", minmax::getAdjMaxZ(enc));
    const auto raw = static_cast<std::int64_t>(c.inHex("raw"));
    c.eq("raw_getAdjMinX", minmax::getAdjMinX(raw));
    c.eq("raw_getAdjMaxX", minmax::getAdjMaxX(raw));
    c.eq("raw_getAdjMinZ", minmax::getAdjMinZ(raw));
    c.eq("raw_getAdjMaxZ", minmax::getAdjMaxZ(raw));
}

std::optional<std::string> varintHex(std::uint32_t v) {
    std::vector<std::byte> bytes;
    if (!varint::writeVarint(bytes, v)) return std::nullopt;
    return toHex(bytes);
}

void checkVarint(gv::Ctx& c) {
    const auto v  = static_cast<std::uint32_t>(c.inI("value"));
    const auto zz = varint::zigzag(static_cast<std::int32_t>(v));
    c.eqOpt("writeVarint_hex", varintHex(v));
    c.eq("zigzagEncode", static_cast<std::int32_t>(zz));
    c.eq("zigzagDecode_of_value", varint::unzigzag(v));
    c.eq("zigzagRoundTrip", varint::unzigzag(zz));
    c.eqOpt("writeVarint_zigzag_hex", varintHex(zz));
}

} // namespace

TEST_CASE("golden pos_misc: DhChunkPos, DhBlockPos, DhBlockPos2D, MinMaxPos, Varint") {
    DH_GOLDEN_SUITE_OR_SKIP(s, "pos_misc");
    gv::Stats st;
    for (const GoldenCase& gc : s.cases) {
        gv::Ctx            c(gc, st);
        const std::string& n = gc.name;
        if (n.rfind("chunk_", 0) == 0) checkChunk(c);
        else if (n.rfind("block2d_", 0) == 0) checkBlock2d(c);
        else if (n.rfind("block_", 0) == 0) checkBlock(c);
        else if (n.rfind("minmax_dir_", 0) == 0) checkMinMaxDir(c);
        else if (n.rfind("minmax_", 0) == 0) checkMinMax(c);
        else if (n.rfind("varint_", 0) == 0) checkVarint(c);
        else FAIL("unhandled case kind " << n);
    }
    gv::report("pos_misc", st);
}
