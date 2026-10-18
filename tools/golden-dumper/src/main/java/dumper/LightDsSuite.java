package dumper;

import com.seibel.distanthorizons.core.dataObjects.fullData.sources.FullDataSourceV2;
import com.seibel.distanthorizons.core.generation.DhLightingEngine;
import com.seibel.distanthorizons.core.pos.DhSectionPos;

import java.util.ArrayList;
import java.util.List;

/**
 * light_ds: DhLightingEngine.INSTANCE.bakeDataSourceSkyLight(source, maxSkyLight).
 * in : {"maxSkyLight":int, "source":<Ds json>}  (a contiguous populated block of columns, others empty)
 * out: {"cols":[[index,[longs]],...]}  the datapoints of every column listed in "in" (same order), after baking.
 */
public class LightDsSuite implements Suite
{
	@Override public String name() { return "light_ds"; }

	@Override
	public List<Obj> cases()
	{
		Gen g = new Gen(0x11647D5L);
		List<Obj> out = new ArrayList<>();
		for (int n = 0; n < 220; n++)
		{
			long pos = DhSectionPos.encode((byte) 6, g.pos28() >> 6, g.pos28() >> 6);
			Ds d = Ds.randomPalette(g, pos, 3 + g.r.nextInt(5));
			int w = 3 + g.r.nextInt(4), hgt = 3 + g.r.nextInt(4);
			int x0 = g.r.nextInt(Ds.W - w + 1), z0 = g.r.nextInt(Ds.W - hgt + 1);
			int top = 20 + g.r.nextInt(100);
			for (int x = x0; x < x0 + w; x++)
			{
				for (int z = z0; z < z0 + hgt; z++)
				{
					if (g.r.nextInt(15) == 0) { continue; }
					long[] pts = Ds.randomColumn(g, d.palette.size(), 4, top + g.r.nextInt(12) - 6, g.r.nextInt(3) == 0, d);
					d.addCol(Ds.idx(x, z), 9, 0, pts);
				}
			}
			int maxSky = g.r.nextInt(10) == 0 ? 1 + g.r.nextInt(14) : 15;
			FullDataSourceV2 s = d.build();
			DhLightingEngine.INSTANCE.bakeDataSourceSkyLight(s, maxSky);
			List<Object> cols = new ArrayList<>();
			for (Ds.Col c : d.cols) { cols.add(java.util.Arrays.asList(c.idx, s.dataPoints[c.idx].toLongArray())); }
			out.add(Obj.testCase("light_ds_" + n, new Obj().p("maxSkyLight", maxSky).p("source", d.json()), new Obj().p("cols", cols)));
		}
		return out;
	}
}
