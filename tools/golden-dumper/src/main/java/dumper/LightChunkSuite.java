package dumper;

import com.seibel.distanthorizons.core.generation.DhLightingEngine;
import com.seibel.distanthorizons.core.wrapperInterfaces.chunk.IChunkWrapper;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

/**
 * light_chunk: DhLightingEngine.INSTANCE.lightChunk(center, nearbyChunks, maxSkyLight).
 * in : {"maxSkyLight":int,"centerChunkX":int,"centerChunkZ":int,"chunks":[{"dx":-1..1,"dz":-1..1,"chunk":<ChunkFx json>},...]}
 *      the chunks passed in `nearbyChunkList`, in this order (the centre chunk, dx=dz=0, is always present; absent
 *      neighbours are simply not in the list). Chunk json is the same encoding as chunk_build (profiles/colRuns/biomeRuns).
 * out: {"skyCorrect":bool,"blockCorrect":bool,"colTable":[[[v,count],...]...],"colRuns":[[tableIdx,count],...]}
 *      for the CENTRE chunk only: per column (order x*16+z) a run-length list over y from minY to maxY-1 of the packed light
 *      v = skyLight*16 + blockLight (ChunkLightStorage values after lighting). colRuns is run-length over the 256 columns.
 */
public class LightChunkSuite implements Suite
{
	@Override public String name() { return "light_chunk"; }

	interface World { int at(int wx, int wy, int wz); }

	private static final String[] NAMES = {
			"open_flat", "torch_center", "torch_cross_chunk", "torch_corner", "glowstone_cluster", "cave_enclosed", "cave_open_shaft",
			"overhang_leaves", "water_pool", "glass_roof", "leaves_canopy", "tunnel_cross_chunk", "lava_lake", "multi_torch",
			"hills", "tall_world", "snow_layer", "void", "no_sky", "half_sky", "stairs", "pillar_full_height", "missing_neighbours" };

	@Override
	public List<Obj> cases()
	{
		List<Obj> out = new ArrayList<>();
		for (int n = 0; n < NAMES.length; n++)
		{
			int minY = NAMES[n].equals("tall_world") ? -64 : 0;
			int maxY = NAMES[n].equals("tall_world") ? 320 : 64;
			int cx = (n % 3 == 0) ? 0 : (n % 3 == 1 ? -3 : 7), cz = (n % 2 == 0) ? 0 : -6;
			int maxSky = NAMES[n].equals("no_sky") ? 0 : NAMES[n].equals("half_sky") ? 8 : 15;
			World w = world(NAMES[n], cx, cz, new Random(0x116C4L + n));
			boolean missing = NAMES[n].equals("missing_neighbours");
			out.add(run(NAMES[n], cx, cz, minY, maxY, maxSky, w, missing ? new Random(5) : null));
		}
		for (int i = 0; i < 12; i++)
		{
			Random r = new Random(0x116C5L + i);
			int cx = r.nextInt(41) - 20, cz = r.nextInt(41) - 20;
			World w = randomWorld(r, cx, cz);
			out.add(run("random_" + i, cx, cz, 0, 64, r.nextInt(8) == 0 ? 10 : 15, w, r.nextInt(3) == 0 ? new Random(i) : null));
		}
		return out;
	}

	private static Obj run(String name, int cx, int cz, int minY, int maxY, int maxSky, World w, Random dropRandom)
	{
		List<Object> chunkJson = new ArrayList<>();
		ArrayList<IChunkWrapper> list = new ArrayList<>();
		DumpChunk center = null;
		for (int dz = -1; dz <= 1; dz++)
		{
			for (int dx = -1; dx <= 1; dx++)
			{
				if ((dx != 0 || dz != 0) && dropRandom != null && dropRandom.nextInt(3) == 0) { continue; }
				ChunkFx f = new ChunkFx(cx + dx, cz + dz, minY, maxY);
				f.fillBiome(0);
				for (int x = 0; x < 16; x++)
				{
					for (int z = 0; z < 16; z++)
					{
						for (int y = minY; y < maxY; y++) { f.col[x * 16 + z][y - minY] = w.at((cx + dx) * 16 + x, y, (cz + dz) * 16 + z); }
					}
				}
				chunkJson.add(new Obj().p("dx", dx).p("dz", dz).p("chunk", f.json()));
				DumpChunk c = f.toChunk();
				list.add(c);
				if (dx == 0 && dz == 0) { center = c; }
			}
		}
		DhLightingEngine.INSTANCE.lightChunk(center, list, maxSky);

		List<Object> table = new ArrayList<>();
		List<String> keys = new ArrayList<>();
		int[] tableOfCol = new int[256];
		for (int x = 0; x < 16; x++)
		{
			for (int z = 0; z < 16; z++)
			{
				int[] vals = new int[maxY - minY];
				for (int y = minY; y < maxY; y++) { vals[y - minY] = center.getDhSkyLight(x, y, z) * 16 + center.getDhBlockLight(x, y, z); }
				String k = Arrays.toString(vals);
				int idx = keys.indexOf(k);
				if (idx < 0) { keys.add(k); table.add(runs(vals)); idx = keys.size() - 1; }
				tableOfCol[x * 16 + z] = idx;
			}
		}
		Obj in = new Obj().p("maxSkyLight", maxSky).p("centerChunkX", cx).p("centerChunkZ", cz).p("chunks", chunkJson);
		Obj res = new Obj().p("skyCorrect", center.isDhSkyLightCorrect()).p("blockCorrect", center.isDhBlockLightingCorrect())
				.p("colTable", table).p("colRuns", runs(tableOfCol));
		return Obj.testCase(name, in, res);
	}

	private static List<Object> runs(int[] a)
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

	// ------------------------------------------------------------------------------------------------ worlds

	private static final int AIR = 0, STONE = 1, DIRT = 2, GRASS = 3, WATER = 4, GLASS = 6, LEAVES = 7, TORCH = 8, SNOW = 9, GLOW = 11, LAVA = 12, STAIRS = 13, SAND = 16;

	private static World flat(int h) { return (x, y, z) -> y < h - 4 ? STONE : y < h - 1 ? DIRT : y < h ? GRASS : AIR; }

	/** world coordinates of chunk (cx,cz) block (bx,bz) */
	private static int wx(int cx, int bx) { return cx * 16 + bx; }

	private static World world(String name, int cx, int cz, Random r)
	{
		final int x0 = wx(cx, 0), z0 = wx(cz, 0);
		World base = flat(30);
		switch (name)
		{
			case "open_flat": return base;
			case "torch_center": return (x, y, z) -> (x == x0 + 8 && z == z0 + 8 && y == 30) ? TORCH : base.at(x, y, z);
			case "torch_cross_chunk": return (x, y, z) -> (x == x0 - 1 && z == z0 + 5 && y == 30) ? TORCH : base.at(x, y, z);
			case "torch_corner": return (x, y, z) -> (x == x0 - 2 && z == z0 - 2 && y == 31) ? GLOW : base.at(x, y, z);
			case "glowstone_cluster": return (x, y, z) -> (y >= 30 && y <= 32 && x >= x0 + 14 && x <= x0 + 17 && z >= z0 + 6 && z <= z0 + 8) ? GLOW : base.at(x, y, z);
			case "cave_enclosed": return (x, y, z) -> (y > 10 && y < 18 && x > x0 + 2 && x < x0 + 19 && z > z0 + 3 && z < z0 + 12) ? ((x == x0 + 5 && z == z0 + 6 && y == 11) ? TORCH : AIR) : (y < 40 ? STONE : AIR);
			case "cave_open_shaft": return (x, y, z) -> (y > 10 && y < 18 && x > x0 + 2 && x < x0 + 19) || (x == x0 + 8 && z == z0 + 8 && y >= 18) ? AIR : (y < 40 ? STONE : AIR);
			case "overhang_leaves": return (x, y, z) -> (y == 38 && x >= x0 + 4 && x <= x0 + 20 && z >= z0 + 4 && z <= z0 + 12) ? (((x + z) & 1) == 0 ? STONE : LEAVES) : base.at(x, y, z);
			case "water_pool": return (x, y, z) -> (y >= 24 && y < 30 && x >= x0 + 2 && x <= x0 + 18) ? WATER : (y < 24 ? SAND : base.at(x, y, z));
			case "glass_roof": return (x, y, z) -> (y == 36) ? GLASS : (x == x0 + 4 && z == z0 + 4 && y == 31 ? TORCH : base.at(x, y, z));
			case "leaves_canopy": return (x, y, z) -> (y >= 36 && y < 38) ? LEAVES : base.at(x, y, z);
			case "tunnel_cross_chunk": return (x, y, z) -> (z == z0 + 8 && y >= 12 && y <= 13 && x >= x0 - 20 && x <= x0 + 36) ? ((x == x0 + 34 && y == 12) ? TORCH : AIR) : (y < 40 ? STONE : AIR);
			case "lava_lake": return (x, y, z) -> (y >= 27 && y < 30 && x >= x0 + 10 && x <= x0 + 22 && z >= z0 + 2 && z <= z0 + 10) ? LAVA : base.at(x, y, z);
			case "multi_torch": return (x, y, z) -> ((x == x0 + 2 && z == z0 + 2 || x == x0 + 13 && z == z0 + 3 || x == x0 + 7 && z == z0 + 14 || x == x0 + 20 && z == z0 + 20) && y == 30) ? TORCH : base.at(x, y, z);
			case "hills":
			{
				final double a = r.nextDouble() * 6, b = r.nextDouble() * 6;
				return (x, y, z) -> { int h = 24 + (int) (8 * Math.sin(x / 7.0 + a) + 6 * Math.cos(z / 5.0 + b)); return y < h - 4 ? STONE : y < h - 1 ? DIRT : y < h ? GRASS : (y == h + 1 && ((x * 7 + z * 13) % 29 == 0) ? TORCH : AIR); };
			}
			case "tall_world": return (x, y, z) -> (x == x0 + 1 && z == z0 + 1 && y == 70) ? TORCH : (y < 64 ? (y < 0 ? 14 : STONE) : (y >= 100 && y < 102 && x < x0 + 8 ? LEAVES : AIR));
			case "snow_layer": return (x, y, z) -> (y == 30) ? SNOW : base.at(x, y, z);
			case "void": return (x, y, z) -> AIR;
			case "no_sky": return (x, y, z) -> (y < 10 || y > 50) ? STONE : ((x == x0 + 8 && z == z0 + 8 && y == 20) ? TORCH : AIR);
			case "half_sky": return (x, y, z) -> (x == x0 + 8 && z == z0 + 8 && y == 31) ? TORCH : base.at(x, y, z);
			case "stairs": return (x, y, z) -> (y >= 30 && y < 30 + ((x - x0 + 16) / 4) && x >= x0 - 16 && x < x0 + 32 && z >= z0 + 4 && z < z0 + 8) ? STAIRS : base.at(x, y, z);
			case "pillar_full_height": return (x, y, z) -> ((x == x0 + 5 && z == z0 + 5) || (x == x0 + 6 && z == z0 + 5 && y < 40)) ? STONE : base.at(x, y, z);
			case "missing_neighbours": return (x, y, z) -> (x == x0 - 1 && z == z0 + 5 && y == 30) ? TORCH : (x == x0 + 16 && z == z0 + 3 && y == 31 ? GLOW : base.at(x, y, z));
			default: throw new IllegalArgumentException(name);
		}
	}

	private static World randomWorld(Random r, int cx, int cz)
	{
		final int x0 = wx(cx, 0), z0 = wx(cz, 0);
		final int h = 20 + r.nextInt(25);
		final int[][] features = new int[2 + r.nextInt(6)][];
		for (int i = 0; i < features.length; i++)
		{
			features[i] = new int[] { x0 - 16 + r.nextInt(48), 8 + r.nextInt(50), z0 - 16 + r.nextInt(48), new int[] { TORCH, GLOW, LAVA, WATER, LEAVES, GLASS, STONE }[r.nextInt(7)], 1 + r.nextInt(3) };
		}
		final boolean cave = r.nextBoolean();
		final int cy = 6 + r.nextInt(10);
		return (x, y, z) ->
		{
			for (int[] f : features)
			{
				if (Math.abs(x - f[0]) < f[4] && Math.abs(z - f[2]) < f[4] && y >= f[1] && y < f[1] + f[4]) { return f[3]; }
			}
			if (cave && y >= cy && y < cy + 4 && ((x ^ z) & 8) == 0) { return AIR; }
			return y < h - 4 ? STONE : y < h - 1 ? DIRT : y < h ? GRASS : AIR;
		};
	}
}
