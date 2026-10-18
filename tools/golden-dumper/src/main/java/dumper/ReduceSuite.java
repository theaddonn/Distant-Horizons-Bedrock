package dumper;

import com.seibel.distanthorizons.core.dataObjects.render.columnViews.ColumnRenderView;
import com.seibel.distanthorizons.core.util.RenderDataPointReducingList;
import com.seibel.distanthorizons.core.util.RenderDataPointUtil;
import it.unimi.dsi.fastutil.longs.LongArrayList;

import java.util.ArrayList;
import java.util.List;

/**
 * reduce: ColumnRenderView.changeVerticalSizeFrom(source, new RenderDataPointReducingList()) for one render column.
 * in : {"column":[longs]}  RenderDataPointUtil datapoints of one column, highest first (index 0 = top), possibly with gaps
 *      (gaps are implicit air), invisible points (alpha nibble 0) and translucent points. size n = 1..64.
 * out: {"t1":[longs],"t2":[..],"t3","t4","t6","t8","t16"}  the target column of exactly that many slots
 *      (ColumnRenderView with maxVerticalSliceCount = target, 1 sub view) or {"throws":..}.
 *      Slots beyond the reduced data are 0. Target >= n is a plain copy (zero padded).
 */
public class ReduceSuite implements Suite
{
	public static final int[] TARGETS = { 1, 2, 3, 4, 6, 8, 16 };

	@Override public String name() { return "reduce"; }

	@Override
	public List<Obj> cases()
	{
		Gen g = new Gen(0x4ED0CEL);
		List<Obj> out = new ArrayList<>();
		for (int n = 0; n < 480; n++)
		{
			int count;
			int k = g.r.nextInt(10);
			if (k < 3) { count = 1 + g.r.nextInt(6); }
			else if (k < 8) { count = 4 + g.r.nextInt(20); }
			else { count = 20 + g.r.nextInt(45); }
			long[] column = randomRenderColumn(g, count);

			Obj res = new Obj();
			for (int t : TARGETS) { res.t("t" + t, () -> reduce(column, t)); }
			out.add(Obj.testCase("reduce_" + n, new Obj().p("column", column), res));
		}
		return out;
	}

	private static long[] reduce(long[] column, int target)
	{
		LongArrayList src = new LongArrayList(column);
		LongArrayList srcVoid = new LongArrayList(new long[] { 0L });
		LongArrayList dst = new LongArrayList(new long[target]);
		LongArrayList dstVoid = new LongArrayList(new long[] { 0L });
		try (ColumnRenderView source = ColumnRenderView.getPooled(src, srcVoid, column.length, 0, 0, column.length);
			ColumnRenderView dest = ColumnRenderView.getPooled(dst, dstVoid, target, 0, 0, target))
		{
			dest.changeVerticalSizeFrom(source, new RenderDataPointReducingList());
		}
		return dst.toLongArray();
	}

	private static long[] randomRenderColumn(Gen g, int count)
	{
		// build bottom-up then reverse so index 0 is the highest
		List<Long> pts = new ArrayList<>();
		int y = g.r.nextInt(2000);
		for (int i = 0; i < count; i++)
		{
			if (g.r.nextInt(10) < 3) { y += 1 + g.r.nextInt(10); } // gap
			int h = g.r.nextInt(3) == 0 ? 1 + g.r.nextInt(30) : 1 + g.r.nextInt(4);
			int alphaKind = g.r.nextInt(20);
			int alpha = alphaKind < 15 ? 255 : alphaKind < 18 ? 16 + g.r.nextInt(220) : g.r.nextInt(16);
			// neighbouring segments often share a colour so merging by alpha is exercised
			int base = (i % 3 == 0) ? g.r.nextInt(256) : 100;
			long p = RenderDataPointUtil.createDataPoint(alpha, base, g.r.nextInt(256), g.r.nextInt(256), y + h, y, g.r.nextInt(16), g.r.nextInt(16), g.r.nextInt(16));
			pts.add(p);
			y += h;
		}
		long[] out = new long[pts.size()];
		for (int i = 0; i < out.length; i++) { out[i] = pts.get(out.length - 1 - i); }
		return out;
	}
}
