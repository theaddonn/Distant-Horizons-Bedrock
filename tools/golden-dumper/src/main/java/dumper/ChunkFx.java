package dumper;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

/**
 * A chunk fixture: per-column block serials (index x*16+z, then yRel) plus 4x4x4 biome cells.
 * JSON form (complete input for rebuilding a DumpChunk):
 * {"chunkX","chunkZ","minY","maxY",
 *  "profiles":[ [[blockSerial,count],...] ... ]      run-length block column profiles, bottom (minY) to top (maxY-1)
 *  "colRuns":[[profileIndex,count],...]               run-length over the 256 columns in order index = x*16 + z
 *  "biomeRuns":[ [[biome,count],...] x16 ]            per quart column (qx*4+qz), runs over qy = (y-minY)>>2 bottom to top }
 */
public final class ChunkFx
{
	public final int chunkX, chunkZ, minY, maxY;
	public final int h;
	/** [x*16+z][yRel] */
	public final int[][] col;
	/** [qx*4+qz][qy] */
	public final int[][] biome;

	public ChunkFx(int chunkX, int chunkZ, int minY, int maxY)
	{
		this.chunkX = chunkX; this.chunkZ = chunkZ; this.minY = minY; this.maxY = maxY;
		this.h = maxY - minY;
		this.col = new int[256][this.h];
		this.biome = new int[16][this.h >> 2];
	}

	public void fill(int x, int z, int yFrom, int yTo, int block)
	{
		for (int y = Math.max(yFrom, this.minY); y < Math.min(yTo, this.maxY); y++) { this.col[x * 16 + z][y - this.minY] = block; }
	}

	public void set(int x, int y, int z, int block)
	{
		if (y >= this.minY && y < this.maxY) { this.col[x * 16 + z][y - this.minY] = block; }
	}

	public int get(int x, int y, int z) { return (y < this.minY || y >= this.maxY) ? 0 : this.col[x * 16 + z][y - this.minY]; }

	public void fillBiome(int biome) { for (int[] c : this.biome) { java.util.Arrays.fill(c, biome); } }

	public DumpChunk toChunk()
	{
		DumpChunk c = new DumpChunk(this.chunkX, this.chunkZ, this.minY, this.maxY);
		for (int x = 0; x < 16; x++)
		{
			for (int z = 0; z < 16; z++)
			{
				int[] column = this.col[x * 16 + z];
				for (int yr = 0; yr < this.h; yr++) { c.blocks[c.relativeBlockPosToIndex(x, yr + this.minY, z)] = column[yr]; }
			}
		}
		for (int i = 0; i < 16; i++) { System.arraycopy(this.biome[i], 0, c.biomes[i], 0, c.biomes[i].length); }
		return c;
	}

	private static List<Object> runs(int[] a)
	{
		List<Object> out = new ArrayList<>();
		int i = 0;
		while (i < a.length)
		{
			int j = i;
			while (j < a.length && a[j] == a[i]) { j++; }
			List<Object> r = new ArrayList<>(2);
			r.add(a[i]); r.add(j - i);
			out.add(r);
			i = j;
		}
		return out;
	}

	public Obj json()
	{
		List<Object> profiles = new ArrayList<>();
		List<String> keys = new ArrayList<>();
		int[] profileOfCol = new int[256];
		for (int i = 0; i < 256; i++)
		{
			String k = java.util.Arrays.toString(this.col[i]);
			int idx = keys.indexOf(k);
			if (idx < 0) { keys.add(k); profiles.add(runs(this.col[i])); idx = keys.size() - 1; }
			profileOfCol[i] = idx;
		}
		List<Object> biomeRuns = new ArrayList<>();
		for (int i = 0; i < 16; i++) { biomeRuns.add(runs(this.biome[i])); }
		return new Obj().p("chunkX", this.chunkX).p("chunkZ", this.chunkZ).p("minY", this.minY).p("maxY", this.maxY)
				.p("profiles", profiles).p("colRuns", runs(profileOfCol)).p("biomeRuns", biomeRuns);
	}

	// ---------------------------------------------------------------- terrain helpers

	public static final int AIR = 0, STONE = 1, DIRT = 2, GRASS = 3, WATER = 4, ICE = 5, GLASS = 6, LEAVES = 7, TORCH = 8,
			SNOW = 9, POPPY = 10, GLOWSTONE = 11, LAVA = 12, STAIRS = 13, DEEPSLATE = 14, CAVE_AIR = 15, SAND = 16;

	/** stone / dirt / grass terrain with the given surface height (first air Y) per column; deepslate below Y 0. */
	public static void terrain(ChunkFx f, int[][] surface)
	{
		for (int x = 0; x < 16; x++)
		{
			for (int z = 0; z < 16; z++)
			{
				int s = surface[x][z];
				f.fill(x, z, f.minY, Math.min(0, s - 4), DEEPSLATE);
				f.fill(x, z, Math.min(0, s - 4), s - 4, STONE);
				f.fill(x, z, s - 4, s - 1, DIRT);
				f.fill(x, z, s - 1, s, GRASS);
			}
		}
	}

	public static int[][] flat(int h)
	{
		int[][] s = new int[16][16];
		for (int[] r : s) { java.util.Arrays.fill(r, h); }
		return s;
	}

	public static int[][] hills(Random r, int base, int amp)
	{
		// bilinear value noise on a 5x5 lattice
		double[][] lat = new double[5][5];
		for (double[] row : lat) { for (int i = 0; i < 5; i++) { row[i] = r.nextDouble(); } }
		int[][] s = new int[16][16];
		for (int x = 0; x < 16; x++)
		{
			for (int z = 0; z < 16; z++)
			{
				double fx = x / 4.0, fz = z / 4.0;
				int ix = (int) fx, iz = (int) fz;
				double tx = fx - ix, tz = fz - iz;
				double v = lat[ix][iz] * (1 - tx) * (1 - tz) + lat[ix + 1][iz] * tx * (1 - tz) + lat[ix][iz + 1] * (1 - tx) * tz + lat[ix + 1][iz + 1] * tx * tz;
				s[x][z] = base + (int) (v * amp);
			}
		}
		return s;
	}
}
