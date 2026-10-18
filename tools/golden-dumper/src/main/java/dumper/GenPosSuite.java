package dumper;

import com.seibel.distanthorizons.api.enums.worldGeneration.EDhApiGeneratorPlan;
import com.seibel.distanthorizons.core.config.Config;
import com.seibel.distanthorizons.core.file.fullDatafile.GeneratedFullDataSourceProvider;
import com.seibel.distanthorizons.core.generation.queues.IFullDataSourceRetrievalQueue;
import com.seibel.distanthorizons.core.pos.DhSectionPos;
import com.seibel.distanthorizons.core.sql.repo.FullDataSourceV2Repo;
import it.unimi.dsi.fastutil.bytes.ByteArrayList;
import it.unimi.dsi.fastutil.longs.LongArrayList;

import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicReference;

/**
 * genpos: GeneratedFullDataSourceProvider.getPositionsToRetrieve(pos, generatorDetailLevel) against a stubbed repo
 * (the provider is instantiated without its constructor; `repo.existsWithKey` / `repo.getColumnGenerationStepForPos` are answered from `in.repo`).
 * in : {"plan":EDhApiGeneratorPlan ordinal (0 SURFACE_THEN_CHUNKS,1 SURFACE_ONLY,2 CHUNKS_ONLY,3 DISABLED),
 *       "pos":long section pos,"generatorDetailLevel":int (absolute section detail level, <= detail of pos),"hasQueue":bool (worldGenQueueRef != null),
 *       "repo":[{"pos":long,"steps":[[genStep,count],...] run-length over the 4096 columns, index relX*64+relZ; [] = row exists but has no column data}]}
 *       Positions not listed in "repo" do not exist.
 * out: {"positions":[longs]} in the order produced (child order of DhSectionPos.forEachChildAtDetailLevel), or {"positions":null} when hasQueue=false.
 */
public class GenPosSuite implements Suite
{
	@Override public String name() { return "genpos"; }

	static class StubRepo extends FullDataSourceV2Repo
	{
		Map<Long, byte[]> rows = new HashMap<>();
		StubRepo() throws Exception { super(null, null); }
		@Override public boolean existsWithKey(Long key) { return this.rows.containsKey(key); }
		@Override public void getColumnGenerationStepForPos(long pos, ByteArrayList out)
		{
			byte[] a = this.rows.get(pos);
			out.clear();
			if (a != null) { out.addElements(0, a); }
		}
	}

	private static final int[] STEPS = { -1, 0, 1, 2, 3, 4, 5, 5, 6, 7, 8, 8, 9, 9, 9 };

	@Override
	public List<Obj> cases() throws Exception
	{
		Field uf = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
		uf.setAccessible(true);
		sun.misc.Unsafe unsafe = (sun.misc.Unsafe) uf.get(null);
		Field repoField = com.seibel.distanthorizons.core.file.fullDatafile.V2.FullDataSourceProviderV2.class.getField("repo");
		repoField.setAccessible(true);
		Field queueField = GeneratedFullDataSourceProvider.class.getField("worldGenQueueRef");
		queueField.setAccessible(true);
		IFullDataSourceRetrievalQueue queue = Stub.of(IFullDataSourceRetrievalQueue.class).build();

		Gen g = new Gen(0x6E4905L);
		List<Obj> out = new ArrayList<>();
		for (int n = 0; n < 400; n++)
		{
			int plan = g.r.nextInt(4);
			int detail = 6 + g.r.nextInt(6);
			int gd = Math.max(6, detail - g.r.nextInt(4));
			long pos = DhSectionPos.encode((byte) detail, g.pos28() >> 8, g.pos28() >> 8);
			boolean hasQueue = g.r.nextInt(15) != 0;

			List<Long> candidates = new ArrayList<>();
			candidates.add(pos);
			DhSectionPos.forEachChildAtDetailLevel(pos, (byte) gd, candidates::add);

			StubRepo repo = (StubRepo) unsafe.allocateInstance(StubRepo.class);
			repo.rows = new HashMap<>();
			List<Object> repoJson = new ArrayList<>();
			int existPct = new int[] { 0, 25, 60, 90, 100 }[g.r.nextInt(5)];
			// "uniform" rows are by far the most common in real data, so bias to a single gen step with small deviations
			for (Long c : candidates)
			{
				if (g.r.nextInt(100) >= existPct) { continue; }
				byte[] steps = new byte[Ds.W * Ds.W];
				List<Object> runs = new ArrayList<>();
				if (g.r.nextInt(25) == 0)
				{
					steps = new byte[0];
				}
				else
				{
					int base = STEPS[g.r.nextInt(STEPS.length)];
					Arrays.fill(steps, (byte) base);
					int deviations = g.r.nextInt(3) == 0 ? 1 + g.r.nextInt(3) : 0;
					for (int d = 0; d < deviations; d++)
					{
						int start = g.r.nextInt(steps.length), len = 1 + g.r.nextInt(300);
						byte v = (byte) STEPS[g.r.nextInt(STEPS.length)];
						for (int i = start; i < Math.min(steps.length, start + len); i++) { steps[i] = v; }
					}
					int i = 0;
					while (i < steps.length)
					{
						int j = i;
						while (j < steps.length && steps[j] == steps[i]) { j++; }
						runs.add(Arrays.asList((int) steps[i], j - i));
						i = j;
					}
				}
				repo.rows.put(c, steps);
				repoJson.add(new Obj().p("pos", c).p("steps", runs));
			}

			GeneratedFullDataSourceProvider provider = (GeneratedFullDataSourceProvider) unsafe.allocateInstance(GeneratedFullDataSourceProvider.class);
			repoField.set(provider, repo);
			queueField.set(provider, new AtomicReference<IFullDataSourceRetrievalQueue>(hasQueue ? queue : null));
			Config.Common.WorldGenerator.generatorPlan.set(EDhApiGeneratorPlan.values()[plan]);

			LongArrayList result = provider.getPositionsToRetrieve(pos, (byte) gd);
			Object positions = result == null ? null : result.toLongArray();
			out.add(Obj.testCase("genpos_" + n,
					new Obj().p("plan", plan).p("pos", pos).p("generatorDetailLevel", gd).p("hasQueue", hasQueue).p("repo", repoJson),
					new Obj().p("positions", positions)));
		}
		return out;
	}
}
