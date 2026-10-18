package dumper;

import com.seibel.distanthorizons.core.config.Config;
import com.seibel.distanthorizons.core.dataObjects.fullData.sources.FullDataSourceV2;
import com.seibel.distanthorizons.core.file.fullDatafile.V2.FullDataSourceProviderV2;
import com.seibel.distanthorizons.core.file.fullDatafile.V2.FullDataUpdatePropagatorV2;
import com.seibel.distanthorizons.core.file.fullDatafile.V2.FullDataUpdaterV2;
import com.seibel.distanthorizons.core.level.IDhLevel;
import com.seibel.distanthorizons.core.pos.DhSectionPos;
import com.seibel.distanthorizons.core.pos.blockPos.DhBlockPos;
import com.seibel.distanthorizons.core.sql.dto.FullDataSourceV2DTO;
import com.seibel.distanthorizons.core.sql.repo.AbstractDhRepo;
import com.seibel.distanthorizons.core.sql.repo.FullDataSourceV2Repo;
import com.seibel.distanthorizons.core.util.threading.PriorityTaskPicker;
import com.seibel.distanthorizons.core.wrapperInterfaces.world.IClientLevelWrapper;
import it.unimi.dsi.fastutil.longs.LongArrayList;

import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Random;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * propagate: the REAL FullDataSourceProviderV2 data path on an in-memory-like SQLite repo (temp file), driven synchronously:
 *   FullDataUpdaterV2.updateDataSource(input)  (the body of updateDataSourceAsync) for every update of a phase, then
 *   FullDataUpdatePropagatorV2.runParentUpdates + runChildUpdates (private methods called by reflection with an inline executor and
 *   target position (0,0)) repeated until repo.getParentPositionsToUpdate / getChildPositionsToUpdate are both empty.
 * Provider/propagator are created without their constructors (they would start threads); Config numberOfThreads is pinned to 4
 * (=> 40 propagate tasks per pass). The level has sky light (leaf sections get bakeDataSourceSkyLight as in production).
 * in : {"maxTasks":40,"phases":[ [ {"applyToParent":bool|null,"applyToChildren":bool|null,"regenerateLeaf":bool|null,"source":<Ds + optional "fill":{"gen","comp","pts"} applied to every unlisted column>} ... ] ... ]}
 *      updates of one phase are applied in listed order, then propagation runs to quiescence before the next phase.
 * out: {"phases":[{"updateOrder":[pos...],"passes":n}],"sections":[{"pos","applyToParent","applyToChildren","regenerate","data":<Ds dump>}]}
 *      updateOrder = the positions in the order the data updater saved them (listener callbacks: first the phase's own updates, then every
 *      parent/child section the passes rewrote); sections = every stored section, ordered by (detail asc, x asc, z asc), dump after the final phase.
 *      Flags are the stored DTO values (null -> JSON null). "data" = {"pos","palette","runs":[[count,genStep,compMode,[longs]],...],"isEmpty"} (run-length over the 4096 columns in index order relX*64+relZ).
 */
public class PropagateSuite implements Suite
{
	@Override public String name() { return "propagate"; }

	/** runs tasks immediately on the calling thread */
	public static class InlineExecutor extends PriorityTaskPicker.Executor
	{
		InlineExecutor() { super(null, null, 0, null); }
		@Override public void execute(Runnable command) { command.run(); }
		@Override public int getQueueSize() { return 0; }
		@Override public boolean isTerminated() { return false; }
	}

	private static sun.misc.Unsafe unsafe;

	private static void setAny(Object o, String name, Object v) throws Exception
	{
		for (Class<?> c = o.getClass(); c != null; c = c.getSuperclass())
		{
			try
			{
				Field f = c.getDeclaredField(name);
				f.setAccessible(true);
				f.set(o, v);
				return;
			}
			catch (NoSuchFieldException ignored) { }
		}
		throw new NoSuchFieldException(name);
	}

	@Override
	public List<Obj> cases() throws Exception
	{
		Field uf = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
		uf.setAccessible(true);
		unsafe = (sun.misc.Unsafe) uf.get(null);
		Config.Common.MultiThreading.numberOfThreads.setWithoutFiringEvents(4);

		List<Obj> out = new ArrayList<>();
		int n = 0;
		// A: aligned k x k blocks, B: unaligned, C: sparse, D: a higher-level section pushed down, E: rewrites
		for (int rep = 0; rep < 6; rep++) { out.add(scenario("aligned_k2_" + rep, n++, 2, 0, 1.0, false, false)); }
		for (int rep = 0; rep < 6; rep++) { out.add(scenario("aligned_k4_" + rep, n++, 4, 0, 1.0, false, false)); }
		for (int rep = 0; rep < 2; rep++) { out.add(scenario("aligned_k8_" + rep, n++, 8, 0, 1.0, false, false)); }
		for (int rep = 0; rep < 3; rep++) { out.add(scenario("unaligned_k2_" + rep, n++, 2, 1, 1.0, false, false)); }
		for (int rep = 0; rep < 3; rep++) { out.add(scenario("unaligned_k4_" + rep, n++, 4, 1, 1.0, false, false)); }
		out.add(scenario("unaligned_k8_0", n++, 8, 1, 1.0, false, false));
		for (int rep = 0; rep < 3; rep++) { out.add(scenario("sparse_k4_" + rep, n++, 4, rep % 2, 0.65, false, false)); }
		out.add(scenario("sparse_k8_0", n++, 8, 0, 0.65, false, false));
		for (int rep = 0; rep < 4; rep++) { out.add(scenario("mid_applyToChildren_" + rep, n++, 2 + 2 * (rep % 2), 0, 1.0, true, false)); }
		for (int rep = 0; rep < 4; rep++) { out.add(scenario("rewrite_" + rep, n++, 2 + 2 * (rep % 2), rep % 2, 1.0, false, true)); }
		return out;
	}

	private Obj scenario(String name, int salt, int k, int offset, double presence, boolean midChildren, boolean rewrite) throws Exception
	{
		Gen g = new Gen(0x9209A6L + salt * 31L);
		Ds universe = Ds.randomPalette(g, 0, 5);
		universe.fillPts = Ds.randomColumn(g, universe.palette.size(), 3, 60 + g.r.nextInt(40), false, universe);
		while (universe.fillPts.length == 0) { universe.fillPts = Ds.randomColumn(g, universe.palette.size(), 3, 60, false, universe); }
		universe.fillGen = 9;
		universe.fillComp = g.r.nextInt(2);
		List<List<Obj>> phases = new ArrayList<>();
		List<List<Ds>> phaseSources = new ArrayList<>();

		int ox = (g.r.nextInt(40) - 20) * 8 + offset, oz = (g.r.nextInt(40) - 20) * 8 + offset * 3;
		List<Ds> leaves = new ArrayList<>();
		for (int x = 0; x < k; x++)
		{
			for (int z = 0; z < k; z++)
			{
				if (g.r.nextDouble() > presence) { continue; }
				leaves.add(leaf(g, universe, DhSectionPos.encode((byte) 6, ox + x, oz + z)));
			}
		}
		if (leaves.isEmpty()) { leaves.add(leaf(g, universe, DhSectionPos.encode((byte) 6, ox, oz))); }
		Collections.shuffle(leaves, g.r);

		List<Ds> phase1 = new ArrayList<>(leaves);
		List<Boolean> a2c = new ArrayList<>();
		for (int i = 0; i < phase1.size(); i++) { a2c.add(null); }
		if (midChildren)
		{
			// a higher-detail-level section (as world generation / the server would write) that must be pushed down to its children
			int det = 8 + g.r.nextInt(2);
			long mid = DhSectionPos.encode((byte) det, (ox >> (det - 6)), (oz >> (det - 6)));
			Ds m = new Ds(mid);
			m.palette.addAll(universe.palette);
			m.fillPts = universe.fillPts;
			m.fillGen = 9;
			m.fillComp = universe.fillComp;
			for (int i = 0; i < 6; i++) { m.addCol(g.r.nextInt(4096), 9, universe.fillComp, Ds.randomColumn(g, m.palette.size(), 3, 20 + g.r.nextInt(100), false, m)); }
			m.cols.sort((a, b) -> Integer.compare(a.idx, b.idx));
			// remove duplicate column indices (keep first)
			Set<Integer> seen = new HashSet<>();
			m.cols.removeIf(c -> !seen.add(c.idx));
			phase1.add(m);
			a2c.add(true);
		}

		List<Object> phaseJson = new ArrayList<>();
		List<List<Object[]>> plan = new ArrayList<>(); // per phase: {Ds, Boolean applyToChildren}
		List<Object[]> p1 = new ArrayList<>();
		for (int i = 0; i < phase1.size(); i++) { p1.add(new Object[] { phase1.get(i), a2c.get(i) }); }
		plan.add(p1);
		if (rewrite)
		{
			// second phase: some leaves are rewritten with different content
			List<Object[]> p2 = new ArrayList<>();
			for (Ds l : leaves)
			{
				if (g.r.nextInt(3) == 0) { p2.add(new Object[] { leaf(g, universe, l.pos), null }); }
			}
			if (p2.isEmpty()) { p2.add(new Object[] { leaf(g, universe, leaves.get(0).pos), null }); }
			plan.add(p2);
		}
		for (List<Object[]> ph : plan)
		{
			List<Object> upd = new ArrayList<>();
			for (Object[] u : ph)
			{
				Ds d = (Ds) u[0];
				boolean applyToParent = true;
				Boolean applyToChildren = (Boolean) u[1];
				upd.add(new Obj().p("applyToParent", applyToParent).p("applyToChildren", applyToChildren).p("regenerateLeaf", null).p("source", d.json()));
			}
			phaseJson.add(upd);
		}

		// ---------------- run the real code ----------------
		File dir = new File(System.getProperty("golden.scratch", "tools/golden-dumper/build/scratch"), "propagate");
		dir.mkdirs();
		File db = new File(dir, "scenario.sqlite");
		db.delete();
		FullDataSourceV2Repo repo = new FullDataSourceV2Repo(AbstractDhRepo.DEFAULT_DATABASE_TYPE, db);
		IClientLevelWrapper levelWrapper = DumpClientLevel.create(-64, 320, false, false);
		IDhLevel dhLevel = Stub.of(IDhLevel.class).val("getLevelWrapper", levelWrapper).build();
		FullDataSourceProviderV2 provider = (FullDataSourceProviderV2) unsafe.allocateInstance(FullDataSourceProviderV2.class);
		setAny(provider, "repo", repo);
		setAny(provider, "level", dhLevel);
		setAny(provider, "levelWrapper", levelWrapper);
		setAny(provider, "levelId", "dump");
		setAny(provider, "isShutdownRef", new AtomicBoolean(false));
		FullDataUpdaterV2 updater = new FullDataUpdaterV2(provider, "dump");
		setAny(provider, "dataUpdater", updater);
		FullDataUpdatePropagatorV2 propagator = (FullDataUpdatePropagatorV2) unsafe.allocateInstance(FullDataUpdatePropagatorV2.class);
		setAny(propagator, "provider", provider);
		setAny(propagator, "dataUpdater", updater);
		setAny(propagator, "dhLevel", dhLevel);
		setAny(propagator, "updatingPosSet", ConcurrentHashMap.newKeySet());
		setAny(provider, "updatePropagator", propagator);
		Method parentPass = FullDataUpdatePropagatorV2.class.getDeclaredMethod("runParentUpdates", PriorityTaskPicker.Executor.class, DhBlockPos.class);
		Method childPass = FullDataUpdatePropagatorV2.class.getDeclaredMethod("runChildUpdates", PriorityTaskPicker.Executor.class, DhBlockPos.class);
		parentPass.setAccessible(true);
		childPass.setAccessible(true);
		PriorityTaskPicker.Executor executor = (PriorityTaskPicker.Executor) unsafe.allocateInstance(InlineExecutor.class);
		int maxTasks = FullDataUpdatePropagatorV2.getMaxPropagateTaskCount();

		List<Long> order = new ArrayList<>();
		updater.addDataSourceUpdateListener(s -> order.add(s.getPos()));

		List<Object> phaseOut = new ArrayList<>();
		for (List<Object[]> ph : plan)
		{
			order.clear();
			for (Object[] u : ph)
			{
				FullDataSourceV2 s = ((Ds) u[0]).build();
				s.applyToParent = true;
				s.applyToChildren = (Boolean) u[1];
				updater.updateDataSource(s);
			}
			int passes = 0;
			while (passes < 400)
			{
				LongArrayList parents = repo.getParentPositionsToUpdate(0, 0, maxTasks);
				LongArrayList children = repo.getChildPositionsToUpdate(0, 0, maxTasks);
				if (System.getProperty("golden.debug") != null) { System.err.println(name + " pass " + passes + " parents=" + parents.size() + " children=" + children.size() + " " + (parents.isEmpty() ? "" : DhSectionPos.toString(parents.getLong(0)))); }
				if (parents.isEmpty() && children.isEmpty()) { break; }
				parentPass.invoke(propagator, executor, DhBlockPos.ZERO);
				childPass.invoke(propagator, executor, DhBlockPos.ZERO);
				passes++;
			}
			phaseOut.add(new Obj().p("updateOrder", new ArrayList<>(order)).p("passes", passes));
		}

		LongArrayList all = repo.getAllPositions();
		List<Long> sorted = new ArrayList<>();
		for (long p : all) { sorted.add(p); }
		sorted.sort((a, b) ->
		{
			int c = Integer.compare(DhSectionPos.getDetailLevel(a), DhSectionPos.getDetailLevel(b));
			if (c != 0) { return c; }
			c = Integer.compare(DhSectionPos.getX(a), DhSectionPos.getX(b));
			return c != 0 ? c : Integer.compare(DhSectionPos.getZ(a), DhSectionPos.getZ(b));
		});
		List<Object> sections = new ArrayList<>();
		for (long p : sorted)
		{
			try (FullDataSourceV2DTO dto = repo.getByKey(p); FullDataSourceV2 ds = provider.get(p))
			{
				sections.add(new Obj().p("pos", p).p("applyToParent", dto.applyToParent).p("applyToChildren", dto.applyToChildren)
						.p("regenerate", dto.regenerate).p("data", Ds.jsonOfRuns(ds)));
			}
		}
		repo.close();

		return Obj.testCase(name, new Obj().p("maxTasks", maxTasks).p("phases", phaseJson), new Obj().p("phases", phaseOut).p("sections", sections));
	}

	/** every leaf shares the scenario's palette and uniform base column ("fill", like a flat generated world) and has a few special columns */
	private static Ds leaf(Gen g, Ds proto, long pos)
	{
		Ds d = new Ds(pos);
		d.palette.addAll(proto.palette);
		d.fillPts = proto.fillPts;
		d.fillGen = proto.fillGen;
		d.fillComp = proto.fillComp;
		Set<Integer> used = new HashSet<>();
		int cols = 1 + g.r.nextInt(3);
		for (int i = 0; i < cols; i++)
		{
			// bias to 2x2 clusters so the parent merge has something to average
			int base = (g.r.nextInt(32) * 2) * 64 + g.r.nextInt(32) * 2;
			for (int k = 0; k < (g.r.nextBoolean() ? 4 : 1); k++)
			{
				int idx = base + (k / 2) * 64 + (k % 2);
				if (!used.add(idx)) { continue; }
				d.addCol(idx, 9, proto.fillComp, Ds.randomColumn(g, d.palette.size(), 4, 20 + g.r.nextInt(80), g.r.nextInt(4) == 0, d));
			}
		}
		d.cols.sort((x, y) -> Integer.compare(x.idx, y.idx));
		return d;
	}
}
