package dumper;

import com.seibel.distanthorizons.api.enums.config.EDhApiDataCompressionMode;
import com.seibel.distanthorizons.api.enums.config.EDhApiHorizontalQuality;
import com.seibel.distanthorizons.api.enums.config.EDhApiMaxHorizontalResolution;
import com.seibel.distanthorizons.api.enums.config.EDhApiVerticalQuality;
import com.seibel.distanthorizons.api.enums.config.EDhApiWorldCompressionMode;
import com.seibel.distanthorizons.api.enums.rendering.EDhApiBlockMaterial;
import com.seibel.distanthorizons.api.enums.worldGeneration.EDhApiDistantGeneratorMode;
import com.seibel.distanthorizons.api.enums.worldGeneration.EDhApiGeneratorPlan;
import com.seibel.distanthorizons.api.enums.worldGeneration.EDhApiWorldGenerationStep;

import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.List;

/**
 * Reflective dump of config enums: one case per enum, out = {constants:[{name, ordinal, <every instance field>}],
 * methods:{...}}. Doubles also get a "<field>_bits" hex entry (exact IEEE-754 bits).
 */
public class ConfigSuite implements Suite
{
	@Override public String name() { return "config"; }

	private static final Class<?>[] ENUMS = {
			EDhApiVerticalQuality.class, EDhApiHorizontalQuality.class, EDhApiMaxHorizontalResolution.class,
			EDhApiWorldGenerationStep.class, EDhApiBlockMaterial.class, EDhApiDistantGeneratorMode.class,
			EDhApiGeneratorPlan.class, EDhApiDataCompressionMode.class, EDhApiWorldCompressionMode.class,
	};

	@Override
	public List<Obj> cases() throws Exception
	{
		List<Obj> out = new ArrayList<>();
		for (Class<?> c : ENUMS)
		{
			Object[] constants = c.getEnumConstants();
			List<Object> list = new ArrayList<>();
			for (Object k : constants)
			{
				Obj o = new Obj().p("name", ((Enum<?>) k).name()).p("ordinal", ((Enum<?>) k).ordinal());
				for (Field f : c.getDeclaredFields())
				{
					if (Modifier.isStatic(f.getModifiers()) || f.isSynthetic()) { continue; }
					f.setAccessible(true);
					putValue(o, f.getName(), f.get(k));
				}
				list.add(o);
			}
			Obj methods = new Obj();
			methods(c, methods);
			out.add(Obj.testCase(c.getSimpleName(), new Obj().p("enum", c.getSimpleName()),
					new Obj().p("count", constants.length).p("constants", list).p("methods", methods)));
		}
		return out;
	}

	private static void putValue(Obj o, String name, Object v)
	{
		if (v instanceof Double) { o.p(name, v).p(name + "_bits", Double.doubleToRawLongBits((Double) v)); }
		else if (v != null && v.getClass().isArray())
		{
			List<Object> l = new ArrayList<>();
			for (int i = 0; i < Array.getLength(v); i++) { l.add(Array.get(v, i)); }
			o.p(name, l);
		}
		else if (v instanceof Enum) { o.p(name, ((Enum<?>) v).name()); }
		else { o.p(name, v); }
	}

	/** Results of the enums' public static/instance helper methods over their input domain. */
	private static void methods(Class<?> c, Obj m)
	{
		if (c == EDhApiVerticalQuality.class)
		{
			for (EDhApiVerticalQuality q : EDhApiVerticalQuality.values())
			{
				List<Object> l = new ArrayList<>();
				for (int d = -2; d <= 14; d++) { l.add(q.calculateMaxNumberOfVerticalSlicesAtDetailLevel((byte) d)); }
				m.p("calculateMaxNumberOfVerticalSlicesAtDetailLevel_" + q.name() + "_detail_-2_to_14", l);
			}
		}
		else if (c == EDhApiMaxHorizontalResolution.class)
		{
			for (EDhApiMaxHorizontalResolution r : EDhApiMaxHorizontalResolution.values())
			{
				List<Object> l = new ArrayList<>();
				for (EDhApiMaxHorizontalResolution x : EDhApiMaxHorizontalResolution.getSelfAndLowerDetails(r)) { l.add(x.name()); }
				m.p("getSelfAndLowerDetails_" + r.name(), l);
			}
		}
		else if (c == EDhApiWorldGenerationStep.class)
		{
			List<Object> l = new ArrayList<>();
			for (int v = -3; v <= 11; v++)
			{
				final int vv = v;
				l.add(Obj.attempt(() -> { EDhApiWorldGenerationStep s = EDhApiWorldGenerationStep.fromValue(vv); return s == null ? null : s.name(); }));
			}
			m.p("fromValue_-3_to_11", l);
		}
		else if (c == EDhApiBlockMaterial.class)
		{
			List<Object> l = new ArrayList<>();
			for (int v = -1; v <= 17; v++)
			{
				final int vv = v;
				l.add(Obj.attempt(() -> EDhApiBlockMaterial.getFromIndex(vv).name()));
			}
			m.p("getFromIndex_-1_to_17", l);
		}
		else if (c == EDhApiDataCompressionMode.class)
		{
			List<Object> l = new ArrayList<>();
			for (int v = -1; v <= 5; v++)
			{
				final int vv = v;
				l.add(Obj.attempt(() -> EDhApiDataCompressionMode.getFromValue((byte) vv).name()));
			}
			m.p("getFromValue_-1_to_5", l);
		}
		else if (c == EDhApiWorldCompressionMode.class)
		{
			List<Object> l = new ArrayList<>();
			for (int v = -1; v <= 2; v++)
			{
				final int vv = v;
				l.add(Obj.attempt(() -> EDhApiWorldCompressionMode.getFromValue((byte) vv).name()));
			}
			m.p("getFromValue_-1_to_2", l);
		}
	}
}
