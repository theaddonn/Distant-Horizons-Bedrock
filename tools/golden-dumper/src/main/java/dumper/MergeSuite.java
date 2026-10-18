package dumper;

import com.seibel.distanthorizons.api.enums.config.EDhApiWorldCompressionMode;
import com.seibel.distanthorizons.core.config.Config;
import com.seibel.distanthorizons.core.dataObjects.fullData.sources.FullDataSourceV2;
import com.seibel.distanthorizons.core.pos.DhSectionPos;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * merge_same / merge_up / downsample: FullDataSourceV2.updateFromDataSource at equal / one-lower / one-higher input detail.
 * in : {"worldCompression":0|1 (Config.Common.LodBuilding.worldCompression ordinal value), "target":<Ds>, "inputs":[<Ds>...]}
 *      (inputs applied in order; merge_same / downsample have exactly 1 input, merge_up has 4 children in index order 0..3)
 * out: {"dataChanged":[bool per input call], "target":<Ds dump after all calls, incl. isEmpty>}
 */
public class MergeSuite implements Suite
{
	public enum Mode { SAME, UP, DOWN }

	private final Mode mode;
	private final int count;

	public MergeSuite(Mode mode, int count) { this.mode = mode; this.count = count; }

	@Override public String name() { return this.mode == Mode.SAME ? "merge_same" : this.mode == Mode.UP ? "merge_up" : "downsample"; }

	@Override
	public List<Obj> cases()
	{
		Gen g = new Gen(0x4D5247L + this.mode.ordinal());
		List<Obj> out = new ArrayList<>();
		for (int i = 0; i < this.count; i++) { out.add(makeCase(this.name() + "_" + i, g)); }
		return out;
	}

	private static int randGen(Gen g)
	{
		int k = g.r.nextInt(10);
		if (k == 0) { return -1; }
		if (k == 1) { return 0; }
		if (k < 5) { return 9; }
		return 1 + g.r.nextInt(9);
	}

	/** palette of 2..5 entries drawn from the shared universe (entry 0 of the universe is air). */
	private static Ds subsetPalette(Gen g, Ds universe, long pos)
	{
		List<int[]> pool = new ArrayList<>(universe.palette);
		Collections.shuffle(pool, g.r);
		int n = 2 + g.r.nextInt(4);
		Ds d = new Ds(pos);
		boolean haveAir = false;
		for (int i = 0; i < Math.min(n, pool.size()); i++)
		{
			d.palette.add(pool.get(i));
			if (Wrappers.block(pool.get(i)[0]).isAir) { haveAir = true; }
		}
		if (!haveAir) { d.palette.set(g.r.nextInt(d.palette.size()), universe.palette.get(0)); }
		return d;
	}

	private Obj makeCase(String name, Gen g)
	{
		int d = 6 + g.r.nextInt(3);
		final boolean cull = g.r.nextInt(3) != 0;
		Ds universe = Ds.randomPalette(g, 0, 7);
		int[] pool = new int[10];
		for (int i = 0; i < pool.length; i++) { pool[i] = g.r.nextInt(Ds.W * Ds.W); }

		Ds target;
		List<Ds> inputs = new ArrayList<>();

		if (this.mode == Mode.SAME)
		{
			long pos = DhSectionPos.encode((byte) d, g.pos28() >> 6, g.pos28() >> 6);
			target = fill(g, subsetPalette(g, universe, pos), pool, 1 + g.r.nextInt(4), 6, true);
			inputs.add(fill(g, subsetPalette(g, universe, pos), pool, 1 + g.r.nextInt(4), 7, true));
			if (g.r.nextInt(25) == 0) { target = new Ds(pos); } // empty target
			if (g.r.nextInt(40) == 0) { inputs.set(0, new Ds(pos)); } // empty input mapping -> early return
		}
		else if (this.mode == Mode.UP)
		{
			long parent = DhSectionPos.encode((byte) (d + 1), g.pos28() >> 6, g.pos28() >> 6);
			target = g.r.nextBoolean() ? new Ds(parent) : fill(g, subsetPalette(g, universe, parent), pool, 1 + g.r.nextInt(3), 4, true);
			int[] anchors = new int[3];
			for (int i = 0; i < anchors.length; i++) { anchors[i] = (g.r.nextInt(32) * 2) * Ds.W + g.r.nextInt(32) * 2; }
			for (int c = 0; c < 4; c++)
			{
				long cp = DhSectionPos.getChildByIndex(parent, c);
				Ds child = subsetPalette(g, universe, cp);
				int na = g.r.nextInt(3);
				for (int a = 0; a < na; a++) { addTwoByTwo(g, child, anchors[g.r.nextInt(anchors.length)]); }
				inputs.add(child);
			}
		}
		else
		{
			long parent = DhSectionPos.encode((byte) (d + 1), g.pos28() >> 6, g.pos28() >> 6);
			int c = g.r.nextInt(4);
			long childPos = DhSectionPos.getChildByIndex(parent, c);
			target = g.r.nextInt(3) == 0 ? new Ds(childPos) : fill(g, subsetPalette(g, universe, childPos), pool, g.r.nextInt(4), 5, true);
			Ds in = subsetPalette(g, universe, parent);
			// parent columns inside the quadrant that maps onto the child (x offset / z offset 0 or 32), plus a few outside
			int offX = (DhSectionPos.getX(childPos) == DhSectionPos.getX(DhSectionPos.getChildByIndex(parent, 0))) ? 0 : 32;
			int offZ = (DhSectionPos.getZ(childPos) == DhSectionPos.getZ(DhSectionPos.getChildByIndex(parent, 0))) ? 0 : 32;
			// the child reads parent column (x/2+offX, z/2+offZ): choose child columns, derive the parent columns they read
			List<Integer> used = new ArrayList<>();
			for (int i = 0; i < 4; i++)
			{
				int px = offX + g.r.nextInt(32), pz = offZ + g.r.nextInt(32);
				int pi = Ds.idx(px, pz);
				if (used.contains(pi)) { continue; }
				used.add(pi);
				in.addCol(pi, randGen(g), g.r.nextInt(2), Ds.randomColumn(g, in.palette.size(), 7, 8 + g.r.nextInt(300), true, in));
				// make sure some child columns that read this parent column exist (some with data -> must not be overwritten)
				if (g.r.nextInt(3) == 0 && !target.palette.isEmpty())
				{
					int cx = (px - offX) * 2 + g.r.nextInt(2), cz = (pz - offZ) * 2 + g.r.nextInt(2);
					addColIfFree(target, Ds.idx(cx, cz), randGen(g), g.r.nextInt(2), Ds.randomColumn(g, target.palette.size(), 4, 8 + g.r.nextInt(100), true, target));
				}
			}
			in.cols.sort((a, b) -> Integer.compare(a.idx, b.idx));
			inputs.add(in);
		}
		target.cols.sort((a, b) -> Integer.compare(a.idx, b.idx));
		for (Ds in : inputs) { in.cols.sort((a, b) -> Integer.compare(a.idx, b.idx)); }

		// ---- run the real code ----
		Config.Common.LodBuilding.worldCompression.set(cull ? EDhApiWorldCompressionMode.VISUALLY_EQUAL : EDhApiWorldCompressionMode.MERGE_SAME_BLOCKS);
		Obj in = new Obj().p("worldCompression", cull ? 1 : 0).p("target", target.json());
		List<Object> inJson = new ArrayList<>();
		for (Ds i : inputs) { inJson.add(i.json()); }
		in.p("inputs", inJson);

		FullDataSourceV2 t = target.build();
		List<Object> changed = new ArrayList<>();
		for (Ds i : inputs)
		{
			FullDataSourceV2 s = i.build();
			changed.add(t.updateFromDataSource(s));
		}
		Obj out = new Obj().p("dataChanged", changed).p("target", Ds.jsonOf(t));
		return Obj.testCase(name, in, out);
	}

	private static void addColIfFree(Ds d, int idx, int gen, int comp, long[] pts)
	{
		for (Ds.Col c : d.cols) { if (c.idx == idx) { return; } }
		d.addCol(idx, gen, comp, pts);
	}

	/** fills `n` columns taken from `pool` (so separate sources overlap) */
	private static Ds fill(Gen g, Ds d, int[] pool, int n, int maxPts, boolean gaps)
	{
		List<Integer> used = new ArrayList<>();
		for (int i = 0; i < n; i++)
		{
			int idx = pool[g.r.nextInt(pool.length)];
			if (used.contains(idx)) { continue; }
			used.add(idx);
			d.addCol(idx, randGen(g), g.r.nextInt(2), Ds.randomColumn(g, d.palette.size(), maxPts, 8 + g.r.nextInt(g.r.nextInt(4) == 0 ? 3000 : 200), gaps, d));
		}
		return d;
	}

	/** populates (a subset of) the 2x2 block of columns starting at anchor index (even x, even z) */
	private static void addTwoByTwo(Gen g, Ds child, int anchor)
	{
		int ax = anchor / Ds.W, az = anchor % Ds.W;
		int variant = g.r.nextInt(10);
		long[] base = Ds.randomColumn(g, child.palette.size(), 4, 8 + g.r.nextInt(200), variant != 0, child);
		boolean allAir = variant == 1;
		for (int dx = 0; dx < 2; dx++)
		{
			for (int dz = 0; dz < 2; dz++)
			{
				if (g.r.nextInt(10) < 2 && variant > 3) { continue; } // sparse: a missing sub-column
				int idx = Ds.idx(ax + dx, az + dz);
				boolean dup = false;
				for (Ds.Col c : child.cols) { if (c.idx == idx) { dup = true; } }
				if (dup) { continue; }
				long[] pts;
				if (variant <= 3)
				{
					// same ids/heights in all four sub-columns, only the light differs (variant 2,3) or all-air (variant 1)
					pts = base.clone();
					for (int i = 0; i < pts.length; i++)
					{
						long p = pts[i];
						if (allAir) { p = com.seibel.distanthorizons.core.util.FullDataPointUtil.setId(p, airId(child)); }
						p = com.seibel.distanthorizons.core.util.FullDataPointUtil.setSkyLight(p, g.r.nextInt(16));
						if (!child.isAirId(com.seibel.distanthorizons.core.util.FullDataPointUtil.getId(p)))
						{
							p = com.seibel.distanthorizons.core.util.FullDataPointUtil.setBlockLight(p, (byte) g.r.nextInt(16));
						}
						pts[i] = p;
					}
				}
				else
				{
					pts = Ds.randomColumn(g, child.palette.size(), 3, 8 + g.r.nextInt(200), true, child);
				}
				child.addCol(idx, randGen(g), g.r.nextInt(2), pts);
			}
		}
	}

	private static int airId(Ds d)
	{
		for (int i = 0; i < d.palette.size(); i++) { if (d.isAirId(i)) { return i; } }
		return 0;
	}
}
