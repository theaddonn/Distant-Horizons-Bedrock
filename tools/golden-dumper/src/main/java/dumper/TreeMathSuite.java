package dumper;

import com.seibel.distanthorizons.api.enums.config.EDhApiHorizontalQuality;
import com.seibel.distanthorizons.api.enums.config.EDhApiMaxHorizontalResolution;
import com.seibel.distanthorizons.api.objects.math.DhApiMat4f;
import com.seibel.distanthorizons.core.config.Config;
import com.seibel.distanthorizons.core.pos.DhSectionPos;
import com.seibel.distanthorizons.core.pos.blockPos.DhBlockPos2D;
import com.seibel.distanthorizons.core.render.CameraZoom;
import com.seibel.distanthorizons.core.render.QuadTree.LodQuadTree;
import com.seibel.distanthorizons.core.render.renderer.cullingFrustum.DhFrustumBounds;
import com.seibel.distanthorizons.coreapi.util.MathUtil;
import org.joml.FrustumIntersection;
import org.joml.Matrix4f;
import org.joml.Vector3f;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * treemath: pure maths of the render quad tree and the view frustum.
 *
 * detail_N : LodQuadTree detail-level maths. The tree is instantiated WITHOUT running its constructor (Unsafe.allocateInstance); only
 *   blockRenderDistanceDiameter and treeRootDetailLevel (= max(max(1, 6), MathUtil.log2(diameter)), as in QuadTree) are set, then the
 *   private updateDetailLevelVariables() is run against the Config values.
 *   in : {"horizontalQuality":ordinal,"maxHorizontalResolution":ordinal,"diameter":int,"player":[x,z],
 *         "distances":[double...],"sections":[long...],"blocks":[[x,z]...],"zoom":null|{"magnification","coneTanHalfAngle","lookX","lookZ"}}
 *   out: {"dropOffUnit":double,"dropOffLogBase":double,"maxLeafDetail":int,"minRootDetail":int,"treeRootDetail":int,
 *         "fromDistance":[int...]        calcDetailLevelFromDistance(distances[i]),
 *         "expectedSection":[int...]     calcExpectedDetailLevel(player, sections[i]),
 *         "expectedBlock":[int...]       calcExpectedDetailLevel(player, x, z)}
 *   (With zoom != null the CameraZoom.INSTANCE fields are set for the call; detail levels of sections >= 12 never zoom.)
 * cone_N   : CameraZoom.coneIntersectsCircle(ox,oz,cx,cz,r) with a random cone. in {"coneTanHalfAngle","lookX","lookZ","origin":[x,z],"circles":[[cx,cz,r]...]} out {"hits":[bool...]}
 * frustum_N: DhFrustumBounds.update(minY,maxY,mat) then intersects(). Matrices are DhApiMat4f element order (m00,m01,m02,m03,m10,...; DH
 *   transposes into JOML). JOML 1.10.2.
 *   in : {"minY","maxY","mat":[16 floats],"matBits":[16 int bit patterns],"boxes":[[minX,minZ,width,detail]...]}
 *   out: {"planeNames":[...24 JOML FrustumIntersection field names],"planes":[24 floats],"planeBits":[24 ints],
 *         "boundsMin":[3],"boundsMax":[3] (frustumAabb of the inverse),"visible":[bool per box]}
 */
public class TreeMathSuite implements Suite
{
	@Override public String name() { return "treemath"; }

	@Override
	public List<Obj> cases() throws Exception
	{
		List<Obj> out = new ArrayList<>();
		Gen g = new Gen(0x7EE3A7L);

		sun.misc.Unsafe unsafe;
		{
			Field uf = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
			uf.setAccessible(true);
			unsafe = (sun.misc.Unsafe) uf.get(null);
		}
		Field diameterField = LodQuadTree.class.getField("blockRenderDistanceDiameter");
		Field rootField = com.seibel.distanthorizons.core.util.objects.quadTree.QuadTree.class.getField("treeRootDetailLevel");
		diameterField.setAccessible(true);
		rootField.setAccessible(true);
		Method update = LodQuadTree.class.getDeclaredMethod("updateDetailLevelVariables");
		Method fromDist = LodQuadTree.class.getDeclaredMethod("calcDetailLevelFromDistance", double.class);
		update.setAccessible(true);
		fromDist.setAccessible(true);
		Field unitF = LodQuadTree.class.getDeclaredField("detailDropOffDistanceUnit");
		Field logF = LodQuadTree.class.getDeclaredField("detailDropOffLogBase");
		Field leafF = LodQuadTree.class.getDeclaredField("maxLeafRenderDetailLevel");
		Field rootRF = LodQuadTree.class.getDeclaredField("minRootRenderDetailLevel");
		for (Field f : new Field[] { unitF, logF, leafF, rootRF }) { f.setAccessible(true); }

		// ---------------- detail level maths ----------------
		for (int n = 0; n < 1300; n++)
		{
			int hq = g.r.nextInt(EDhApiHorizontalQuality.values().length);
			int mhr = g.r.nextInt(EDhApiMaxHorizontalResolution.values().length);
			int diameter = g.r.nextInt(5) == 0 ? 64 + g.r.nextInt(512) : 256 + g.r.nextInt(1 << (9 + g.r.nextInt(9)));
			int px = g.anyInt() >> (g.r.nextBoolean() ? 8 : 0), pz = g.anyInt() >> (g.r.nextBoolean() ? 8 : 0);
			if (g.r.nextInt(3) == 0) { px = g.r.nextInt(2001) - 1000; pz = g.r.nextInt(2001) - 1000; }

			List<Object> distances = new ArrayList<>();
			for (int i = 0; i < 8; i++)
			{
				double d;
				switch (g.r.nextInt(5))
				{
					case 0: d = g.r.nextInt(300); break;
					case 1: d = g.r.nextDouble() * 100000; break;
					case 2: d = Math.pow(2, g.r.nextDouble() * 17); break;
					case 3: d = new double[] { 0.0, 1.0, 16.0, 64.0, 1e9, Double.MIN_VALUE }[g.r.nextInt(6)]; break;
					default: d = g.r.nextDouble() * 5000;
				}
				distances.add(d);
			}
			List<Object> sections = new ArrayList<>();
			for (int i = 0; i < 6; i++)
			{
				byte det = (byte) (6 + g.r.nextInt(g.r.nextInt(5) == 0 ? 22 : 10));
				int sx = (px >> det) + g.r.nextInt(41) - 20, sz = (pz >> det) + g.r.nextInt(41) - 20;
				sections.add(DhSectionPos.encode(det, sx, sz));
			}
			List<Object> blocks = new ArrayList<>();
			for (int i = 0; i < 4; i++) { blocks.add(java.util.Arrays.asList(px + g.r.nextInt(8001) - 4000, pz + g.r.nextInt(8001) - 4000)); }

			Obj zoom = null;
			if (g.r.nextInt(3) == 0)
			{
				double a = g.r.nextDouble() * Math.PI * 2;
				zoom = new Obj().p("magnification", 1.5 + g.r.nextDouble() * 6).p("coneTanHalfAngle", 0.05 + g.r.nextDouble() * 0.5)
						.p("lookX", Math.cos(a)).p("lookZ", Math.sin(a));
			}

			Config.Client.Advanced.Graphics.Quality.horizontalQuality.set(EDhApiHorizontalQuality.values()[hq]);
			Config.Client.Advanced.Graphics.Quality.maxHorizontalResolution.set(EDhApiMaxHorizontalResolution.values()[mhr]);
			LodQuadTree tree = (LodQuadTree) unsafe.allocateInstance(LodQuadTree.class);
			diameterField.setInt(tree, diameter);
			byte rootLevel = (byte) Math.max(Math.max(1, (int) DhSectionPos.SECTION_MINIMUM_DETAIL_LEVEL), MathUtil.log2(diameter));
			rootField.setByte(tree, rootLevel);
			update.invoke(tree);

			CameraZoom z = CameraZoom.INSTANCE;
			if (zoom != null)
			{
				z.magnification = (Double) zoom.get("magnification");
				z.coneTanHalfAngle = (Double) zoom.get("coneTanHalfAngle");
				z.lookDirectionX = (Double) zoom.get("lookX");
				z.lookDirectionZ = (Double) zoom.get("lookZ");
			}
			DhBlockPos2D player = new DhBlockPos2D(px, pz);
			List<Object> fromD = new ArrayList<>(), expS = new ArrayList<>(), expB = new ArrayList<>();
			for (Object d : distances) { fromD.add((int) (Byte) fromDist.invoke(tree, (Double) d)); }
			for (Object s : sections) { expS.add((int) tree.calcExpectedDetailLevel(player, (Long) s)); }
			for (Object b : blocks) { List<?> bl = (List<?>) b; expB.add((int) tree.calcExpectedDetailLevel(player, (Integer) bl.get(0), (Integer) bl.get(1))); }
			z.magnification = CameraZoom.NOT_ZOOMED_MAGNIFICATION; z.coneTanHalfAngle = 0; z.lookDirectionX = 0; z.lookDirectionZ = 0;

			Obj in = new Obj().p("horizontalQuality", hq).p("maxHorizontalResolution", mhr).p("diameter", diameter)
					.p("player", java.util.Arrays.asList(px, pz)).p("distances", distances).p("sections", sections).p("blocks", blocks).p("zoom", zoom);
			Obj res = new Obj().p("dropOffUnit", unitF.getDouble(tree)).p("dropOffLogBase", logF.getDouble(tree))
					.p("maxLeafDetail", (int) leafF.getByte(tree)).p("minRootDetail", (int) rootRF.getByte(tree)).p("treeRootDetail", (int) rootLevel)
					.p("fromDistance", fromD).p("expectedSection", expS).p("expectedBlock", expB);
			out.add(Obj.testCase("detail_" + n, in, res));
		}

		// ---------------- zoom cone ----------------
		for (int n = 0; n < 300; n++)
		{
			double a = g.r.nextDouble() * Math.PI * 2;
			CameraZoom z = CameraZoom.INSTANCE;
			z.magnification = 2.0;
			z.coneTanHalfAngle = 0.02 + g.r.nextDouble() * 0.6;
			z.lookDirectionX = Math.cos(a);
			z.lookDirectionZ = Math.sin(a);
			double ox = g.r.nextInt(2001) - 1000, oz = g.r.nextInt(2001) - 1000;
			List<Object> circles = new ArrayList<>(), hits = new ArrayList<>();
			for (int i = 0; i < 8; i++)
			{
				double cx = ox + (g.r.nextDouble() - 0.5) * 4000, cz = oz + (g.r.nextDouble() - 0.5) * 4000, r = g.r.nextInt(4) == 0 ? 0.0 : g.r.nextDouble() * 700;
				circles.add(java.util.Arrays.asList(cx, cz, r));
				hits.add(z.coneIntersectsCircle(ox, oz, cx, cz, r));
			}
			Obj in = new Obj().p("coneTanHalfAngle", z.coneTanHalfAngle).p("lookX", z.lookDirectionX).p("lookZ", z.lookDirectionZ)
					.p("origin", java.util.Arrays.asList(ox, oz)).p("circles", circles);
			z.magnification = CameraZoom.NOT_ZOOMED_MAGNIFICATION; z.coneTanHalfAngle = 0; z.lookDirectionX = 0; z.lookDirectionZ = 0;
			out.add(Obj.testCase("cone_" + n, in, new Obj().p("hits", hits)));
		}

		// ---------------- frustum ----------------
		Field[] planeFields = FrustumIntersection.class.getDeclaredFields();
		List<Field> pf = new ArrayList<>();
		for (Field f : planeFields) { if (f.getType() == float.class && !java.lang.reflect.Modifier.isStatic(f.getModifiers())) { f.setAccessible(true); pf.add(f); } }
		Field bminF = DhFrustumBounds.class.getDeclaredField("boundsMin"), bmaxF = DhFrustumBounds.class.getDeclaredField("boundsMax"), frF = DhFrustumBounds.class.getDeclaredField("frustum");
		bminF.setAccessible(true); bmaxF.setAccessible(true); frF.setAccessible(true);
		for (int n = 0; n < 700; n++)
		{
			float fov = 20 + g.r.nextFloat() * 100, aspect = 0.5f + g.r.nextFloat() * 2f, near = 0.05f + g.r.nextFloat() * 2f, far = 64 + g.r.nextFloat() * (g.r.nextBoolean() ? 2000 : 40000);
			Matrix4f m = new Matrix4f().perspective((float) Math.toRadians(fov), aspect, near, far);
			if (g.r.nextInt(10) == 0) { m = new Matrix4f().ortho(-100 - g.r.nextInt(500), 100 + g.r.nextInt(500), -80, 80, near, far); }
			m.rotateX((g.r.nextFloat() - 0.5f) * 3.0f).rotateY(g.r.nextFloat() * 6.2831853f);
			if (g.r.nextInt(6) == 0) { m.rotateZ((g.r.nextFloat() - 0.5f) * 0.6f); }
			float spread = g.r.nextInt(4) == 0 ? 100000f : 600f;
			m.translate((g.r.nextFloat() - 0.5f) * spread, (g.r.nextFloat() - 0.5f) * 400f, (g.r.nextFloat() - 0.5f) * spread);
			float[] colMajor = new float[16];
			m.get(colMajor);
			float[] dh = new float[16]; // DhApiMat4f order (row-major): values[r*4+c]
			int[] bits = new int[16];
			for (int r = 0; r < 4; r++) { for (int c = 0; c < 4; c++) { dh[r * 4 + c] = colMajor[c * 4 + r]; } }
			for (int i = 0; i < 16; i++) { bits[i] = Float.floatToRawIntBits(dh[i]); }
			int minY = g.r.nextBoolean() ? -64 : 0, maxY = g.r.nextBoolean() ? 320 : 256;

			DhFrustumBounds fb = new DhFrustumBounds();
			fb.update(minY, maxY, new DhApiMat4f(dh));

			List<Object> boxes = new ArrayList<>(), vis = new ArrayList<>();
			// the inverse frustum AABB tells where the camera looks: sample boxes inside and outside of it
			Vector3f bmin = (Vector3f) bminF.get(fb), bmax = (Vector3f) bmaxF.get(fb);
			for (int i = 0; i < 12; i++)
			{
				int det = 6 + g.r.nextInt(10);
				int width = 1 << det;
				float fx = bmin.x + (bmax.x - bmin.x) * g.r.nextFloat() + (g.r.nextInt(4) == 0 ? (g.r.nextFloat() - 0.5f) * 4000f : 0f);
				float fz = bmin.z + (bmax.z - bmin.z) * g.r.nextFloat() + (g.r.nextInt(4) == 0 ? (g.r.nextFloat() - 0.5f) * 4000f : 0f);
				int bx = (int) Math.max(-1e9f, Math.min(1e9f, fx)) & ~(width - 1), bz = (int) Math.max(-1e9f, Math.min(1e9f, fz)) & ~(width - 1);
				boxes.add(java.util.Arrays.asList(bx, bz, width, det));
				vis.add(fb.intersects(bx, bz, width, det));
			}
			FrustumIntersection fi = (FrustumIntersection) frF.get(fb);
			List<Object> names = new ArrayList<>(), values = new ArrayList<>(), vbits = new ArrayList<>();
			for (Field f : pf) { names.add(f.getName()); float v = f.getFloat(fi); values.add(v); vbits.add(Float.floatToRawIntBits(v)); }
			Vector3f bn = new Vector3f(bmin), bx = new Vector3f(bmax);
			Obj in = new Obj().p("minY", minY).p("maxY", maxY).p("mat", dh).p("matBits", bits).p("boxes", boxes);
			Obj res = new Obj().p("planeNames", names).p("planes", values).p("planeBits", vbits)
					.p("boundsMin", new float[] { bn.x, bn.y, bn.z }).p("boundsMax", new float[] { bx.x, bx.y, bx.z }).p("visible", vis);
			out.add(Obj.testCase("frustum_" + n, in, res));
		}
		return out;
	}
}
