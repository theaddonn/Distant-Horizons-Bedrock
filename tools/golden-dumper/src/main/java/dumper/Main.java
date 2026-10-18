package dumper;

import java.io.BufferedWriter;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class Main
{
	public static final String JAVA_COMMIT = "2206fe7a4";
	public static final String GENERATOR = "golden-dumper 0.1";

	private static final Map<String, Suite> REGISTRY = new LinkedHashMap<>();
	private static void reg(Suite s) { REGISTRY.put(s.name(), s); }

	static
	{
		// One line per suite (sharded suites register several files).
		for (int i = 0; i < PosSuite.SHARDS; i++) { reg(new PosSuite(i)); }
		reg(new PosMiscSuite());
		for (int i = 0; i < DatapointSuite.SHARDS; i++) { reg(new DatapointSuite(i)); }
		for (int i = 0; i < RdpSuite.SHARDS; i++) { reg(new RdpSuite(i)); }
		reg(new ConfigSuite());
		reg(new BlockTableSuite());
		reg(new MergeSuite(MergeSuite.Mode.SAME, 1000));
		reg(new MergeSuite(MergeSuite.Mode.UP, 1000));
		reg(new MergeSuite(MergeSuite.Mode.DOWN, 600));
		reg(new ChunkSuite());
		reg(new CullSuite());
		reg(new LightDsSuite());
		reg(new ReduceSuite());
		reg(new QuadSuite());
		reg(new LightChunkSuite());
		reg(new TreeMathSuite());
		reg(new NetSuite());
		reg(new GenPosSuite());
		reg(new F2rSuite());
		reg(new MeshSuite());
		reg(new PropagateSuite());
	}

	public static void main(String[] args) throws Exception
	{
		File outDir = new File(System.getProperty("golden.out", "tests/golden"));
		outDir.mkdirs();
		Bootstrap.init();

		List<String> names = args.length == 0 ? new ArrayList<>(REGISTRY.keySet()) : expand(args);
		for (String n : names)
		{
			Suite s = REGISTRY.get(n);
			if (s == null) { System.err.println("unknown suite: " + n + " (have " + REGISTRY.keySet() + ")"); System.exit(2); }
			long t0 = System.currentTimeMillis();
			List<Obj> cases = s.cases();
			File f = new File(outDir, s.name() + ".gv.json");
			try (BufferedWriter w = Files.newBufferedWriter(f.toPath(), StandardCharsets.UTF_8))
			{
				w.write("{\"suite\":" + Json.toString(s.name()) + ",\"javaCommit\":\"" + JAVA_COMMIT + "\",\"generator\":\"" + GENERATOR + "\",\"cases\":[\n");
				for (int i = 0; i < cases.size(); i++)
				{
					w.write(Json.toString(cases.get(i)));
					w.write(i + 1 < cases.size() ? ",\n" : "\n");
				}
				w.write("]}\n");
			}
			System.out.println(s.name() + ": " + cases.size() + " cases, " + f.length() + " bytes, " + (System.currentTimeMillis() - t0) + " ms");
		}
		System.exit(0); // config listener timers are non-daemon
	}

	/** "pos" expands to its shards (pos, pos_1, ...) so `run.sh pos` regenerates all of them. */
	private static List<String> expand(String[] args)
	{
		List<String> out = new ArrayList<>();
		for (String a : args)
		{
			boolean any = false;
			for (String k : REGISTRY.keySet())
			{
				if (k.equals(a) || k.matches(java.util.regex.Pattern.quote(a) + "_\\d+")) { out.add(k); any = true; }
			}
			if (!any) { out.add(a); }
		}
		return out;
	}
}
