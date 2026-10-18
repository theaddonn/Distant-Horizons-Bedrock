package dumper;

import com.seibel.distanthorizons.api.enums.config.EDhApiGrassSideRendering;
import com.seibel.distanthorizons.api.enums.rendering.EDhApiDebugRendering;
import com.seibel.distanthorizons.core.config.Config;
import com.seibel.distanthorizons.core.dataObjects.render.bufferBuilding.LodQuadBuilder;
import com.seibel.distanthorizons.core.enums.EDhDirection;
import com.seibel.distanthorizons.core.util.objects.pooling.PhantomArrayList.PhantomArrayListCheckout;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * quads: LodQuadBuilder. One (doTransparency, grassSideRendering) combination per case (cycled).
 * in : {"doTransparency":bool,"grassSideRendering":0|1|2 (AS_GRASS,FADE_TO_DIRT,AS_DIRT),"debugRendering":0..3 (EDhApiDebugRendering ordinal),
 *       "earthCurveRatio":0|100,"showOverlappingQuadErrors":bool,
 *       "calls":[ ["adj",dirOrdinal,x,y,z,width,height,color,faceTexId,materialId,skyLight,blockLight],
 *                 ["up",x,y,z,blockWidth,color,faceTexId,materialId,skyLight,blockLight]   (x=minX,y=maxY,z=minZ),
 *                 ["down",x,y,z,blockWidth,color,faceTexId,materialId,skyLight,blockLight] ] }
 *       dirOrdinal = EDhDirection ordinal (0 DOWN,1 UP,2 NORTH,3 SOUTH,4 WEST,5 EAST). color is a signed ARGB int.
 *       The client level is DumpClientLevel (dirt colour hashColor(2,0), vanilla shades).
 * out: {"opaqueQuads":n,"transparentQuads":n,"opaque":[hex per ByteBuffer],"transparent":[hex per ByteBuffer]}
 *       counts are after mergeQuads(); buffers are produced by makeOpaque/TransparentVertexBuffers (16 bytes per vertex,
 *       4 vertices per quad, little-endian = native order on the host: i16 x,y,z; u16 meta; u8 r,g,b,a; u8 material; u8 normalIndex; u16 textureTileId).
 */
public class QuadSuite implements Suite
{
	private static final com.seibel.distanthorizons.core.wrapperInterfaces.world.IClientLevelWrapper LEVEL = DumpClientLevel.create();
	@Override public String name() { return "quads"; }

	private static final int[] COLORS = { 0xFF3A8F2B, 0xFF7B5B3A, 0xFFAAAAAA, 0xFFFF0000, 0x80336699, 0xC0884422, 0xFE112233 };
	private static final int[] MATS = { 0, 13, 13, 12, 2 };

	@Override
	public List<Obj> cases()
	{
		Gen g = new Gen(0x9A4D5L);
		List<Obj> out = new ArrayList<>();
		for (int n = 0; n < 360; n++)
		{
			boolean transparency = (n % 2) == 1;
			int grassMode = (n / 2) % 3;
			int debug = g.r.nextInt(12) == 0 ? 1 + g.r.nextInt(3) : 0;
			int earth = g.r.nextInt(8) == 0 ? 100 : 0;
			boolean showErr = g.r.nextInt(6) == 0;

			int callCount = 2 + g.r.nextInt(10);
			List<Object> calls = new ArrayList<>();
			int[] prev = null;
			for (int i = 0; i < callCount; i++)
			{
				int[] c = (prev != null && g.r.nextInt(100) < 55) ? continuation(g, prev) : randomCall(g);
				calls.add(toJson(c));
				prev = c;
			}

			Config.Client.Advanced.Graphics.Quality.grassSideRendering.set(EDhApiGrassSideRendering.values()[grassMode]);
			Config.Client.Advanced.Debugging.debugRenderingColors.set(EDhApiDebugRendering.values()[debug]);
			Config.Client.Advanced.Graphics.Experimental.earthCurveRatio.set(earth);
			Config.Client.Advanced.Debugging.showOverlappingQuadErrors.set(showErr);

			Obj res = new Obj();
			try (LodQuadBuilder b = LodQuadBuilder.getBuilder(transparency, LEVEL))
			{
				for (Object o : calls) { apply(b, (List<?>) o); }
				b.mergeQuads();
				res.p("opaqueQuads", b.getCurrentOpaqueQuadsCount()).p("transparentQuads", b.getCurrentTransparentQuadsCount());
				res.p("opaque", buffers(b, true)).p("transparent", buffers(b, false));
			}
			Obj in = new Obj().p("doTransparency", transparency).p("grassSideRendering", grassMode).p("debugRendering", debug)
					.p("earthCurveRatio", earth).p("showOverlappingQuadErrors", showErr).p("calls", calls);
			out.add(Obj.testCase("quads_" + n, in, res));
		}
		return out;
	}

	private static List<Object> buffers(LodQuadBuilder b, boolean opaque)
	{
		List<Object> l = new ArrayList<>();
		try (PhantomArrayListCheckout co = LodQuadBuilder.ARRAY_LIST_POOL.checkoutByteBuffers(0))
		{
			ArrayList<ByteBuffer> bufs = opaque ? b.makeOpaqueVertexBuffers(co) : b.makeTransparentVertexBuffers(co);
			for (ByteBuffer bb : bufs)
			{
				byte[] arr = new byte[bb.remaining()];
				bb.duplicate().get(arr);
				l.add(Json.hex(arr));
			}
		}
		return l;
	}

	private static void apply(LodQuadBuilder b, List<?> c)
	{
		String t = (String) c.get(0);
		if (t.equals("adj"))
		{
			b.addQuadAdj(EDhDirection.values()[(Integer) c.get(1)], s(c, 2), s(c, 3), s(c, 4), s(c, 5), s(c, 6),
					(Integer) c.get(7), s(c, 8), (byte) (int) (Integer) c.get(9), (byte) (int) (Integer) c.get(10), (byte) (int) (Integer) c.get(11));
		}
		else if (t.equals("up"))
		{
			b.addQuadUp(s(c, 1), s(c, 2), s(c, 3), s(c, 4), (Integer) c.get(5), s(c, 6), (byte) (int) (Integer) c.get(7), (byte) (int) (Integer) c.get(8), (byte) (int) (Integer) c.get(9));
		}
		else
		{
			b.addQuadDown(s(c, 1), s(c, 2), s(c, 3), s(c, 4), (Integer) c.get(5), s(c, 6), (byte) (int) (Integer) c.get(7), (byte) (int) (Integer) c.get(8), (byte) (int) (Integer) c.get(9));
		}
	}

	private static short s(List<?> c, int i) { return (short) (int) (Integer) c.get(i); }

	// int[] call layout: type(0 adj,1 up,2 down), dir, x,y,z, w,h, color, tex, mat, sky, block
	private static int[] randomCall(Gen g)
	{
		int type = g.r.nextInt(10) < 6 ? 0 : g.r.nextInt(2) + 1;
		int dir = type == 0 ? 2 + g.r.nextInt(4) : (type == 1 ? 1 : 0);
		int w = 1 + g.r.nextInt(4);
		int h = type == 0 ? 1 + g.r.nextInt(5) : w;
		return new int[] { type, dir, g.r.nextInt(10) * 2 - 6, g.r.nextInt(12) * 2, g.r.nextInt(10) * 2 - 6, w, h,
				COLORS[g.r.nextInt(COLORS.length)], g.r.nextInt(3), MATS[g.r.nextInt(MATS.length)], g.r.nextInt(3) * 7, g.r.nextInt(3) * 5 };
	}

	/** an adjacent quad continuing the previous one along x, z or y, with the same appearance most of the time */
	private static int[] continuation(Gen g, int[] p)
	{
		int[] c = p.clone();
		switch (g.r.nextInt(3))
		{
			case 0: c[2] += p[5]; break;
			case 1: c[4] += p[5]; break;
			default: if (p[0] == 0) { c[3] += p[6]; } else { c[4] += p[5]; }
		}
		if (g.r.nextInt(8) == 0) { c[7] = COLORS[g.r.nextInt(COLORS.length)]; }
		if (g.r.nextInt(10) == 0) { c[5] = 1 + g.r.nextInt(4); if (p[0] != 0) { c[6] = c[5]; } }
		if (g.r.nextInt(10) == 0) { c[10] = g.r.nextInt(16); }
		return c;
	}

	private static List<Object> toJson(int[] c)
	{
		List<Object> l = new ArrayList<>();
		if (c[0] == 0)
		{
			l.add("adj"); l.add(c[1]); l.add(c[2]); l.add(c[3]); l.add(c[4]); l.add(c[5]); l.add(c[6]); l.add(c[7]); l.add(c[8]); l.add(c[9]); l.add(c[10]); l.add(c[11]);
		}
		else
		{
			l.add(c[0] == 1 ? "up" : "down"); l.add(c[2]); l.add(c[3]); l.add(c[4]); l.add(c[5]); l.add(c[7]); l.add(c[8]); l.add(c[9]); l.add(c[10]); l.add(c[11]);
		}
		return l;
	}
}
