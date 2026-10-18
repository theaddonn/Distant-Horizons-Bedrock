package dumper;

import com.seibel.distanthorizons.core.util.RenderDataPointUtil;

import java.util.ArrayList;
import java.util.List;

/**
 * RenderDataPointUtil. NOTE: validation is ON in this build (IS_DEV_BUILD), so createDataPoint throws
 * AssertFailureException for maxY/minY >= 6095, minY > maxY, alpha/rgb outside 0..255, etc.
 * maxY in 4096..6094 passes validation but is truncated to 12 bits in the packed value.
 * Sharded (rdp, rdp_1) to keep each file under 5 MB.
 */
public class RdpSuite implements Suite
{
	public static final int TOTAL = 2000;
	public static final int SHARDS = 1;
	/** extra raw-long cases appended to shard 0 */
	public static final int RAW = 1000;
	private final int shard;

	public RdpSuite(int shard) { this.shard = shard; }

	@Override public String name() { return this.shard == 0 ? "rdp" : "rdp_" + this.shard; }

	@Override
	public List<Obj> cases()
	{
		Gen g = new Gen(0x7D9001L + this.shard);
		int per = TOTAL / SHARDS;
		List<Obj> out = new ArrayList<>(per);
		for (int i = 0; i < per; i++)
		{
			out.add(createCase("rdp_" + (this.shard * per + i), g));
		}
		if (this.shard == 0)
		{
			for (int i = 0; i < RAW; i++) { out.add(rawCase("raw_" + i, g)); }
		}
		return out;
	}

	private static int ch(Gen g)
	{
		int k = g.r.nextInt(20);
		if (k == 0) { return new int[] { 0, 255, 256, -1, 15, 16, 1000 }[g.r.nextInt(7)]; }
		return g.r.nextInt(256);
	}

	private static Obj createCase(String name, Gen g)
	{
		final int alpha = ch(g), red = ch(g), green = ch(g), blue = ch(g);
		int maxYv, minYv;
		int k = g.r.nextInt(20);
		if (k == 0) { maxYv = new int[] { 0, 4095, 4096, 6094, 6095, -1, 8191 }[g.r.nextInt(7)]; minYv = new int[] { 0, 1, 4095, 4096, 6094, -1 }[g.r.nextInt(6)]; }
		else
		{
			maxYv = g.r.nextBoolean() ? g.r.nextInt(6095) : g.r.nextInt(512);
			minYv = g.r.nextInt(maxYv + 1);
		}
		final int maxY = maxYv, minY = minYv;
		final int sky = g.r.nextInt(30) == 0 ? new int[] { -1, 16, 255 }[g.r.nextInt(3)] : g.r.nextInt(16);
		final int block = g.r.nextInt(30) == 0 ? new int[] { -1, 16, 255 }[g.r.nextInt(3)] : g.r.nextInt(16);
		final int mat = g.r.nextInt(40) == 0 ? new int[] { 16, 255, 256, -1 }[g.r.nextInt(4)] : g.r.nextInt(16);
		final int color = (alpha << 24) | ((red & 0xFF) << 16) | ((green & 0xFF) << 8) | (blue & 0xFF);
		final int newMinY = g.r.nextBoolean() ? g.r.nextInt(4096) : g.anyInt();

		Obj in = new Obj().p("alpha", alpha).p("red", red).p("green", green).p("blue", blue)
				.p("maxY", maxY).p("minY", minY).p("lightSky", sky).p("lightBlock", block).p("materialId", mat)
				.p("color", color).p("newMinY", newMinY);

		Obj res = new Obj();
		res.t("createDataPoint", () -> RenderDataPointUtil.createDataPoint(alpha, red, green, blue, maxY, minY, sky, block, mat));
		res.t("createDataPoint_color", () -> RenderDataPointUtil.createDataPoint(maxY, minY, color, sky, block, mat));
		Object packed = res.get("createDataPoint");

		// partner datapoint (also valid-ish) for compareDatapointPriority
		long other = RenderDataPointUtil.createDataPoint(g.r.nextInt(256), g.r.nextInt(256), g.r.nextInt(256), g.r.nextInt(256),
				g.r.nextInt(6095), 0, g.r.nextInt(16), g.r.nextInt(16), g.r.nextInt(16));
		if (g.r.nextInt(4) == 0) { other = g.r.nextInt(3) == 0 ? 0L : (packed instanceof Long ? ((Long) packed ^ (long) g.r.nextInt(16)) : g.anyLong()); }
		final long otherF = other;
		in.p("other", other);

		if (packed instanceof Long)
		{
			addGetters(res, (Long) packed, otherF, newMinY);
		}
		return Obj.testCase(name, in, res);
	}

	private static Obj rawCase(String name, Gen g)
	{
		final long raw = g.anyLong();
		final long other = g.r.nextBoolean() ? g.anyLong() : raw ^ (1L << g.r.nextInt(64));
		final int newMinY = g.anyInt();
		Obj in = new Obj().p("raw", raw).p("other", other).p("newMinY", newMinY);
		Obj res = new Obj();
		addGetters(res, raw, other, newMinY);
		return Obj.testCase(name, in, res);
	}

	private static void addGetters(Obj res, long dp, long other, int newMinY)
	{
		res.p("getYMax", (int) RenderDataPointUtil.getYMax(dp));
		res.p("getYMin", (int) RenderDataPointUtil.getYMin(dp));
		res.p("getAlpha", (int) RenderDataPointUtil.getAlpha(dp));
		res.p("getRed", (int) RenderDataPointUtil.getRed(dp));
		res.p("getGreen", (int) RenderDataPointUtil.getGreen(dp));
		res.p("getBlue", (int) RenderDataPointUtil.getBlue(dp));
		res.p("getLightSky", (int) RenderDataPointUtil.getLightSky(dp));
		res.p("getLightBlock", (int) RenderDataPointUtil.getLightBlock(dp));
		res.p("getBlockMaterialId", (int) RenderDataPointUtil.getBlockMaterialId(dp));
		res.p("getColor", RenderDataPointUtil.getColor(dp));
		res.p("doesDataPointExist", RenderDataPointUtil.doesDataPointExist(dp));
		res.p("hasZeroHeight", RenderDataPointUtil.hasZeroHeight(dp));
		// RenderDataPointUtil has no isVoid(); "void" in toString() == exists && hasZeroHeight
		res.p("isVoid_derived", RenderDataPointUtil.doesDataPointExist(dp) && RenderDataPointUtil.hasZeroHeight(dp));
		res.p("setYMin", RenderDataPointUtil.setYMin(dp, newMinY));
		res.p("compareDatapointPriority", RenderDataPointUtil.compareDatapointPriority(dp, other));
		res.p("compareDatapointPriority_rev", RenderDataPointUtil.compareDatapointPriority(other, dp));
	}
}
