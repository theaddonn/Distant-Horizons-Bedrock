package dumper;

import com.seibel.distanthorizons.api.enums.config.EDhApiDataCompressionMode;
import com.seibel.distanthorizons.core.enums.EDhDirection;
import com.seibel.distanthorizons.core.pos.DhChunkPos;
import com.seibel.distanthorizons.core.pos.blockPos.DhBlockPos;
import com.seibel.distanthorizons.core.pos.blockPos.DhBlockPos2D;
import com.seibel.distanthorizons.core.sql.dto.util.FullDataMinMaxPosUtil;
import com.seibel.distanthorizons.core.sql.dto.util.VarintUtil;
import com.seibel.distanthorizons.core.util.math.DhVec3d;
import com.seibel.distanthorizons.core.util.objects.dataStreams.DhDataOutputStream;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import it.unimi.dsi.fastutil.bytes.ByteArrayList;

import java.util.ArrayList;
import java.util.List;

/** DhChunkPos, DhBlockPos, DhBlockPos2D, FullDataMinMaxPosUtil, VarintUtil (2000 cases each). */
public class PosMiscSuite implements Suite
{
	public static final int N = 1000;

	@Override public String name() { return "pos_misc"; }

	@Override
	public List<Obj> cases()
	{
		List<Obj> out = new ArrayList<>();
		chunk(out);
		block(out);
		block2d(out);
		minmax(out);
		varint(out);
		return out;
	}

	private static void chunk(List<Obj> out)
	{
		Gen g = new Gen(0xC4A11L);
		for (int i = 0; i < N; i++)
		{
			final int x = g.r.nextInt(3) == 0 ? g.anyInt() : g.pos28();
			final int z = g.r.nextInt(3) == 0 ? g.anyInt() : g.pos28();
			final int ox = g.r.nextBoolean() ? x + g.r.nextInt(7) - 3 : g.anyInt();
			final int oz = g.r.nextBoolean() ? z + g.r.nextInt(7) - 3 : g.anyInt();
			final int bx = g.r.nextBoolean() ? (x << 4) + g.r.nextInt(40) - 12 : g.anyInt();
			final int bz = g.r.nextBoolean() ? (z << 4) + g.r.nextInt(40) - 12 : g.anyInt();
			final int by = g.r.nextInt(500) - 100;
			final double vx = g.r.nextBoolean() ? (g.r.nextDouble() - 0.5) * 2e9 : (g.r.nextDouble() - 0.5) * 4000.0;
			final double vz = g.r.nextBoolean() ? (g.r.nextDouble() - 0.5) * 2e9 : (g.r.nextDouble() - 0.5) * 4000.0;
			final DhChunkPos c = new DhChunkPos(x, z);
			final DhChunkPos o = new DhChunkPos(ox, oz);

			Obj in = new Obj().p("x", x).p("z", z).p("otherX", ox).p("otherZ", oz)
					.p("blockX", bx).p("blockY", by).p("blockZ", bz).p("vecX", vx).p("vecZ", vz);
			Obj res = new Obj();
			res.p("hashCode", c.hashCode());
			res.t("centerBlockPos", () -> blockPosObj(c.centerBlockPos()));
			res.t("minCornerBlockPos", () -> blockPosObj(c.minCornerBlockPos()));
			res.p("getMinBlockX", c.getMinBlockX()).p("getMinBlockZ", c.getMinBlockZ());
			res.p("getMaxBlockX", c.getMaxBlockX()).p("getMaxBlockZ", c.getMaxBlockZ());
			res.t("getMinBlockPos", () -> new Obj().p("x", c.getMinBlockPos().x).p("z", c.getMinBlockPos().z));
			res.t("contains_block", () -> c.contains(new DhBlockPos(bx, by, bz)));
			res.t("contains_block2d", () -> c.contains(new DhBlockPos2D(bx, bz)));
			res.p("distance", c.distance(o));
			res.p("squaredDistance", c.squaredDistance(o));
			res.t("ctor_blockPos", () -> chunkObj(new DhChunkPos(new DhBlockPos(bx, by, bz))));
			res.t("ctor_blockPos2d", () -> chunkObj(new DhChunkPos(new DhBlockPos2D(bx, bz))));
			res.t("ctor_vec3d", () -> chunkObj(new DhChunkPos(new DhVec3d(vx, 0.0, vz))));
			out.add(Obj.testCase("chunk_" + i, in, res));
		}
	}

	private static Obj blockPosObj(DhBlockPos p) { return new Obj().p("x", p.getX()).p("y", p.getY()).p("z", p.getZ()); }
	private static Obj chunkObj(DhChunkPos p) { return new Obj().p("x", p.getX()).p("z", p.getZ()); }

	private static void block(List<Obj> out)
	{
		Gen g = new Gen(0xB10C4L);
		for (int i = 0; i < N; i++)
		{
			final int x = g.anyInt(), y = g.anyInt(), z = g.anyInt();
			final int dx = g.r.nextInt(5) == 0 ? g.anyInt() : g.r.nextInt(65) - 32;
			final int dy = g.r.nextInt(5) == 0 ? g.anyInt() : g.r.nextInt(65) - 32;
			final int dz = g.r.nextInt(5) == 0 ? g.anyInt() : g.r.nextInt(65) - 32;
			final int ox = g.anyInt(), oy = g.anyInt(), oz = g.anyInt();
			final DhBlockPos p = new DhBlockPos(x, y, z);
			Obj in = new Obj().p("x", x).p("y", y).p("z", z).p("dx", dx).p("dy", dy).p("dz", dz)
					.p("otherX", ox).p("otherY", oy).p("otherZ", oz);
			Obj res = new Obj();
			res.p("hashCode", p.hashCode());
			for (EDhDirection d : EDhDirection.values())
			{
				res.t("createOffset_" + d.name(), () -> blockPosObj(p.createOffset(d)));
			}
			res.t("createOffset_xyz", () -> blockPosObj(p.createOffset(dx, dy, dz)));
			res.t("createChunkRelativePos", () -> blockPosObj(p.createChunkRelativePos()));
			res.t("getManhattanDistance_3d", () -> p.getManhattanDistance(new DhBlockPos(ox, oy, oz)));
			res.t("getManhattanDistance_2d", () -> p.getManhattanDistance(new DhBlockPos2D(ox, oz)));
			res.t("encode_hex", () ->
			{
				ByteBuf buf = Unpooled.buffer();
				p.encode(buf);
				byte[] b = new byte[buf.readableBytes()];
				buf.readBytes(b);
				return Json.hex(b);
			});
			out.add(Obj.testCase("block_" + i, in, res));
		}
	}

	private static void block2d(List<Obj> out)
	{
		Gen g = new Gen(0xB10C2DL);
		for (int i = 0; i < N; i++)
		{
			final int x = g.anyInt(), z = g.anyInt();
			final int ox = g.r.nextBoolean() ? x + g.r.nextInt(2001) - 1000 : g.anyInt();
			final int oz = g.r.nextBoolean() ? z + g.r.nextInt(2001) - 1000 : g.anyInt();
			final DhBlockPos2D p = new DhBlockPos2D(x, z);
			final DhBlockPos2D o = new DhBlockPos2D(ox, oz);
			Obj in = new Obj().p("x", x).p("z", z).p("otherX", ox).p("otherZ", oz);
			Obj res = new Obj();
			res.p("hashCode", p.hashCode());
			res.p("dist", p.dist(o));
			res.p("distSquared", p.distSquared(o));
			res.p("chebyshevDist", p.chebyshevDist(o));
			res.p("manhattanDist", p.manhattanDist(o));
			res.p("add", new Obj().p("x", p.add(o).x).p("z", p.add(o).z));
			res.p("subtract", new Obj().p("x", p.subtract(o).x).p("z", p.subtract(o).z));
			out.add(Obj.testCase("block2d_" + i, in, res));
		}
	}

	private static void minmax(List<Obj> out)
	{
		for (EDhDirection d : EDhDirection.values())
		{
			Obj in = new Obj().p("direction", d.name());
			Obj res = new Obj();
			res.t("getEncodedMinMaxPos", () ->
			{
				long v = FullDataMinMaxPosUtil.getEncodedMinMaxPos(d);
				return new Obj().p("encoded", v)
						.p("minX", FullDataMinMaxPosUtil.getAdjMinX(v)).p("maxX", FullDataMinMaxPosUtil.getAdjMaxX(v))
						.p("minZ", FullDataMinMaxPosUtil.getAdjMinZ(v)).p("maxZ", FullDataMinMaxPosUtil.getAdjMaxZ(v));
			});
			out.add(Obj.testCase("minmax_dir_" + d.name(), in, res));
		}
		Gen g = new Gen(0x3141L);
		for (int i = 0; i < N; i++)
		{
			final short a = pickShort(g), b = pickShort(g), c = pickShort(g), d = pickShort(g);
			Obj in = new Obj().p("minX", (int) a).p("maxX", (int) b).p("minZ", (int) c).p("maxZ", (int) d);
			Obj res = new Obj();
			final long v = FullDataMinMaxPosUtil.encodeAdjMinMaxPos(a, b, c, d);
			res.p("encodeAdjMinMaxPos", v);
			res.p("getAdjMinX", FullDataMinMaxPosUtil.getAdjMinX(v));
			res.p("getAdjMaxX", FullDataMinMaxPosUtil.getAdjMaxX(v));
			res.p("getAdjMinZ", FullDataMinMaxPosUtil.getAdjMinZ(v));
			res.p("getAdjMaxZ", FullDataMinMaxPosUtil.getAdjMaxZ(v));
			// getters applied to an arbitrary 64-bit value
			final long raw = g.anyLong();
			in.p("raw", raw);
			res.p("raw_getAdjMinX", FullDataMinMaxPosUtil.getAdjMinX(raw));
			res.p("raw_getAdjMaxX", FullDataMinMaxPosUtil.getAdjMaxX(raw));
			res.p("raw_getAdjMinZ", FullDataMinMaxPosUtil.getAdjMinZ(raw));
			res.p("raw_getAdjMaxZ", FullDataMinMaxPosUtil.getAdjMaxZ(raw));
			out.add(Obj.testCase("minmax_" + i, in, res));
		}
	}

	private static short pickShort(Gen g)
	{
		int k = g.r.nextInt(10);
		if (k == 0) { return Short.MIN_VALUE; }
		if (k == 1) { return Short.MAX_VALUE; }
		if (k < 5) { return (short) g.r.nextInt(70); }
		return (short) g.r.nextInt();
	}

	private static void varint(List<Obj> out)
	{
		Gen g = new Gen(0x7A91L);
		for (int i = 0; i < N; i++)
		{
			int v;
			int k = g.r.nextInt(10);
			if (k == 0) { v = new int[] { 0, 1, 127, 128, 16383, 16384, Integer.MAX_VALUE, -1, Integer.MIN_VALUE, 255, 256, 2097151, 2097152 }[g.r.nextInt(13)]; }
			else if (k < 7) { v = g.r.nextInt() >>> g.r.nextInt(32); }
			else { v = g.anyInt(); }
			final int value = v;
			Obj in = new Obj().p("value", value);
			Obj res = new Obj();
			res.t("writeVarint_hex", () -> writeVarintHex(value));
			final int zz = VarintUtil.zigzagEncode(value);
			res.p("zigzagEncode", zz);
			res.p("zigzagDecode_of_value", VarintUtil.zigzagDecode(value));
			res.p("zigzagRoundTrip", VarintUtil.zigzagDecode(zz));
			res.t("writeVarint_zigzag_hex", () -> writeVarintHex(zz));
			out.add(Obj.testCase("varint_" + i, in, res));
		}
	}

	private static Object writeVarintHex(int value) throws Exception
	{
		ByteArrayList bytes = new ByteArrayList();
		try (DhDataOutputStream s = DhDataOutputStream.create(EDhApiDataCompressionMode.UNCOMPRESSED, bytes))
		{
			VarintUtil.writeVarint(s, value);
		}
		return Json.hex(bytes.toByteArray());
	}
}
