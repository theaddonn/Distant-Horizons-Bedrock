package dumper;

import com.seibel.distanthorizons.api.enums.config.EDhApiGrassSideRendering;
import com.seibel.distanthorizons.api.enums.rendering.EDhApiDebugRendering;
import com.seibel.distanthorizons.api.enums.rendering.EDhApiTransparency;
import com.seibel.distanthorizons.core.config.Config;
import com.seibel.distanthorizons.core.dataObjects.render.ColumnRenderSource;
import com.seibel.distanthorizons.core.dataObjects.render.bufferBuilding.ColumnRenderBufferBuilder;
import com.seibel.distanthorizons.core.dataObjects.render.bufferBuilding.LodQuadBuilder;
import com.seibel.distanthorizons.core.pos.DhSectionPos;
import com.seibel.distanthorizons.core.util.RenderDataPointUtil;
import com.seibel.distanthorizons.core.util.objects.pooling.PhantomArrayList.PhantomArrayListCheckout;
import com.seibel.distanthorizons.core.wrapperInterfaces.world.IClientLevelWrapper;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.Random;

/**
 * mesh: ColumnRenderBufferBuilder.makeLodRenderData(quadBuilder, center, level, [N,S,W,E], isSameDetailLevel).
 * in : {"options":{"transparency":0 DISABLED|1 COMPLETE (also the LodQuadBuilder doTransparency flag),"grassSideRendering":0..2,
 *                  "enableCaveCulling":bool,"caveCullingHeight":int,"brightnessMultiplier":float,"saturationMultiplier":float,
 *                  "debugRendering":0 (always OFF)},
 *       "level":{"minHeight":int},
 *       "center":<render source>,"neighbours":[N,S,W,E each <render source> or null],"sameDetail":[bool x4 in N,S,W,E order],
 *       "dirtColor":int (client level getDirtBlockColor()),"shades":{"UP":1.0,"DOWN":0.5,"NORTH":0.8,"SOUTH":0.8,"WEST":0.6,"EAST":0.6}}
 *       render source = same encoding as the f2r output: {"pos","maxVerticalSlices","yOffset","dataTable","dataRuns","voidTable","voidRuns"}
 *       (RenderDataPointUtil longs, index 0 = highest; neighbours N = z-1, S = z+1, W = x-1, E = x+1 sections, same detail or one level coarser).
 * out: {"opaqueQuads","transparentQuads","opaque":[hex per ByteBuffer],"transparent":[hex]} - exactly the vertex dump of the quads suite.
 * Note: coarser neighbours are not wrapped (xAdj/zAdj stay out of range), so DH's populateColumnView throws, which is caught and logged, i.e. they behave like "no data".
 */
public class MeshSuite implements Suite
{
	@Override public String name() { return "mesh"; }

	// ---- small render source builder ----
	static final class RS
	{
		final long pos;
		final int maxV, yOffset;
		final long[][] cols = new long[4096][];
		final long[] voids = new long[4096];
		RS(long pos, int maxV, int yOffset) { this.pos = pos; this.maxV = maxV; this.yOffset = yOffset; }
		RS fill(int x0, int z0, int x1, int z1, long[] col) { for (int x = x0; x < x1; x++) { for (int z = z0; z < z1; z++) { this.cols[x * 64 + z] = col; } } return this; }
		RS fill(int x0, int z0, int x1, int z1, long[] col, long voidData) { this.fill(x0, z0, x1, z1, col); for (int x = x0; x < x1; x++) { for (int z = z0; z < z1; z++) { this.voids[x * 64 + z] = voidData; } } return this; }
		RS set(int x, int z, long[] col) { this.cols[x * 64 + z] = col; return this; }
		ColumnRenderSource build()
		{
			ColumnRenderSource s = ColumnRenderSource.createEmpty(this.pos, this.maxV, this.yOffset);
			s.markNotEmpty();
			for (int i = 0; i < 4096; i++)
			{
				long[] c = this.cols[i];
				if (c != null) { for (int k = 0; k < Math.min(c.length, this.maxV); k++) { s.renderDataContainer.set(i * this.maxV + k, c[k]); } }
				s.voidRenderDataContainer.set(i, this.voids[i]);
			}
			return s;
		}
	}

	static long pt(int maxY, int minY, int rgb, int alpha, int sky, int block, int mat)
	{
		return RenderDataPointUtil.createDataPoint(alpha, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, maxY, minY, sky, block, mat);
	}
	static final int GRASS = 0x3A8F2B, DIRT = 0x7B5B3A, STONE = 0x888888, WATER = 0x2255CC, GLASS = 0xCCEEFF, SAND = 0xDDCC88, SNOW = 0xFFFFFF, LAVA = 0xFF6600;
	static long[] terrain(int top) { return new long[] { pt(top, top - 1, GRASS, 255, 15, 0, 13), pt(top - 1, top - 4, DIRT, 255, 15, 0, 5), pt(top - 4, 0, STONE, 255, 15, 0, 2) }; }
	static long[] stone(int top) { return new long[] { pt(top, 0, STONE, 255, 15, 0, 2) }; }
	static long voidAir(int maxY, int sky) { return RenderDataPointUtil.createDataPoint(0, 0, 0, 0, maxY, 0, sky, 0, 14); }

	private static final String[] GEOMS = { "flat", "cliff_x", "cliff_z", "steps", "overhang", "floating_slab", "water_over_sand", "glass_over_stone", "water_glass_stack",
			"void_edge", "pit", "border_lit", "border_unlit", "border_mixed", "coarser_N", "coarser_all", "missing_all", "missing_some", "grass_top", "cave_cut",
			"checker", "single_column", "spire", "plateau", "lava_glow" };

	private static final float[][] COLOR_MULT = { { 1f, 1f }, { 1f, 1f }, { 1.25f, 0.75f }, { 0.8f, 1.3f } };

	@Override
	public List<Obj> cases()
	{
		List<Obj> out = new ArrayList<>();
		for (int g = 0; g < GEOMS.length; g++)
		{
			for (int v = 0; v < 4; v++)
			{
				Random r = new Random(0x3E5A00L + g * 8 + v);
				out.add(run(GEOMS[g] + "_v" + v, GEOMS[g], v, r, g, false));
			}
		}
		for (int i = 0; i < 60; i++)
		{
			Random r = new Random(0x3E5B00L + i);
			out.add(run("random_" + i, null, i, r, i, true));
		}
		return out;
	}

	private Obj run(String name, String geom, int v, Random r, int salt, boolean random)
	{
		int minH = (v % 2 == 0) ? -64 : 0;
		int opt = random ? r.nextInt(4) : v;
		boolean transparency = random ? r.nextBoolean() : (v >= 2);
		int grass = random ? r.nextInt(3) : v % 3;
		boolean cull = random ? r.nextBoolean() : (v != 1);
		int cullH = random ? r.nextInt(200) : (v == 3 ? 40 : 80);
		float bright = COLOR_MULT[opt][0], sat = COLOR_MULT[opt][1];

		byte detail = 6;
		long pos = DhSectionPos.encode(detail, 3 + salt % 5, -2 - salt % 4);
		RS center = new RS(pos, 4, minH);
		RS[] adj = new RS[4];
		boolean[] same = new boolean[4];
		for (int i = 0; i < 4; i++)
		{
			adj[i] = new RS(neighbourPos(pos, i, false), 4, minH);
			same[i] = true;
		}
		if (random) { randomGeometry(center, adj, same, r, pos, minH); }
		else { geometry(geom, center, adj, same, r, pos, minH); }

		Config.Client.Advanced.Graphics.Quality.transparency.setWithoutFiringEvents(transparency ? EDhApiTransparency.COMPLETE : EDhApiTransparency.DISABLED);
		Config.Client.Advanced.Graphics.Quality.grassSideRendering.setWithoutFiringEvents(EDhApiGrassSideRendering.values()[grass]);
		Config.Client.Advanced.Graphics.Culling.enableCaveCulling.setWithoutFiringEvents(cull);
		Config.Client.Advanced.Graphics.Culling.caveCullingHeight.setWithoutFiringEvents(cullH);
		Config.Client.Advanced.Graphics.Quality.brightnessMultiplier.setWithoutFiringEvents(bright);
		Config.Client.Advanced.Graphics.Quality.saturationMultiplier.setWithoutFiringEvents(sat);
		Config.Client.Advanced.Debugging.debugRenderingColors.setWithoutFiringEvents(EDhApiDebugRendering.OFF);
		Config.Client.Advanced.Debugging.ColumnBuilderDebugging.columnBuilderDebugEnable.setWithoutFiringEvents(false);
		Config.Client.Advanced.Graphics.Experimental.earthCurveRatio.setWithoutFiringEvents(0);
		Config.Client.Advanced.Debugging.showOverlappingQuadErrors.setWithoutFiringEvents(false);
		Config.Client.Advanced.Graphics.Texture.enableTexturedLods.setWithoutFiringEvents(false);

		IClientLevelWrapper level = DumpClientLevel.create(minH, minH + 384, false, false);
		ColumnRenderSource cs = center.build();
		ColumnRenderSource[] adjSrc = new ColumnRenderSource[4];
		List<Object> adjJson = new ArrayList<>();
		for (int i = 0; i < 4; i++)
		{
			adjSrc[i] = adj[i] == null ? null : adj[i].build();
			adjJson.add(adj[i] == null ? null : F2rSuite.dumpRender(adjSrc[i]));
		}
		Obj in = new Obj().p("options", new Obj().p("transparency", transparency ? 1 : 0).p("grassSideRendering", grass).p("enableCaveCulling", cull)
						.p("caveCullingHeight", cullH).p("brightnessMultiplier", bright).p("saturationMultiplier", sat).p("debugRendering", 0))
				.p("level", new Obj().p("minHeight", minH)).p("center", F2rSuite.dumpRender(cs)).p("neighbours", adjJson)
				.p("sameDetail", new boolean[] { same[0], same[1], same[2], same[3] })
				.p("dirtColor", level.getDirtBlockColor())
				.p("shades", new Obj().p("UP", 1.0f).p("DOWN", 0.5f).p("NORTH", 0.8f).p("SOUTH", 0.8f).p("WEST", 0.6f).p("EAST", 0.6f));

		Obj res = new Obj();
		try (LodQuadBuilder b = LodQuadBuilder.getBuilder(transparency, level))
		{
			ColumnRenderBufferBuilder.makeLodRenderData(b, cs, level, adjSrc, same);
			res.p("opaqueQuads", b.getCurrentOpaqueQuadsCount()).p("transparentQuads", b.getCurrentTransparentQuadsCount());
			res.p("opaque", buffers(b, true)).p("transparent", buffers(b, false));
		}
		return Obj.testCase(name, in, res);
	}

	/** neighbour index order N,S,W,E = compass 0..3 */
	private static long neighbourPos(long pos, int i, boolean coarser)
	{
		int[] dx = { 0, 0, -1, 1 }, dz = { -1, 1, 0, 0 };
		long p = DhSectionPos.encode(DhSectionPos.getDetailLevel(pos), DhSectionPos.getX(pos) + dx[i], DhSectionPos.getZ(pos) + dz[i]);
		return coarser ? DhSectionPos.getParentPos(p) : p;
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

	// ---------------------------------------------------------------- geometries

	private static void edgeStrip(RS s, int side, long[] col)
	{
		// side: 0 N (z=63 row of the north neighbour), 1 S (z=0 row), 2 W (x=63 col), 3 E (x=0 col)
		for (int i = 0; i < 64; i++)
		{
			if (side == 0) { s.set(i, 63, col); }
			else if (side == 1) { s.set(i, 0, col); }
			else if (side == 2) { s.set(63, i, col); }
			else { s.set(0, i, col); }
		}
	}

	private static void geometry(String g, RS c, RS[] adj, boolean[] same, Random r, long pos, int minH)
	{
		switch (g)
		{
			case "flat":
				c.fill(0, 0, 64, 64, terrain(80));
				for (int i = 0; i < 4; i++) { edgeStrip(adj[i], i, terrain(80)); }
				break;
			case "cliff_x":
				c.fill(0, 0, 32, 64, terrain(110)).fill(32, 0, 64, 64, terrain(70));
				edgeStrip(adj[2], 2, terrain(110)); edgeStrip(adj[3], 3, terrain(70)); edgeStrip(adj[0], 0, terrain(110)); edgeStrip(adj[1], 1, terrain(70));
				break;
			case "cliff_z":
				c.fill(0, 0, 64, 20, terrain(60)).fill(0, 20, 64, 64, terrain(95));
				for (int i = 0; i < 4; i++) { edgeStrip(adj[i], i, terrain(i == 0 ? 60 : 95)); }
				break;
			case "steps":
				for (int x = 0; x < 64; x += 8) { c.fill(x, 0, x + 8, 64, terrain(40 + x / 2)); }
				edgeStrip(adj[2], 2, terrain(40)); edgeStrip(adj[3], 3, terrain(40 + 31));
				break;
			case "overhang":
				c.fill(0, 0, 64, 64, new long[] { pt(100, 90, STONE, 255, 15, 0, 2), pt(60, 0, STONE, 255, 5, 0, 2) });
				c.fill(20, 20, 40, 40, new long[] { pt(100, 90, GRASS, 255, 15, 0, 13), pt(90, 80, DIRT, 255, 15, 0, 5), pt(60, 0, STONE, 255, 5, 0, 2) });
				edgeStrip(adj[0], 0, new long[] { pt(100, 90, STONE, 255, 15, 0, 2), pt(60, 0, STONE, 255, 5, 0, 2) });
				break;
			case "floating_slab":
				c.fill(10, 10, 54, 54, new long[] { pt(130, 120, GRASS, 255, 15, 0, 13), pt(120, 100, STONE, 255, 8, 0, 2) }, voidAir(100, 15));
				break;
			case "water_over_sand":
				c.fill(0, 0, 64, 64, new long[] { pt(64, 56, WATER, 120, 12, 0, 12), pt(56, 50, SAND, 255, 10, 0, 9), pt(50, 0, STONE, 255, 0, 0, 2) });
				c.fill(20, 20, 30, 30, terrain(70));
				for (int i = 0; i < 4; i++) { edgeStrip(adj[i], i, new long[] { pt(64, 56, WATER, 120, 12, 0, 12), pt(56, 50, SAND, 255, 10, 0, 9), pt(50, 0, STONE, 255, 0, 0, 2) }); }
				break;
			case "glass_over_stone":
				c.fill(0, 0, 64, 64, new long[] { pt(90, 88, GLASS, 90, 15, 0, 0), pt(88, 70, STONE, 255, 15, 0, 2) });
				c.fill(30, 0, 34, 64, new long[] { pt(90, 70, STONE, 255, 15, 0, 2) });
				break;
			case "water_glass_stack":
				c.fill(0, 0, 64, 64, new long[] { pt(100, 98, GLASS, 90, 15, 0, 0), pt(98, 90, WATER, 120, 14, 0, 12), pt(90, 88, GLASS, 90, 12, 0, 0), pt(88, 0, STONE, 255, 5, 0, 2) });
				c.fill(10, 10, 20, 20, new long[] { pt(100, 98, GLASS, 90, 15, 0, 0), pt(98, 0, STONE, 255, 5, 0, 2) });
				break;
			case "void_edge":
				c.fill(8, 8, 56, 56, terrain(70), voidAir(70, 15));
				adj[0] = null; adj[3] = null; same[0] = false; same[3] = false;
				edgeStrip(adj[1], 1, terrain(70));
				break;
			case "pit":
				c.fill(0, 0, 64, 64, terrain(80));
				for (int x = 24; x < 40; x++) { for (int z = 24; z < 40; z++) { c.cols[x * 64 + z] = null; } }
				break;
			case "border_lit":
				c.fill(0, 0, 64, 64, new long[] { pt(80, 70, GRASS, 255, 15, 0, 13), pt(70, 0, STONE, 255, 15, 0, 2) });
				for (int i = 0; i < 4; i++) { edgeStrip(adj[i], i, new long[] { pt(60, 50, GRASS, 255, 15, 0, 13), pt(50, 0, STONE, 255, 15, 0, 2) }); }
				break;
			case "border_unlit":
				c.fill(0, 0, 64, 64, new long[] { pt(80, 70, GRASS, 255, 0, 3, 13), pt(70, 0, STONE, 255, 0, 0, 2) });
				for (int i = 0; i < 4; i++) { edgeStrip(adj[i], i, new long[] { pt(60, 50, GRASS, 255, 0, 0, 13), pt(50, 0, STONE, 255, 0, 5, 2) }); }
				break;
			case "border_mixed":
				c.fill(0, 0, 64, 64, terrain(75));
				edgeStrip(adj[0], 0, terrain(90)); edgeStrip(adj[1], 1, terrain(75)); edgeStrip(adj[2], 2, new long[] { pt(75, 40, STONE, 255, 15, 0, 2) });
				edgeStrip(adj[3], 3, new long[] { pt(100, 90, STONE, 255, 15, 0, 2), pt(75, 0, STONE, 255, 15, 0, 2) });
				break;
			case "coarser_N":
				c.fill(0, 0, 64, 64, terrain(80));
				adj[0] = new RS(neighbourPos(pos, 0, true), 4, minH).fill(0, 0, 64, 64, terrain(100)); same[0] = false;
				for (int i = 1; i < 4; i++) { edgeStrip(adj[i], i, terrain(80)); }
				break;
			case "coarser_all":
				c.fill(0, 0, 64, 64, terrain(80));
				for (int i = 0; i < 4; i++) { adj[i] = new RS(neighbourPos(pos, i, true), 4, minH).fill(0, 0, 64, 64, terrain(60 + i * 15)); same[i] = false; }
				break;
			case "missing_all":
				c.fill(0, 0, 64, 64, terrain(80));
				for (int i = 0; i < 4; i++) { adj[i] = null; same[i] = false; }
				break;
			case "missing_some":
				c.fill(0, 0, 64, 64, terrain(80));
				adj[1] = null; same[1] = false; adj[2] = null; same[2] = false;
				edgeStrip(adj[0], 0, terrain(70)); edgeStrip(adj[3], 3, terrain(95));
				break;
			case "grass_top":
				c.fill(0, 0, 64, 64, new long[] { pt(80, 79, GRASS, 255, 15, 0, 13), pt(79, 0, DIRT, 255, 15, 0, 5) });
				c.fill(16, 16, 48, 48, new long[] { pt(100, 99, GRASS, 255, 15, 0, 13), pt(99, 0, STONE, 255, 15, 0, 2) });
				break;
			case "cave_cut":
				c.fill(0, 0, 64, 64, new long[] { pt(100, 99, GRASS, 255, 15, 0, 13), pt(99, 90, STONE, 255, 0, 0, 2), pt(70, 60, STONE, 255, 0, 0, 2), pt(55, 30, STONE, 255, 0, 0, 2), pt(20, 0, STONE, 255, 0, 0, 2) });
				c.fill(20, 20, 30, 30, new long[] { pt(100, 99, GRASS, 255, 15, 0, 13), pt(99, 0, STONE, 255, 0, 0, 2) });
				break;
			case "checker":
				for (int x = 0; x < 16; x++) { for (int z = 0; z < 16; z++) { c.set(x, z, terrain(((x + z) & 1) == 0 ? 60 : 72)); } }
				break;
			case "single_column":
				c.set(31, 31, terrain(90));
				break;
			case "spire":
				c.fill(0, 0, 64, 64, terrain(60));
				c.fill(30, 30, 32, 32, new long[] { pt(140, 139, STONE, 255, 15, 0, 2), pt(139, 60, STONE, 255, 15, 0, 2) });
				break;
			case "plateau":
				c.fill(0, 0, 64, 64, terrain(50));
				c.fill(0, 0, 64, 64, terrain(120));
				for (int i = 0; i < 4; i++) { edgeStrip(adj[i], i, terrain(50)); }
				break;
			case "lava_glow":
				c.fill(0, 0, 64, 64, new long[] { pt(70, 60, LAVA, 255, 0, 15, 6), pt(60, 0, STONE, 255, 0, 3, 2) });
				c.fill(20, 20, 24, 24, terrain(70));
				for (int i = 0; i < 4; i++) { edgeStrip(adj[i], i, terrain(65)); }
				break;
			default: throw new IllegalArgumentException(g);
		}
	}

	private static void randomGeometry(RS c, RS[] adj, boolean[] same, Random r, long pos, int minH)
	{
		int regions = 2 + r.nextInt(5);
		for (int i = 0; i < regions; i++)
		{
			int x0 = r.nextInt(60), z0 = r.nextInt(60), x1 = Math.min(64, x0 + 1 + r.nextInt(30)), z1 = Math.min(64, z0 + 1 + r.nextInt(30));
			c.fill(x0, z0, x1, z1, randomColumn(r), r.nextInt(5) == 0 ? voidAir(40 + r.nextInt(60), r.nextInt(16)) : 0L);
		}
		for (int i = 0; i < 4; i++)
		{
			int k = r.nextInt(6);
			if (k == 0) { adj[i] = null; same[i] = false; }
			else if (k == 1) { adj[i] = new RS(neighbourPos(pos, i, true), 4, minH).fill(0, 0, 64, 64, randomColumn(r)); same[i] = false; }
			else { for (int s = 0; s < 3; s++) { edgeStrip(adj[i], i, randomColumn(r)); } }
		}
	}

	private static long[] randomColumn(Random r)
	{
		int n = 1 + r.nextInt(4);
		long[] col = new long[n];
		int y = 40 + r.nextInt(100);
		int[] cols = { GRASS, DIRT, STONE, WATER, GLASS, SAND, SNOW, LAVA };
		int[] mats = { 13, 5, 2, 12, 0, 9, 8, 6 };
		for (int i = 0; i < n; i++)
		{
			int h = 1 + r.nextInt(20);
			int k = r.nextInt(cols.length);
			int alpha = (k == 3 || k == 4) ? 80 + r.nextInt(100) : 255;
			int bottom = Math.max(0, y - h);
			if (i == n - 1) { bottom = r.nextBoolean() ? 0 : bottom; }
			col[i] = pt(y, bottom, cols[k], alpha, r.nextInt(16), r.nextInt(16), mats[k]);
			y = bottom - r.nextInt(3) * (r.nextInt(4) == 0 ? 1 : 0);
			if (y <= 0) { return java.util.Arrays.copyOf(col, i + 1); }
		}
		return col;
	}
}
