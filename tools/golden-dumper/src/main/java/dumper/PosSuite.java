package dumper;

import com.seibel.distanthorizons.core.enums.EDhDirection;
import com.seibel.distanthorizons.core.pos.DhChunkPos;
import com.seibel.distanthorizons.core.pos.DhSectionPos;
import com.seibel.distanthorizons.core.pos.blockPos.DhBlockPos;
import com.seibel.distanthorizons.core.pos.blockPos.DhBlockPos2D;

import java.util.ArrayList;
import java.util.List;

/**
 * DhSectionPos. Sharded (pos, pos_1, ...) to keep every file under 5 MB; together they hold TOTAL cases.
 * Every operation is wrapped so exceptions/assert failures are recorded as {"throws":"Name"}.
 */
public class PosSuite implements Suite
{
	public static final int TOTAL = 2500;
	public static final int SHARDS = 1;
	private final int shard;

	public PosSuite(int shard) { this.shard = shard; }

	@Override public String name() { return this.shard == 0 ? "pos" : "pos_" + this.shard; }

	@Override
	public List<Obj> cases()
	{
		Gen g = new Gen(0x505300L + this.shard);
		int perShard = TOTAL / SHARDS;
		List<Obj> out = new ArrayList<>(perShard);
		if (this.shard == 0)
		{
			// hand-checkable fixed inputs: {detail, x, z}
			int[][] fixed = { { 6, 1, 1 }, { 0, 0, 0 }, { 0, -1, -1 }, { 6, -1, 0 }, { 1, 0, 0 }, { 1, 5, -7 }, { 127, Gen.POS_MIN, Gen.POS_MAX }, { 10, Gen.POS_MAX, Gen.POS_MIN }, { 26, 3, -3 } };
			for (int i = 0; i < fixed.length; i++) { out.add(makeCase("fixed_" + i, g, fixed[i])); }
		}
		for (int i = 0; i < perShard; i++)
		{
			out.add(makeCase("sec_" + (this.shard * perShard + i), g, null));
		}
		return out;
	}

	private static Obj makeCase(String name, Gen g, int[] forced)
	{
		byte detailR = g.detail();
		int xR = g.pos28();
		int zR = g.pos28();
		if (forced != null) { detailR = (byte) forced[0]; xR = forced[1]; zR = forced[2]; }
		final byte detail = detailR;
		final int x = xR;
		final int z = zR;
		final byte targetDetail = g.detail();
		final byte relDetail = g.r.nextInt(4) == 0 ? g.detail() : (byte) Math.min(127, detail + g.r.nextInt(8));
		final byte returnDetail = g.r.nextInt(4) == 0 ? g.detail() : (byte) Math.max(0, detail - g.r.nextInt(8));
		final byte outDetail = g.detail();

		final long pos = DhSectionPos.encode(detail, x, z);

		// "other" section for contains(): half the time derived from pos so containment is actually exercised
		long otherTmp;
		if (g.r.nextBoolean())
		{
			byte od = (byte) Math.max(0, detail - g.r.nextInt(4));
			int diff = detail - od;
			int shift = Math.min(diff, 20);
			otherTmp = DhSectionPos.encode(od, (x << shift) + g.r.nextInt(1 << shift), (z << shift) + g.r.nextInt(1 << shift));
			if (g.r.nextInt(8) == 0) { otherTmp = DhSectionPos.getParentPos(pos); }
		}
		else
		{
			otherTmp = DhSectionPos.encode(g.detail(), g.pos28(), g.pos28());
		}
		final long other = otherTmp;

		final int cx, cz;
		{
			int bx, bz;
			if (g.r.nextBoolean())
			{
				bx = DhSectionPos.getCenterBlockPosX(pos) + g.r.nextInt(141) - 70;
				bz = DhSectionPos.getCenterBlockPosZ(pos) + g.r.nextInt(141) - 70;
			}
			else
			{
				bx = g.anyInt();
				bz = g.anyInt();
			}
			cx = bx; cz = bz;
		}
		final int blockX = cx, blockZ = cz;
		final int blockY = g.r.nextInt(6000) - 2000;
		final int chunkX = g.r.nextBoolean() ? (blockX >> 4) : g.anyInt();
		final int chunkZ = g.r.nextBoolean() ? (blockZ >> 4) : g.anyInt();

		Obj in = new Obj()
				.p("detail", (int) detail).p("x", x).p("z", z)
				.p("targetDetail", (int) targetDetail).p("relDetail", (int) relDetail).p("returnDetail", (int) returnDetail)
				.p("other", other)
				.p("blockX", blockX).p("blockY", blockY).p("blockZ", blockZ)
				.p("chunkX", chunkX).p("chunkZ", chunkZ).p("outDetail", (int) outDetail);

		Obj o = new Obj();
		o.p("encode", pos);
		o.p("getDetailLevel", (int) DhSectionPos.getDetailLevel(pos));
		o.p("getX", DhSectionPos.getX(pos));
		o.p("getZ", DhSectionPos.getZ(pos));
		o.t("getParentPos", () -> DhSectionPos.getParentPos(pos));
		for (int c = 0; c < 4; c++)
		{
			final int ci = c;
			o.t("getChildByIndex_" + c, () -> DhSectionPos.getChildByIndex(pos, ci));
		}
		o.t("getChildIndexOfParent", () -> DhSectionPos.getChildIndexOfParent(pos));
		for (EDhDirection d : EDhDirection.values())
		{
			o.t("getAdjacentPos_" + d.name(), () -> DhSectionPos.getAdjacentPos(pos, d));
		}
		o.t("convertToDetailLevel", () -> DhSectionPos.convertToDetailLevel(pos, targetDetail));
		o.t("getMinCornerBlockX", () -> DhSectionPos.getMinCornerBlockX(pos));
		o.t("getMinCornerBlockZ", () -> DhSectionPos.getMinCornerBlockZ(pos));
		o.t("getCenterBlockPosX", () -> DhSectionPos.getCenterBlockPosX(pos));
		o.t("getCenterBlockPosZ", () -> DhSectionPos.getCenterBlockPosZ(pos));
		o.t("getBlockWidth", () -> DhSectionPos.getBlockWidth(pos));
		o.t("getChunkWidth", () -> DhSectionPos.getChunkWidth(pos));
		o.t("getWidthCountForLowerDetailedSection", () -> DhSectionPos.getWidthCountForLowerDetailedSection(pos, returnDetail));
		o.t("contains_pos", () -> DhSectionPos.contains(pos, other));
		o.t("contains_block", () -> DhSectionPos.contains(pos, new DhBlockPos(blockX, blockY, blockZ)));
		o.t("getDhSectionRelativePositionForDetailLevel", () -> DhSectionPos.getDhSectionRelativePositionForDetailLevel(pos, relDetail));
		o.t("getChebyshevSignedBlockDistance", () -> DhSectionPos.getChebyshevSignedBlockDistance(pos, blockX, blockZ));
		o.t("getManhattanBlockDistance", () -> DhSectionPos.getManhattanBlockDistance(pos, new DhBlockPos2D(blockX, blockZ)));
		o.t("encodeContaining_block", () -> DhSectionPos.encodeContaining(outDetail, new DhBlockPos(blockX, blockY, blockZ)));
		o.t("encodeContaining_chunk", () -> DhSectionPos.encodeContaining(outDetail, new DhChunkPos(chunkX, chunkZ)));

		return Obj.testCase(name, in, o);
	}
}
