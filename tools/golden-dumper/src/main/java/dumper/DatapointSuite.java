package dumper;

import com.seibel.distanthorizons.core.util.FullDataPointUtil;

import java.util.ArrayList;
import java.util.List;

/**
 * FullDataPointUtil. NOTE: the DH build here has ModInfo.IS_DEV_BUILD == true ("3.3.5-dev"), so
 * FullDataPointUtil.RUN_VALIDATION is on: encode() throws DataCorruptedException / AssertFailureException
 * on out-of-range input. Those cases are recorded as {"throws":"..."}; the C++ port must decide whether to mirror them.
 */
public class DatapointSuite implements Suite
{
	public static final int TOTAL = 2500;
	public static final int SHARDS = 1;
	private final int shard;
	
	public DatapointSuite(int shard) { this.shard = shard; }

	@Override public String name() { return this.shard == 0 ? "datapoint" : "datapoint_" + this.shard; }

	private static int pickHeight(Gen g)
	{
		int k = g.r.nextInt(12);
		if (k == 0) { return new int[] { 0, 1, 2, 4095, 4096, 6094, 6095, -1, 4064, 100 }[g.r.nextInt(10)]; }
		if (k < 6) { return 1 + g.r.nextInt(300); }
		return 1 + g.r.nextInt(4095);
	}

	private static int pickMinY(Gen g)
	{
		int k = g.r.nextInt(12);
		if (k == 0) { return new int[] { 0, 1, 4095, 4096, 6094, 6095, -1, 2031, 2032 }[g.r.nextInt(9)]; }
		if (k < 6) { return g.r.nextInt(400); }
		return g.r.nextInt(4096);
	}

	private static int pickLight(Gen g)
	{
		int k = g.r.nextInt(14);
		if (k == 0) { return new int[] { -1, 16, 17, 127, -128, 255 & 0xFF }[g.r.nextInt(6)]; }
		return g.r.nextInt(16);
	}

	@Override
	public List<Obj> cases()
	{
		Gen g = new Gen(0xDA7A901L + this.shard);
		int N = TOTAL / SHARDS;
		List<Obj> out = new ArrayList<>(N);
		for (int i = 0; i < N; i++)
		{
			final int id = g.r.nextInt(10) == 0 ? new int[] { 0, 1, Integer.MAX_VALUE, Integer.MIN_VALUE, -1, 65535, 1 << 30 }[g.r.nextInt(7)] : g.r.nextInt(g.r.nextBoolean() ? 1000 : Integer.MAX_VALUE);
			final int height = pickHeight(g);
			final int minY = pickMinY(g);
			// block light / sky light are `byte` parameters of encode()
			final byte blockLight = (byte) pickLight(g);
			final byte skyLight = (byte) pickLight(g);

			// arbitrary packed value for getters/setters
			final long raw = g.anyLong();
			final int newId = g.anyInt();
			final int newHeight = g.r.nextBoolean() ? g.r.nextInt(4096) : g.anyInt();
			final int newMinY = g.r.nextBoolean() ? g.r.nextInt(4096) : g.anyInt();
			final byte newBlockLight = (byte) (g.r.nextBoolean() ? g.r.nextInt(16) : g.r.nextInt(256) - 128);
			final int newSkyLight = g.r.nextBoolean() ? g.r.nextInt(16) : g.anyInt();

			Obj in = new Obj().p("id", id).p("height", height).p("minY", minY)
					.p("blockLight", (int) blockLight).p("skyLight", (int) skyLight)
					.p("raw", raw).p("newId", newId).p("newHeight", newHeight).p("newMinY", newMinY)
					.p("newBlockLight", (int) newBlockLight).p("newSkyLight", newSkyLight);

			Obj res = new Obj();
			// encode(id, height, relMinY, byte blockLight, byte skyLight)
			res.t("encode", () -> FullDataPointUtil.encode(id, height, minY, blockLight, skyLight));
			Object enc = res.get("encode");
			if (enc instanceof Long)
			{
				long e = (Long) enc;
				res.p("encoded_getId", FullDataPointUtil.getId(e));
				res.p("encoded_getHeight", FullDataPointUtil.getHeight(e));
				res.p("encoded_getBottomY", FullDataPointUtil.getBottomY(e));
				res.p("encoded_getBlockLight", FullDataPointUtil.getBlockLight(e));
				res.p("encoded_getSkyLight", FullDataPointUtil.getSkyLight(e));
			}

			res.p("getId", FullDataPointUtil.getId(raw));
			res.p("getHeight", FullDataPointUtil.getHeight(raw));
			res.p("getBottomY", FullDataPointUtil.getBottomY(raw));
			res.p("getBlockLight", FullDataPointUtil.getBlockLight(raw));
			res.p("getSkyLight", FullDataPointUtil.getSkyLight(raw));
			res.p("setId", FullDataPointUtil.setId(raw, newId));
			res.p("setHeight", FullDataPointUtil.setHeight(raw, newHeight));
			res.p("setBottomY", FullDataPointUtil.setBottomY(raw, newMinY));
			// setBlockLight / setSkyLight do NOT mask the new value before OR-ing (and sign-extend negatives)
			res.p("setBlockLight", FullDataPointUtil.setBlockLight(raw, newBlockLight));
			res.p("setSkyLight", FullDataPointUtil.setSkyLight(raw, newSkyLight));
			res.t("validateDatapoint", () -> { FullDataPointUtil.validateDatapoint(raw); return true; });
			out.add(Obj.testCase("dp_" + (this.shard * N + i), in, res));
		}
		return out;
	}
}
