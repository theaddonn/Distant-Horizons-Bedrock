package dumper;

import com.seibel.distanthorizons.core.dataObjects.fullData.sources.FullDataSourceV2;
import com.seibel.distanthorizons.core.dataObjects.transformers.FullDataOcclusionCuller;
import com.seibel.distanthorizons.core.pos.DhSectionPos;
import com.seibel.distanthorizons.core.util.FullDataPointUtil;

import java.util.ArrayList;
import java.util.List;

/**
 * cull: FullDataOcclusionCuller.cullHiddenDatapointsInColumn.
 * in : {"source":<Ds json>}  a data source with a small populated region (plus shape or 3x3, other columns empty).
 * The real call sequence is applied: for every column index ascending (x-major) with more than 1 datapoint,
 * cullHiddenDatapointsInColumn(source, x, z).
 * out: {"changed":[[index,[longs]],...]}  the columns whose datapoint list differs after culling (ascending index);
 *      every other column is unchanged from "in".
 */
public class CullSuite implements Suite
{
	@Override public String name() { return "cull"; }

	@Override
	public List<Obj> cases()
	{
		Gen g = new Gen(0xC0111L);
		List<Obj> out = new ArrayList<>();
		for (int n = 0; n < 1000; n++)
		{
			long pos = DhSectionPos.encode((byte) 6, g.pos28() >> 6, g.pos28() >> 6);
			Ds d = Ds.randomPalette(g, pos, 4 + g.r.nextInt(5));
			int cx = 1 + g.r.nextInt(62), cz = 1 + g.r.nextInt(62);
			boolean full = g.r.nextInt(10) < 3;
			int[][] cells = full
					? new int[][] { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } }
					: new int[][] { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
			// shared layer structure with per-column jitter
			int top = 40 + g.r.nextInt(80);
			int layerCount = 2 + g.r.nextInt(5);
			int[] layerId = new int[layerCount];
			int[] layerH = new int[layerCount];
			for (int i = 0; i < layerCount; i++)
			{
				layerId[i] = pickId(g, d, 80);
				layerH[i] = 1 + g.r.nextInt(i == layerCount - 1 ? 30 : 12);
			}
			for (int[] c : cells)
			{
				if (g.r.nextInt(12) == 0 && (c[0] != 0 || c[1] != 0)) { continue; } // a missing neighbour (should prevent culling)
				List<Long> pts = new ArrayList<>();
				int y = top + (g.r.nextInt(10) == 0 ? g.r.nextInt(5) - 2 : 0);
				for (int i = 0; i < layerCount && y > 0; i++)
				{
					int h = layerH[i] + (g.r.nextInt(8) == 0 ? g.r.nextInt(5) - 2 : 0);
					h = Math.max(1, Math.min(h, y));
					int id = (g.r.nextInt(25) == 0) ? g.r.nextInt(d.palette.size()) : layerId[i];
					boolean air = d.isAirId(id);
					try { pts.add(FullDataPointUtil.encode(id, h, y - h, (byte) (air ? 0 : g.r.nextInt(16)), (byte) g.r.nextInt(16))); }
					catch (Exception e) { throw new RuntimeException(e); }
					y -= h;
				}
				long[] arr = new long[pts.size()];
				for (int i = 0; i < arr.length; i++) { arr[i] = pts.get(i); }
				d.addCol(Ds.idx(cx + c[0], cz + c[1]), 9, 1, arr);
			}
			d.cols.sort((a, b) -> Integer.compare(a.idx, b.idx));

			FullDataSourceV2 s = d.build();
			long[][] before = new long[Ds.W * Ds.W][];
			for (int i = 0; i < before.length; i++) { before[i] = s.dataPoints[i].toLongArray(); }
			for (int x = 0; x < Ds.W; x++)
			{
				for (int z = 0; z < Ds.W; z++)
				{
					if (s.getColumnAtRelPos(x, z).size() > 1) { FullDataOcclusionCuller.cullHiddenDatapointsInColumn(s, x, z); }
				}
			}
			List<Object> changed = new ArrayList<>();
			for (int i = 0; i < before.length; i++)
			{
				long[] after = s.dataPoints[i].toLongArray();
				if (!java.util.Arrays.equals(before[i], after)) { changed.add(java.util.Arrays.asList(i, after)); }
			}
			out.add(Obj.testCase("cull_" + n, new Obj().p("source", d.json()), new Obj().p("changed", changed)));
		}
		return out;
	}
	
	/** with probability pctOpaque% picks an opaque (opacity 16) palette id, otherwise any id */
	private static int pickId(Gen g, Ds d, int pctOpaque)
	{
		if (g.r.nextInt(100) < pctOpaque)
		{
			List<Integer> ids = new ArrayList<>();
			for (int i = 0; i < d.palette.size(); i++) { if (Wrappers.block(d.palette.get(i)[0]).opacity == 16) { ids.add(i); } }
			if (!ids.isEmpty()) { return ids.get(g.r.nextInt(ids.size())); }
		}
		return g.r.nextInt(d.palette.size());
	}
}
