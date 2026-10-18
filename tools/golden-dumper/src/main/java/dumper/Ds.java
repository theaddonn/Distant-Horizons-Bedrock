package dumper;

import com.seibel.distanthorizons.core.dataObjects.fullData.FullDataPointIdMap;
import com.seibel.distanthorizons.core.dataObjects.fullData.sources.FullDataSourceV2;
import com.seibel.distanthorizons.core.util.FullDataPointUtil;
import it.unimi.dsi.fastutil.longs.LongArrayList;

import java.util.ArrayList;
import java.util.List;

/**
 * Compact, JSON-able description of a FullDataSourceV2 plus helpers to build it / dump it.
 * JSON form: {"pos":long,"palette":[{"block":serial,"biome":serial},...],"cols":[[index,genStep,compMode,[longs...]],...]}
 * "cols" is sparse, ascending index (index = relX*64 + relZ). Unlisted columns have no datapoints, genStep = "defGen"
 * and compMode = "defComp" (both default to 0 when the keys are absent, as in every input). Listed columns carry their own values.
 */
public final class Ds
{
	public static final int W = FullDataSourceV2.WIDTH;

	public static final class Col
	{
		public final int idx;
		public final int gen, comp;
		public final long[] pts;
		public Col(int idx, int gen, int comp, long[] pts) { this.idx = idx; this.gen = gen; this.comp = comp; this.pts = pts; }
	}

	public boolean emptyFlag;
	/** optional uniform base column applied to every column that is not listed in cols ("fill" in the JSON) */
	public long[] fillPts;
	public int fillGen, fillComp;
	public long pos;
	public List<int[]> palette = new ArrayList<>(); // {block, biome}
	public List<Col> cols = new ArrayList<>();

	public Ds(long pos) { this.pos = pos; }

	public Ds addPalette(int block, int biome) { this.palette.add(new int[] { block, biome }); return this; }
	public Ds addCol(int idx, int gen, int comp, long... pts) { this.cols.add(new Col(idx, gen, comp, pts)); return this; }

	public boolean isAirId(int id) { return Wrappers.block(this.palette.get(id)[0]).isAir; }

	/** Builds the real data source. */
	public FullDataSourceV2 build()
	{
		FullDataPointIdMap map = new FullDataPointIdMap(this.pos);
		for (int[] p : this.palette) { map.addIfNotPresentAndGetId(Wrappers.biome(p[1]), Wrappers.block(p[0])); }
		LongArrayList[] data = new LongArrayList[W * W];
		byte[] gen = new byte[W * W];
		byte[] comp = new byte[W * W];
		for (int i = 0; i < data.length; i++) { data[i] = new LongArrayList(); }
		if (this.fillPts != null)
		{
			for (int i = 0; i < data.length; i++) { data[i].addAll(new LongArrayList(this.fillPts)); gen[i] = (byte) this.fillGen; comp[i] = (byte) this.fillComp; }
		}
		for (Col c : this.cols)
		{
			data[c.idx].clear();
			data[c.idx].addAll(new LongArrayList(c.pts));
			gen[c.idx] = (byte) c.gen;
			comp[c.idx] = (byte) c.comp;
		}
		return FullDataSourceV2.createWithData(this.pos, map, data, gen, comp);
	}

	public Obj json()
	{
		List<Object> pal = new ArrayList<>();
		for (int[] p : this.palette) { pal.add(new Obj().p("block", p[0]).p("biome", p[1])); }
		List<Object> cs = new ArrayList<>();
		for (Col c : this.cols) { cs.add(colJson(c.idx, c.gen, c.comp, c.pts)); }
		Obj o = new Obj().p("pos", this.pos).p("palette", pal);
		if (this.fillPts != null) { o.p("fill", new Obj().p("gen", this.fillGen).p("comp", this.fillComp).p("pts", this.fillPts)); }
		return o.p("cols", cs);
	}

	private static List<Object> colJson(int idx, int gen, int comp, long[] pts)
	{
		List<Object> l = new ArrayList<>(4);
		l.add(idx); l.add(gen); l.add(comp); l.add(pts);
		return l;
	}

	/** Dumps the full state of a real data source (palette after any remap, sparse columns). */
	public static Obj jsonOf(FullDataSourceV2 s)
	{
		List<Object> pal = new ArrayList<>();
		for (int id = 0; id < s.mapping.size(); id++)
		{
			pal.add(new Obj()
					.p("block", ((Wrappers.DumpBlockState) s.mapping.getBlockStateWrapper(id)).serial)
					.p("biome", ((Wrappers.DumpBiome) s.mapping.getBiomeWrapper(id)).serial));
		}
		// default (gen, comp) = most common pair among columns without datapoints; only deviating columns are listed
		java.util.Map<Integer, Integer> freq = new java.util.HashMap<>();
		for (int i = 0; i < W * W; i++)
		{
			LongArrayList l = s.dataPoints[i];
			if (l == null || l.isEmpty())
			{
				freq.merge(((int) s.columnGenerationSteps.getByte(i) << 8) | (s.columnWorldCompressionMode.getByte(i) & 0xFF), 1, Integer::sum);
			}
		}
		int best = 0, bestCount = -1;
		for (java.util.Map.Entry<Integer, Integer> e : freq.entrySet())
		{
			if (e.getValue() > bestCount || (e.getValue() == bestCount && e.getKey() < best)) { best = e.getKey(); bestCount = e.getValue(); }
		}
		int defGen = (byte) (best >> 8), defComp = best & 0xFF;
		List<Object> cs = new ArrayList<>();
		for (int i = 0; i < W * W; i++)
		{
			LongArrayList l = s.dataPoints[i];
			int gen = s.columnGenerationSteps.getByte(i);
			int comp = s.columnWorldCompressionMode.getByte(i);
			boolean hasPts = l != null && !l.isEmpty();
			if (hasPts || gen != defGen || comp != defComp)
			{
				cs.add(colJson(i, gen, comp, l == null ? new long[0] : l.toLongArray()));
			}
		}
		return new Obj().p("pos", s.getPos()).p("palette", pal).p("defGen", defGen).p("defComp", defComp).p("cols", cs).p("isEmpty", s.isEmpty);
	}
	
	/**
	 * Dense-friendly dump: the most common (genStep, compMode, datapoints) column becomes "fill", only deviating columns are listed.
	 * {"pos","palette","fill":{"gen","comp","pts"},"cols":[[index,gen,comp,[longs]],...],"isEmpty"}
	 */
	public static Obj jsonOfDense(FullDataSourceV2 s)
	{
		List<Object> pal = new ArrayList<>();
		for (int id = 0; id < s.mapping.size(); id++)
		{
			pal.add(new Obj().p("block", ((Wrappers.DumpBlockState) s.mapping.getBlockStateWrapper(id)).serial)
					.p("biome", ((Wrappers.DumpBiome) s.mapping.getBiomeWrapper(id)).serial));
		}
		java.util.Map<String, Integer> freq = new java.util.LinkedHashMap<>();
		String[] keys = new String[W * W];
		for (int i = 0; i < W * W; i++)
		{
			keys[i] = s.columnGenerationSteps.getByte(i) + "/" + s.columnWorldCompressionMode.getByte(i) + "/" + java.util.Arrays.toString(s.dataPoints[i].toLongArray());
			freq.merge(keys[i], 1, Integer::sum);
		}
		String best = null;
		int bestCount = -1;
		for (java.util.Map.Entry<String, Integer> e : freq.entrySet()) { if (e.getValue() > bestCount) { best = e.getKey(); bestCount = e.getValue(); } }
		int fillIdx = java.util.Arrays.asList(keys).indexOf(best);
		Obj fill = new Obj().p("gen", (int) s.columnGenerationSteps.getByte(fillIdx)).p("comp", (int) s.columnWorldCompressionMode.getByte(fillIdx)).p("pts", s.dataPoints[fillIdx].toLongArray());
		List<Object> cs = new ArrayList<>();
		for (int i = 0; i < W * W; i++)
		{
			if (!keys[i].equals(best)) { cs.add(colJson(i, s.columnGenerationSteps.getByte(i), s.columnWorldCompressionMode.getByte(i), s.dataPoints[i].toLongArray())); }
		}
		return new Obj().p("pos", s.getPos()).p("palette", pal).p("fill", fill).p("cols", cs).p("isEmpty", s.isEmpty);
	}

	/**
	 * Run-length dump over all 4096 columns in index order (relX*64+relZ):
	 * {"pos","palette","runs":[[count,genStep,compMode,[longs]],...],"isEmpty"}; consecutive identical columns are merged into one run.
	 */
	public static Obj jsonOfRuns(FullDataSourceV2 s)
	{
		List<Object> pal = new ArrayList<>();
		for (int id = 0; id < s.mapping.size(); id++)
		{
			pal.add(new Obj().p("block", ((Wrappers.DumpBlockState) s.mapping.getBlockStateWrapper(id)).serial)
					.p("biome", ((Wrappers.DumpBiome) s.mapping.getBiomeWrapper(id)).serial));
		}
		List<Object> runs = new ArrayList<>();
		int i = 0;
		while (i < W * W)
		{
			long[] a = s.dataPoints[i].toLongArray();
			int g = s.columnGenerationSteps.getByte(i), c = s.columnWorldCompressionMode.getByte(i);
			int j = i + 1;
			while (j < W * W && s.columnGenerationSteps.getByte(j) == g && s.columnWorldCompressionMode.getByte(j) == c && java.util.Arrays.equals(s.dataPoints[j].toLongArray(), a)) { j++; }
			List<Object> r = new ArrayList<>(4);
			r.add(j - i); r.add(g); r.add(c); r.add(a);
			runs.add(r);
			i = j;
		}
		return new Obj().p("pos", s.getPos()).p("palette", pal).p("runs", runs).p("isEmpty", s.isEmpty);
	}

	// ---------------- random generation ----------------

	/** Random distinct palette of {@code n} entries; entry 0 is always air or cave_air. */
	public static Ds randomPalette(Gen g, long pos, int n)
	{
		Ds d = new Ds(pos);
		d.addPalette(g.r.nextInt(5) == 0 ? 15 : 0, g.r.nextInt(Wrappers.BIOME_COUNT));
		int guard = 0;
		while (d.palette.size() < n && guard++ < 100)
		{
			int b = g.r.nextInt(Wrappers.CLASSIC_BLOCK_COUNT);
			int m = g.r.nextInt(Wrappers.BIOME_COUNT);
			boolean dup = false;
			for (int[] p : d.palette) { if (p[0] == b && p[1] == m) { dup = true; } }
			if (!dup) { d.addPalette(b, m); }
		}
		return d;
	}

	/**
	 * Random top-down column of up to maxPts datapoints (index 0 = highest), optionally with gaps.
	 * ids are drawn from [0,paletteSize).
	 */
	public static long[] randomColumn(Gen g, int paletteSize, int maxPts, int topY, boolean allowGaps, Ds air)
	{
		int n = maxPts <= 0 ? 0 : g.r.nextInt(maxPts + 1);
		List<Long> pts = new ArrayList<>();
		int top = topY;
		for (int i = 0; i < n && top > 0; i++)
		{
			int h = g.r.nextInt(4) == 0 ? 1 + g.r.nextInt(60) : 1 + g.r.nextInt(8);
			h = Math.min(h, top);
			int bottom = top - h;
			int id = g.r.nextInt(paletteSize);
			boolean isAir = air != null && air.isAirId(id);
			int block = isAir ? (g.r.nextInt(3) == 0 ? 0 : 15) : g.r.nextInt(16);
			int sky = isAir && g.r.nextInt(3) != 0 ? 15 : g.r.nextInt(16);
			try
			{
				pts.add(FullDataPointUtil.encode(id, h, bottom, (byte) (isAir ? 0 : block), (byte) sky));
			}
			catch (Exception e) { throw new RuntimeException(e); }
			top = bottom;
			if (allowGaps && g.r.nextInt(8) == 0) { top = Math.max(0, top - 1 - g.r.nextInt(6)); }
		}
		long[] out = new long[pts.size()];
		for (int i = 0; i < out.length; i++) { out[i] = pts.get(i); }
		return out;
	}

	public static int idx(int x, int z) { return x * W + z; }
}
