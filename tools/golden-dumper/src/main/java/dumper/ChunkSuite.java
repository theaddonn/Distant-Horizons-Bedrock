package dumper;

import com.seibel.distanthorizons.api.enums.config.EDhApiWorldCompressionMode;
import com.seibel.distanthorizons.core.config.Config;
import com.seibel.distanthorizons.core.dataObjects.fullData.sources.FullDataSourceV2;
import com.seibel.distanthorizons.core.dataObjects.transformers.LodDataBuilder;
import com.seibel.distanthorizons.core.generation.DhLightingEngine;
import com.seibel.distanthorizons.core.wrapperInterfaces.chunk.IChunkWrapper;
import com.seibel.distanthorizons.core.wrapperInterfaces.world.IClientLevelWrapper;
import it.unimi.dsi.fastutil.longs.LongArrayList;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

/**
 * chunk_build: LodDataBuilder.createFromChunk(level, DumpChunk).
 * in : {"worldCompression":0|1, "lit":bool, "chunk":<ChunkFx json>}
 *      lit=true: the chunk was first lit with DhLightingEngine.INSTANCE.lightChunk(chunk, [chunk], 15) (only this one chunk,
 *      createDhHeightMaps() is always called before). lit=false: no DH light data (block light 0, sky light 0 inside the
 *      build height, 15 above it; the lighting-correct flags are set).
 * out: {"pos","palette":[{"block","biome"}],"genStep","compMode","isEmpty","applyToParent","offsetX","offsetZ",
 *       "colTable":[[longs]...],"colRuns":[[tableIdx,count],...],"otherNonEmpty":n}
 *      colRuns is run-length over the chunk's 256 columns, index x*16+z (data source column = (offsetX+x)*64 + offsetZ+z).
 *      otherNonEmpty = number of data-source columns outside the chunk's 16x16 area that contain datapoints (always 0).
 */
public class ChunkSuite implements Suite
{
	@Override public String name() { return "chunk_build"; }

	private static final int[][] VARIANTS = {
			// chunkX, chunkZ, minY, maxY, lit, worldCompression
			{ 0, 0, -64, 320, 0, 0 },
			{ -5, 7, 0, 320, 1, 1 },
			{ 4, -4, -64, 320, 1, 0 },
			{ -1, -1, 0, 256, 0, 1 },
	};

	@Override
	public List<Obj> cases()
	{
		List<Obj> out = new ArrayList<>();
		String[] names = { "flat", "stairs", "snow_layer", "flower_field", "water_over_ice", "water_column", "caves", "floating",
				"leaves_canopy", "lava_lake", "glass_roof", "torch_glowstone", "empty", "void_deepslate", "full_height", "biome_layers", "cliffs" };
		for (int n = 0; n < names.length; n++)
		{
			for (int v = 0; v < VARIANTS.length; v++)
			{
				int[] var = VARIANTS[v];
				Random r = new Random(0xC4B1D0L + n * 31 + v);
				ChunkFx f = new ChunkFx(var[0], var[1], var[2], var[3]);
				fixture(names[n], f, r);
				out.add(run(names[n] + "_v" + v, f, var[4] == 1, var[5]));
			}
		}
		Random r = new Random(0xC4B1D1L);
		for (int i = 0; i < 200; i++)
		{
			int minY = r.nextBoolean() ? -64 : 0;
			int maxY = r.nextInt(3) == 0 ? 256 : 320;
			ChunkFx f = new ChunkFx(r.nextInt(2001) - 1000, r.nextInt(2001) - 1000, minY, maxY);
			randomFixture(f, r);
			out.add(run("random_" + i, f, r.nextInt(3) != 0, r.nextInt(2)));
		}
		return out;
	}

	private static int surfaceBase(ChunkFx f) { return 64 + (f.minY == 0 ? 0 : 0); }

	private static void fixture(String name, ChunkFx f, Random r)
	{
		int base = surfaceBase(f);
		f.fillBiome(0);
		switch (name)
		{
			case "flat":
				ChunkFx.terrain(f, ChunkFx.flat(base));
				break;
			case "stairs":
				ChunkFx.terrain(f, ChunkFx.flat(base));
				for (int x = 3; x < 12; x++) { for (int z = 4; z < 9; z++) { f.fill(x, z, base, base + (x - 2) / 2, ChunkFx.STAIRS); } }
				break;
			case "snow_layer":
				ChunkFx.terrain(f, ChunkFx.flat(base));
				for (int x = 0; x < 16; x++) { for (int z = 0; z < 16; z++) { f.set(x, base, z, ChunkFx.SNOW); } }
				f.fillBiome(1);
				break;
			case "flower_field":
				ChunkFx.terrain(f, ChunkFx.hills(r, base - 3, 6));
				for (int x = 0; x < 16; x++) { for (int z = 0; z < 16; z++) { if (r.nextBoolean()) { f.set(x, surfaceOf(f, x, z), z, ChunkFx.POPPY); } } }
				break;
			case "water_over_ice":
				ChunkFx.terrain(f, ChunkFx.flat(base - 3));
				for (int x = 0; x < 16; x++) { for (int z = 0; z < 16; z++) { f.fill(x, z, base - 3, base - 1, ChunkFx.ICE); f.fill(x, z, base - 1, base + 2, ChunkFx.WATER); } }
				f.fillBiome(2);
				break;
			case "water_column":
				for (int x = 0; x < 16; x++)
				{
					for (int z = 0; z < 16; z++)
					{
						f.fill(x, z, f.minY, 40, ChunkFx.STONE);
						f.fill(x, z, 40, 50, ChunkFx.SAND);
						f.fill(x, z, 50, 63, ChunkFx.WATER);
					}
				}
				f.fillBiome(3);
				break;
			case "caves":
				ChunkFx.terrain(f, ChunkFx.hills(r, base, 10));
				for (int k = 0; k < 6; k++)
				{
					int cx = r.nextInt(16), cz = r.nextInt(16), cy = f.minY + 10 + r.nextInt(80), rad = 2 + r.nextInt(3);
					for (int x = Math.max(0, cx - rad); x < Math.min(16, cx + rad); x++)
					{
						for (int z = Math.max(0, cz - rad); z < Math.min(16, cz + rad); z++)
						{
							for (int y = cy - 2; y < cy + 2 + r.nextInt(2); y++) { f.set(x, y, z, r.nextInt(3) == 0 ? ChunkFx.CAVE_AIR : ChunkFx.AIR); }
						}
					}
				}
				// one shaft open to the sky
				for (int y = f.minY + 30; y < f.maxY; y++) { f.set(7, y, 7, ChunkFx.AIR); }
				break;
			case "floating":
				ChunkFx.terrain(f, ChunkFx.flat(base));
				f.set(5, base + 10, 5, ChunkFx.STONE);
				f.set(6, base + 10, 5, ChunkFx.STONE);
				f.set(5, base + 3, 5, ChunkFx.TORCH);
				f.set(10, base + 20, 10, ChunkFx.GLOWSTONE);
				break;
			case "leaves_canopy":
				ChunkFx.terrain(f, ChunkFx.flat(base));
				for (int x = 2; x < 14; x++) { for (int z = 2; z < 14; z++) { f.fill(x, z, base + 5, base + 7, ChunkFx.LEAVES); } }
				for (int y = base; y < base + 5; y++) { f.set(8, y, 8, ChunkFx.DIRT); }
				break;
			case "lava_lake":
				ChunkFx.terrain(f, ChunkFx.flat(base));
				for (int x = 3; x < 13; x++) { for (int z = 3; z < 13; z++) { f.fill(x, z, base - 3, base, ChunkFx.LAVA); } }
				break;
			case "glass_roof":
				ChunkFx.terrain(f, ChunkFx.flat(base));
				for (int x = 0; x < 16; x++) { for (int z = 0; z < 16; z++) { f.set(x, base + 6, z, ChunkFx.GLASS); } }
				f.set(4, base + 1, 4, ChunkFx.TORCH);
				break;
			case "torch_glowstone":
				ChunkFx.terrain(f, ChunkFx.flat(base));
				f.set(0, base, 0, ChunkFx.TORCH);
				f.set(15, base, 15, ChunkFx.GLOWSTONE);
				f.set(8, base + 1, 0, ChunkFx.TORCH);
				break;
			case "empty":
				break;
			case "void_deepslate":
				for (int x = 0; x < 16; x++) { for (int z = 0; z < 16; z++) { f.fill(x, z, f.minY, f.minY + 2, ChunkFx.DEEPSLATE); } }
				break;
			case "full_height":
				for (int x = 0; x < 16; x++) { for (int z = 0; z < 16; z++) { f.fill(x, z, f.minY, f.maxY, ((x + z) & 1) == 0 ? ChunkFx.STONE : ChunkFx.DIRT); } }
				break;
			case "biome_layers":
				ChunkFx.terrain(f, ChunkFx.hills(r, base - 4, 10));
				for (int q = 0; q < 16; q++)
				{
					for (int qy = 0; qy < f.biome[q].length; qy++) { f.biome[q][qy] = qy < 16 ? 2 : ((q & 1) == 0 ? 0 : 1); }
				}
				for (int x = 0; x < 16; x++) { for (int z = 0; z < 16; z++) { if (((x ^ z) & 5) == 0) { f.set(x, surfaceOf(f, x, z), z, ChunkFx.POPPY); } } }
				break;
			case "cliffs":
				int[][] s = new int[16][16];
				for (int x = 0; x < 16; x++) { for (int z = 0; z < 16; z++) { s[x][z] = base + ((x / 4) * 7) % 23 - (z / 8) * 11; } }
				ChunkFx.terrain(f, s);
				break;
			default:
				throw new IllegalArgumentException(name);
		}
	}

	/** first air Y above the highest non-air block of the column (used to place plants) */
	private static int surfaceOf(ChunkFx f, int x, int z)
	{
		for (int y = f.maxY - 1; y >= f.minY; y--) { if (f.get(x, y, z) != 0) { return y + 1; } }
		return f.minY;
	}

	private static void randomFixture(ChunkFx f, Random r)
	{
		int base = 40 + r.nextInt(60);
		ChunkFx.terrain(f, ChunkFx.hills(r, base, 4 + r.nextInt(30)));
		f.fillBiome(r.nextInt(4));
		for (int i = 0; i < 16; i++) { if (r.nextInt(5) == 0) { Arrays.fill(f.biome[i], r.nextInt(4)); } }
		int features = r.nextInt(5);
		for (int k = 0; k < features; k++)
		{
			int x = r.nextInt(16), z = r.nextInt(16);
			switch (r.nextInt(8))
			{
				case 0: f.set(x, surfaceOf(f, x, z), z, ChunkFx.POPPY); break;
				case 1: f.set(x, surfaceOf(f, x, z), z, ChunkFx.SNOW); break;
				case 2: f.set(x, surfaceOf(f, x, z), z, ChunkFx.TORCH); break;
				case 3: f.fill(x, z, surfaceOf(f, x, z) + 3, surfaceOf(f, x, z) + 5, ChunkFx.LEAVES); break;
				case 4: { int s = surfaceOf(f, x, z); f.fill(x, z, s, s + 1 + r.nextInt(3), ChunkFx.WATER); break; }
				case 5: f.fill(x, z, 20 + r.nextInt(20), 24 + r.nextInt(8), r.nextBoolean() ? ChunkFx.AIR : ChunkFx.CAVE_AIR); break;
				case 6: f.set(x, surfaceOf(f, x, z) + 2 + r.nextInt(10), z, r.nextBoolean() ? ChunkFx.GLASS : ChunkFx.STONE); break;
				default: f.set(x, surfaceOf(f, x, z), z, ChunkFx.STAIRS); break;
			}
		}
	}

	// ------------------------------------------------------------------------------------------------

	static final IClientLevelWrapper LEVEL = Stub.of(IClientLevelWrapper.class).build();

	private Obj run(String name, ChunkFx f, boolean lit, int wc)
	{
		Config.Common.LodBuilding.worldCompression.set(wc == 1 ? EDhApiWorldCompressionMode.VISUALLY_EQUAL : EDhApiWorldCompressionMode.MERGE_SAME_BLOCKS);
		DumpChunk c = f.toChunk();
		c.createDhHeightMaps();
		if (lit)
		{
			ArrayList<IChunkWrapper> near = new ArrayList<>();
			near.add(c);
			DhLightingEngine.INSTANCE.lightChunk(c, near, 15);
		}
		else
		{
			c.setIsDhBlockLightCorrect(true);
			c.setIsDhSkyLightCorrect(true);
		}
		FullDataSourceV2 ds = LodDataBuilder.createFromChunk(LEVEL, c);

		Obj in = new Obj().p("worldCompression", wc).p("lit", lit).p("chunk", f.json());
		return Obj.testCase(name, in, dumpChunkSource(ds, f.chunkX, f.chunkZ));
	}

	static Obj dumpChunkSource(FullDataSourceV2 ds, int chunkX, int chunkZ)
	{
		int offX = (chunkX & 3) * 16, offZ = (chunkZ & 3) * 16;
		List<Object> table = new ArrayList<>();
		List<String> keys = new ArrayList<>();
		List<Object> runs = new ArrayList<>();
		int lastIdx = -1, lastCount = 0;
		int gen = ds.columnGenerationSteps.getByte(Ds.idx(offX, offZ));
		int comp = ds.columnWorldCompressionMode.getByte(Ds.idx(offX, offZ));
		int other = 0;
		for (int i = 0; i < Ds.W * Ds.W; i++)
		{
			int x = i / Ds.W, z = i % Ds.W;
			boolean inside = x >= offX && x < offX + 16 && z >= offZ && z < offZ + 16;
			if (!inside && ds.dataPoints[i] != null && !ds.dataPoints[i].isEmpty()) { other++; }
		}
		for (int x = 0; x < 16; x++)
		{
			for (int z = 0; z < 16; z++)
			{
				int di = Ds.idx(offX + x, offZ + z);
				if (ds.columnGenerationSteps.getByte(di) != gen || ds.columnWorldCompressionMode.getByte(di) != comp) { throw new IllegalStateException("non-uniform gen/comp"); }
				LongArrayList l = ds.dataPoints[di];
				long[] pts = l.toLongArray();
				String k = Arrays.toString(pts);
				int ti = keys.indexOf(k);
				if (ti < 0) { keys.add(k); table.add(pts); ti = keys.size() - 1; }
				if (ti == lastIdx) { lastCount++; }
				else
				{
					if (lastIdx >= 0) { runs.add(Arrays.asList(lastIdx, lastCount)); }
					lastIdx = ti; lastCount = 1;
				}
			}
		}
		runs.add(Arrays.asList(lastIdx, lastCount));
		List<Object> pal = new ArrayList<>();
		for (int id = 0; id < ds.mapping.size(); id++)
		{
			pal.add(new Obj().p("block", ((Wrappers.DumpBlockState) ds.mapping.getBlockStateWrapper(id)).serial)
					.p("biome", ((Wrappers.DumpBiome) ds.mapping.getBiomeWrapper(id)).serial));
		}
		return new Obj().p("pos", ds.getPos()).p("palette", pal).p("genStep", gen).p("compMode", comp).p("isEmpty", ds.isEmpty)
				.p("applyToParent", ds.applyToParent).p("offsetX", offX).p("offsetZ", offZ)
				.p("colTable", table).p("colRuns", runs).p("otherNonEmpty", other);
	}
}
