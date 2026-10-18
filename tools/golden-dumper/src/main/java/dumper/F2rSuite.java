package dumper;

import com.seibel.distanthorizons.api.enums.config.EDhApiBlocksToAvoid;
import com.seibel.distanthorizons.api.enums.config.EDhApiVerticalQuality;
import com.seibel.distanthorizons.core.config.Config;
import com.seibel.distanthorizons.core.dataObjects.fullData.sources.FullDataSourceV2;
import com.seibel.distanthorizons.core.dataObjects.render.ColumnRenderSource;
import com.seibel.distanthorizons.core.dataObjects.transformers.FullDataToRenderDataTransformer;
import com.seibel.distanthorizons.core.pos.DhSectionPos;
import com.seibel.distanthorizons.core.util.FullDataPointUtil;
import com.seibel.distanthorizons.core.wrapperInterfaces.world.IClientLevelWrapper;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

/**
 * f2r: FullDataToRenderDataTransformer.transformFullDataToRenderSource(fullSource, clientLevel).
 * in : {"options":{"verticalQuality":ordinal (EDhApiVerticalQuality: 0 HEIGHT_MAP,1 LOW,2 MEDIUM,3 HIGH,4 VERY_HIGH,5 EXTREME,6 PIXEL_ART),
 *                  "blocksToIgnore":0 NONE|1 NON_COLLIDING,"tintWithAvoidedBlocks":bool,"enableCaveCulling":bool,"caveCullingHeight":int,
 *                  "ignoredRenderBlockCsv","ignoredRenderCaveBlockCsv","waterSurfaceBlockReplacementCsv","waterSubSurfaceBlockReplacementCsv"},
 *       "level":{"minHeight","maxHeight","hasCeiling","isEnd"},"source":<Ds json>}
 *       The CSVs are resource names ("minecraft:stone", see block_table resourceName), comma separated. The ignored-block set always also
 *       contains air (minecraft:air); the two water replacement sets never contain air; unknown names resolve to air.
 *       Level colour: getBlockColor = 0xFF000000 | ((block*0x9E3779B1) ^ (biome*0x85EBCA6B)) & 0xFFFFFF.
 * out: {"maxVerticalSlices":n,"yOffset":minHeight,
 *       "dataTable":[[longs]...],"dataRuns":[[tableIdx,count],...],"voidTable":[longs],"voidRuns":[[tableIdx,count],...]}
 *       dataRuns / voidRuns are run-length over all 4096 columns, index relX*64+relZ. A dataTable entry is the column's render datapoints
 *       (index 0 = highest) with trailing zero slots trimmed (the column has maxVerticalSlices slots, missing ones are 0).
 *       voidTable entries are the per-column "void data" long.
 */
public class F2rSuite implements Suite
{
	@Override public String name() { return "f2r"; }

	private static final int[][] LEVELS = { { -64, 320, 0, 0 }, { 0, 256, 0, 0 }, { -64, 320, 0, 0 }, { 0, 256, 1, 0 }, { -64, 320, 0, 1 }, { 0, 320, 0, 0 } };
	private static final int[] QUALITY = { 2, 0, 3, 1, 5, 6 };
	private static final int[] BLOCKS_TO_IGNORE = { 1, 0, 1, 0, 1, 1 };
	private static final boolean[] TINT = { true, false, false, true, true, true };
	private static final boolean[] CULL = { true, false, true, true, true, true };
	private static final int[] CULL_HEIGHT = { 60, 60, 200, -20, 60, 100 };

	private static final String DEF_IGNORE = "minecraft:barrier,minecraft:structure_void,minecraft:light,minecraft:tripwire,minecraft:brown_mushroom";
	private static final String DEF_SURFACE = "minecraft:lily_pad";
	private static final String DEF_SUB = "minecraft:kelp,minecraft:tall_seagrass,minecraft:seagrass";

	private static String[] csv(int combo)
	{
		if (combo == 5) { return new String[] { "minecraft:poppy,minecraft:glass", "minecraft:stone", "minecraft:seagrass", "minecraft:kelp,minecraft:lily_pad" }; }
		if (combo == 3) { return new String[] { DEF_IGNORE, "minecraft:dirt, minecraft:oak_leaves", DEF_SURFACE, DEF_SUB }; }
		return new String[] { DEF_IGNORE, "", DEF_SURFACE, DEF_SUB };
	}

	// ---- column DSL ----
	/** segment: {paletteId, height, skyLight, blockLight}; segments are top-down and contiguous; the lowest one starts at baseY */
	private static long[] col(int baseY, int[]... segs)
	{
		int total = 0;
		for (int[] s : segs) { total += s[1]; }
		long[] out = new long[segs.length];
		int top = baseY + total;
		try
		{
			for (int i = 0; i < segs.length; i++)
			{
				top -= segs[i][1];
				out[i] = FullDataPointUtil.encode(segs[i][0], segs[i][1], top, (byte) segs[i][3], (byte) segs[i][2]);
			}
		}
		catch (Exception e) { throw new RuntimeException(e); }
		return out;
	}
	private static int[] s(int id, int h, int sky, int blk) { return new int[] { id, h, sky, blk }; }

	private static final String[] FAMILIES = { "snow_layers", "flowers", "water_ice", "lily_kelp", "caves", "floating_island", "lava", "glass_stack",
			"same_colour", "broken_id", "barrier", "torch_stairs", "leaves", "tall_column", "snow_block", "mixed_air_gaps", "empty_flag" };

	@Override
	public List<Obj> cases()
	{
		List<Obj> out = new ArrayList<>();
		for (int f = 0; f < FAMILIES.length; f++)
		{
			for (int v = 0; v < 4; v++)
			{
				Random r = new Random(0xF2A000L + f * 16 + v);
				Ds d = fixture(FAMILIES[f], v, r);
				for (int combo = 0; combo < 6; combo++)
				{
					out.add(run(FAMILIES[f] + "_v" + v + "_c" + combo, d, combo, v));
				}
			}
		}
		Random r = new Random(0xF2A111L);
		for (int i = 0; i < 200; i++)
		{
			Ds d = randomFixture(r);
			out.add(run("random_" + i, d, r.nextInt(6), i));
		}
		return out;
	}

	private static Ds fixture(String fam, int v, Random r)
	{
		long pos = DhSectionPos.encode((byte) 6, v * 3 - 4, 5 - v);
		Ds d = new Ds(pos);
		int biome = v % 4;
		int base = 20 + v * 17 + r.nextInt(30);
		int sk = (v == 3) ? 9 : 15;
		switch (fam)
		{
			case "snow_layers":
			{
				int snow = 17 + (v % 3);
				d.addPalette(0, biome).addPalette(snow, biome).addPalette(3, biome).addPalette(2, biome).addPalette(1, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, sk, 0), s(2, 1, sk, 0), s(3, 3, 0, 0), s(4, 20, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(1, 2, sk, 0), s(2, 1, sk, 0), s(3, 3, 0, 0), s(4, 20, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, sk, 5), s(2, 1, 11, 0), s(3, 3, 0, 0), s(4, 20, 0, 0)));
				d.addCol(3, 9, 0, col(base, s(0, 20, 15, 0), s(3, 3, 0, 0), s(4, 20, 0, 0), s(1, 1, 3, 0)));
				d.addCol(64, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, sk, 0), s(0, 2, 0, 0), s(2, 1, 0, 0), s(4, 10, 0, 0)));
				d.addCol(65, 9, 0, col(base, s(1, 1, sk, 0), s(2, 1, sk, 0), s(3, 3, 0, 0)));
				break;
			}
			case "snow_block":
			{
				d.addPalette(0, biome).addPalette(20, biome).addPalette(3, biome).addPalette(2, biome).addPalette(9, biome).addPalette(1, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, sk, 0), s(2, 1, sk, 0), s(3, 3, 0, 0), s(5, 20, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(4, 1, sk, 0), s(2, 1, sk, 0), s(3, 3, 0, 0), s(5, 20, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(1, 4, sk, 0), s(5, 20, 0, 0)));
				break;
			}
			case "flowers":
			{
				d.addPalette(0, biome).addPalette(10, biome).addPalette(3, biome).addPalette(2, biome).addPalette(1, biome).addPalette(6, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, sk, 0), s(2, 1, sk, 0), s(3, 3, 0, 0), s(4, 20, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(5, 1, 15, 0), s(0, 3, 12, 0), s(1, 1, 12, 0), s(2, 1, 12, 0), s(3, 3, 0, 0), s(4, 20, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(5, 1, 15, 0), s(1, 1, 12, 3), s(2, 1, 12, 0), s(3, 3, 0, 0), s(4, 20, 0, 0)));
				d.addCol(3, 9, 0, col(base, s(0, 20, 15, 0), s(1, 2, sk, 0), s(2, 1, sk, 0), s(3, 3, 0, 0), s(4, 20, 0, 0)));
				d.addCol(4, 9, 0, col(base, s(1, 1, 0, 0), s(2, 1, 0, 0), s(4, 20, 0, 0)));
				break;
			}
			case "water_ice":
			{
				d.addPalette(0, biome).addPalette(5, biome).addPalette(4, biome).addPalette(16, biome).addPalette(1, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, sk, 0), s(2, 5, 8, 0), s(3, 4, 0, 0), s(4, 20, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(2, 6, 15, 0), s(3, 4, 0, 0), s(4, 20, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, sk, 0), s(0, 2, 6, 0), s(2, 5, 8, 0), s(3, 4, 0, 0), s(4, 20, 0, 0)));
				d.addCol(3, 9, 0, col(base, s(0, 20, 15, 0), s(1, 3, sk, 0), s(2, 5, 8, 0), s(3, 4, 0, 0), s(4, 20, 0, 0)));
				d.addCol(4, 9, 0, col(base, s(0, 20, 15, 0), s(2, 3, 15, 0), s(1, 1, 12, 0), s(2, 3, 4, 0), s(1, 2, 2, 0)));
				break;
			}
			case "lily_kelp":
			{
				d.addPalette(0, biome).addPalette(4, biome).addPalette(21, biome).addPalette(22, biome).addPalette(23, biome).addPalette(16, biome).addPalette(1, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(2, 1, 15, 0), s(1, 6, 10, 0), s(5, 4, 0, 0), s(6, 20, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(2, 2, 15, 0), s(1, 6, 10, 0), s(5, 4, 0, 0), s(6, 20, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(1, 3, 12, 0), s(3, 4, 8, 0), s(1, 2, 6, 0), s(5, 4, 0, 0), s(6, 20, 0, 0)));
				d.addCol(3, 9, 0, col(base, s(0, 20, 15, 0), s(3, 3, 15, 0), s(1, 4, 8, 0), s(5, 4, 0, 0), s(6, 20, 0, 0)));
				d.addCol(4, 9, 0, col(base, s(0, 20, 15, 0), s(1, 6, 10, 0), s(4, 2, 5, 0), s(5, 4, 0, 0)));
				d.addCol(5, 9, 0, col(base, s(0, 20, 15, 0), s(4, 1, 15, 0), s(1, 6, 10, 0), s(5, 4, 0, 0)));
				break;
			}
			case "caves":
			{
				d.addPalette(0, biome).addPalette(15, biome).addPalette(1, biome).addPalette(2, biome).addPalette(3, biome).addPalette(4, biome);
				int top = 20 + v * 30;
				d.addCol(0, 9, 0, col(5, s(0, 30, 15, 0), s(4, 1, 15, 0), s(3, 4, 0, 0), s(2, top, 0, 0), s(1, 6, 0, 0), s(2, 10, 0, 0), s(0, 5, 0, 0), s(2, 10, 0, 0)));
				d.addCol(1, 9, 0, col(5, s(0, 30, 15, 0), s(4, 1, 15, 0), s(3, 4, 0, 0), s(2, top, 0, 0), s(1, 6, 5, 0), s(2, 10, 0, 0)));
				d.addCol(2, 9, 0, col(5, s(1, 6, 0, 0), s(2, 10, 0, 0), s(1, 6, 0, 0), s(2, 5, 0, 0)));
				d.addCol(3, 9, 0, col(5, s(0, 30, 15, 0), s(4, 1, 15, 0), s(3, 4, 0, 0), s(2, top, 0, 0), s(1, 6, 0, 0)));
				d.addCol(4, 9, 0, col(5, s(0, 30, 15, 0), s(4, 1, 15, 0), s(5, 3, 0, 0), s(1, 3, 0, 0), s(5, 3, 0, 0), s(2, 10, 0, 0)));
				d.addCol(5, 9, 0, col(5, s(0, 30, 15, 0), s(2, 5, 0, 0), s(0, 4, 0, 0), s(2, 5, 0, 0), s(0, 4, 0, 0), s(2, 5, 0, 0)));
				break;
			}
			case "floating_island":
			{
				d.addPalette(0, biome).addPalette(3, biome).addPalette(2, biome).addPalette(1, biome);
				d.addCol(0, 9, 0, col(0, s(0, 200, 15, 0), s(1, 1, 15, 0), s(2, 3, 0, 0), s(3, 5, 0, 0), s(0, 60, 15, 0)));
				d.addCol(1, 9, 0, col(0, s(0, 200, 15, 0), s(1, 1, 15, 0), s(2, 3, 0, 0), s(3, 5, 0, 0)));
				d.addCol(2, 9, 0, col(40, s(0, 100, 15, 0), s(1, 1, 15, 0), s(2, 3, 0, 0)));
				d.addCol(3, 9, 0, col(0, s(0, 120, 15, 0), s(3, 4, 2, 0), s(0, 70, 15, 3)));
				d.addCol(4, 9, 0, col(0, s(0, 200, 14, 0), s(0, 10, 12, 0)));
				break;
			}
			case "lava":
			{
				d.addPalette(0, biome).addPalette(12, biome).addPalette(1, biome).addPalette(8, biome).addPalette(14, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 3, 0, 15), s(2, 20, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(3, 1, 15, 14), s(2, 4, 0, 0), s(4, 10, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(1, 3, 0, 15), s(0, 2, 0, 3), s(1, 3, 0, 15), s(4, 10, 0, 0)));
				break;
			}
			case "glass_stack":
			{
				d.addPalette(0, biome).addPalette(6, biome).addPalette(1, biome).addPalette(4, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, 15, 0), s(0, 3, 15, 0), s(1, 1, 15, 0), s(0, 3, 4, 0), s(2, 20, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(1, 4, 15, 0), s(2, 20, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(1, 2, 15, 0), s(3, 3, 12, 0), s(1, 2, 8, 0), s(2, 20, 0, 0)));
				break;
			}
			case "same_colour":
			{
				d.addPalette(0, biome).addPalette(1, biome).addPalette(2, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 3, 15, 0), s(1, 3, 4, 0), s(1, 3, 0, 0), s(2, 6, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(1, 3, 15, 0), s(0, 1, 0, 0), s(1, 3, 4, 0), s(2, 6, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(1, 3, 15, 5), s(1, 3, 15, 9), s(1, 3, 15, 0)));
				break;
			}
			case "broken_id":
			{
				d.addPalette(0, biome).addPalette(1, biome).addPalette(2, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 3, 15, 0), s(7, 3, 4, 0), s(2, 6, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(9, 20, 15, 0), s(7, 3, 15, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(1, 3, 15, 0), s(2, 6, 0, 0)));
				break;
			}
			case "barrier":
			{
				d.addPalette(0, biome).addPalette(24, biome).addPalette(1, biome).addPalette(3, biome);
				d.addCol(0, 9, 0, col(base, s(1, 1, 15, 0), s(0, 20, 15, 0), s(3, 1, 15, 0), s(2, 10, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(3, 1, 15, 0), s(2, 10, 0, 0), s(1, 4, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(1, 2, 15, 0), s(3, 1, 15, 0), s(2, 10, 0, 0)));
				d.addCol(3, 9, 0, col(base, s(1, 6, 15, 0)));
				break;
			}
			case "torch_stairs":
			{
				d.addPalette(0, biome).addPalette(8, biome).addPalette(13, biome).addPalette(1, biome).addPalette(3, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, 15, 14), s(4, 1, 15, 0), s(3, 10, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(2, 1, 15, 0), s(4, 1, 15, 0), s(3, 10, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(2, 3, 15, 0), s(3, 10, 0, 0)));
				d.addCol(3, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, 5, 14), s(0, 2, 5, 3), s(3, 10, 0, 0)));
				break;
			}
			case "leaves":
			{
				d.addPalette(0, biome).addPalette(7, biome).addPalette(1, biome).addPalette(3, biome);
				d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 2, 15, 0), s(0, 4, 13, 0), s(3, 1, 15, 0), s(2, 10, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(0, 20, 15, 0), s(1, 6, 13, 0), s(3, 1, 15, 0), s(2, 10, 0, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 20, 15, 0), s(1, 1, 15, 0), s(1, 1, 14, 0), s(0, 3, 10, 0), s(2, 10, 0, 0)));
				break;
			}
			case "tall_column":
			{
				d.addPalette(0, biome).addPalette(1, biome).addPalette(2, biome).addPalette(3, biome).addPalette(4, biome).addPalette(6, biome).addPalette(7, biome);
				int n = 30 + v * 8;
				int[][] segs = new int[n][];
				for (int i = 0; i < n; i++) { segs[i] = s(i % 7, 1 + r.nextInt(4), r.nextInt(16), r.nextInt(16)); }
				d.addCol(0, 9, 0, col(5, segs));
				int[][] segs2 = new int[12][];
				for (int i = 0; i < 12; i++) { segs2[i] = s((i * 3) % 7, 1 + r.nextInt(6), r.nextInt(16), 0); }
				d.addCol(1, 9, 0, col(5, segs2));
				break;
			}
			case "mixed_air_gaps":
			{
				d.addPalette(0, biome).addPalette(1, biome).addPalette(15, biome).addPalette(2, biome).addPalette(4, biome);
				d.addCol(0, 9, 0, col(base, s(0, 10, 15, 0), s(1, 2, 15, 0), s(2, 3, 0, 0), s(1, 2, 0, 0), s(0, 4, 0, 0), s(3, 2, 0, 0)));
				d.addCol(1, 9, 0, col(base, s(1, 2, 15, 0), s(0, 1, 15, 0), s(4, 3, 15, 0), s(0, 1, 15, 0), s(1, 3, 15, 0)));
				d.addCol(2, 9, 0, col(base, s(0, 10, 15, 0), s(4, 1, 15, 0), s(0, 1, 15, 0), s(4, 1, 15, 0), s(1, 5, 0, 0)));
				break;
			}
			case "empty_flag":
			{
				d.addPalette(0, biome).addPalette(1, biome);
				if (v >= 2) { d.addCol(0, 9, 0, col(base, s(0, 20, 15, 0), s(1, 3, 15, 0))); }
				break;
			}
			default: throw new IllegalArgumentException(fam);
		}
		if (fam.equals("empty_flag") && v % 2 == 0) { d.emptyFlag = true; }
		d.cols.sort((a, b) -> Integer.compare(a.idx, b.idx));
		return d;
	}

	private static Ds randomFixture(Random r)
	{
		Gen g = new Gen(r.nextLong());
		long pos = DhSectionPos.encode((byte) (6 + r.nextInt(3)), g.pos28() >> 8, g.pos28() >> 8);
		Ds d = new Ds(pos);
		int n = 3 + r.nextInt(8);
		boolean[] used = new boolean[Wrappers.BLOCK_COUNT];
		for (int i = 0; i < n; i++)
		{
			int b = i == 0 ? (r.nextBoolean() ? 0 : 15) : r.nextInt(Wrappers.BLOCK_COUNT);
			if (used[b]) { continue; }
			used[b] = true;
			d.addPalette(b, r.nextInt(4));
		}
		int cols = 2 + r.nextInt(8);
		List<Integer> idxs = new ArrayList<>();
		for (int i = 0; i < cols; i++)
		{
			int idx = r.nextInt(8) * 64 + r.nextInt(8);
			if (idxs.contains(idx)) { continue; }
			idxs.add(idx);
			int pts = 1 + r.nextInt(r.nextInt(6) == 0 ? 30 : 9);
			int[][] segs = new int[pts][];
			for (int k = 0; k < pts; k++)
			{
				int id = r.nextInt(d.palette.size());
				boolean air = Wrappers.block(d.palette.get(id)[0]).isAir;
				segs[k] = s(id, 1 + r.nextInt(r.nextInt(3) == 0 ? 25 : 4), air && r.nextInt(3) != 0 ? 15 : r.nextInt(16), air ? 0 : r.nextInt(16));
			}
			d.addCol(idx, 9, 0, col(r.nextInt(150), segs));
		}
		d.cols.sort((a, b) -> Integer.compare(a.idx, b.idx));
		return d;
	}

	// -------------------------------------------------------------------------------------------------

	private static Obj run(String name, Ds d, int combo, int salt)
	{
		int[] lv = LEVELS[combo];
		String[] csv = csv(combo);
		Obj options = new Obj().p("verticalQuality", QUALITY[combo]).p("blocksToIgnore", BLOCKS_TO_IGNORE[combo]).p("tintWithAvoidedBlocks", TINT[combo])
				.p("enableCaveCulling", CULL[combo]).p("caveCullingHeight", CULL_HEIGHT[combo])
				.p("ignoredRenderBlockCsv", csv[0]).p("ignoredRenderCaveBlockCsv", csv[1])
				.p("waterSurfaceBlockReplacementCsv", csv[2]).p("waterSubSurfaceBlockReplacementCsv", csv[3]);
		Obj level = new Obj().p("minHeight", lv[0]).p("maxHeight", lv[1]).p("hasCeiling", lv[2] == 1).p("isEnd", lv[3] == 1);
		Obj srcJson = d.json();
		if (d.emptyFlag) { srcJson.p("isEmpty", true); }

		Config.Client.Advanced.Graphics.Quality.verticalQuality.setWithoutFiringEvents(EDhApiVerticalQuality.values()[QUALITY[combo]]);
		Config.Client.Advanced.Graphics.Culling.blocksToIgnore.setWithoutFiringEvents(EDhApiBlocksToAvoid.values()[BLOCKS_TO_IGNORE[combo]]);
		Config.Client.Advanced.Graphics.Culling.tintWithAvoidedBlocks.setWithoutFiringEvents(TINT[combo]);
		Config.Client.Advanced.Graphics.Culling.enableCaveCulling.setWithoutFiringEvents(CULL[combo]);
		Config.Client.Advanced.Graphics.Culling.caveCullingHeight.setWithoutFiringEvents(CULL_HEIGHT[combo]);
		Config.Client.Advanced.Graphics.Texture.enableTexturedLods.setWithoutFiringEvents(false);
		Wrappers.ignoredRenderBlockCsv = csv[0];
		Wrappers.ignoredRenderCaveBlockCsv = csv[1];
		Wrappers.waterSurfaceBlockReplacementCsv = csv[2];
		Wrappers.waterSubSurfaceBlockReplacementCsv = csv[3];
		IClientLevelWrapper levelW = DumpClientLevel.create(lv[0], lv[1], lv[2] == 1, lv[3] == 1);

		FullDataSourceV2 ds = d.build();
		if (d.emptyFlag) { ds.isEmpty = true; }
		ColumnRenderSource rs = FullDataToRenderDataTransformer.transformFullDataToRenderSource(ds, levelW);

		return Obj.testCase(name, new Obj().p("options", options).p("level", level).p("source", srcJson), dumpRender(rs));
	}

	static Obj dumpRender(ColumnRenderSource rs)
	{
		int maxV = rs.maxVerticalSliceCount;
		List<Object> table = new ArrayList<>();
		List<String> keys = new ArrayList<>();
		int[] ofCol = new int[ColumnRenderSource.WIDTH * ColumnRenderSource.WIDTH];
		for (int x = 0; x < 64; x++)
		{
			for (int z = 0; z < 64; z++)
			{
				long[] c = new long[maxV];
				int last = -1;
				for (int i = 0; i < maxV; i++) { c[i] = rs.getDataPoint(x, z, i); if (c[i] != 0) { last = i; } }
				long[] t = Arrays.copyOf(c, last + 1);
				String k = Arrays.toString(t);
				int idx = keys.indexOf(k);
				if (idx < 0) { keys.add(k); table.add(t); idx = keys.size() - 1; }
				ofCol[x * 64 + z] = idx;
			}
		}
		List<Object> vtable = new ArrayList<>();
		List<Long> vkeys = new ArrayList<>();
		int[] vofCol = new int[4096];
		for (int i = 0; i < 4096; i++)
		{
			long v = rs.voidRenderDataContainer.getLong(i);
			int idx = vkeys.indexOf(v);
			if (idx < 0) { vkeys.add(v); vtable.add(v); idx = vkeys.size() - 1; }
			vofCol[i] = idx;
		}
		return new Obj().p("maxVerticalSlices", maxV).p("yOffset", rs.yOffset).p("dataTable", table).p("dataRuns", runs(ofCol))
				.p("voidTable", vtable).p("voidRuns", runs(vofCol));
	}

	static List<Object> runs(int[] a)
	{
		List<Object> out = new ArrayList<>();
		int i = 0;
		while (i < a.length)
		{
			int j = i;
			while (j < a.length && a[j] == a[i]) { j++; }
			out.add(Arrays.asList(a[i], j - i));
			i = j;
		}
		return out;
	}
}
