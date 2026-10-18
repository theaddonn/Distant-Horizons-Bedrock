package dumper;

import java.util.List;
import java.util.Map;

/**
 * Minimal JSON writer with the golden-vector encoding rules:
 * <ul>
 * <li>{@link Long} / long[] are written as "0x%016x" strings (two's-complement bits).</li>
 * <li>Byte/Short/Integer and their arrays are plain numbers.</li>
 * <li>Double/Float are plain numbers (shortest round-trip repr); NaN/Infinity become strings.</li>
 * <li>byte[] is written as an array of numbers; use {@link #hex(byte[])} for a hex string.</li>
 * </ul>
 */
public final class Json
{
	private Json() { }

	public static String hex64(long v) { return String.format("0x%016x", v); }

	public static String hex(byte[] bytes)
	{
		StringBuilder sb = new StringBuilder();
		for (byte b : bytes) { sb.append(String.format("%02x", b & 0xFF)); }
		return sb.toString();
	}

	public static String toString(Object o)
	{
		StringBuilder sb = new StringBuilder();
		write(sb, o);
		return sb.toString();
	}

	@SuppressWarnings("unchecked")
	public static void write(StringBuilder sb, Object o)
	{
		if (o == null) { sb.append("null"); }
		else if (o instanceof Long) { sb.append('"').append(hex64((Long) o)).append('"'); }
		else if (o instanceof Integer || o instanceof Short || o instanceof Byte) { sb.append(o.toString()); }
		else if (o instanceof Boolean) { sb.append(o.toString()); }
		else if (o instanceof Double || o instanceof Float)
		{
			double d = ((Number) o).doubleValue();
			if (Double.isNaN(d) || Double.isInfinite(d)) { sb.append('"').append(d).append('"'); }
			else { sb.append(o instanceof Float ? Float.toString((Float) o) : Double.toString(d)); }
		}
		else if (o instanceof String || o instanceof Enum) { quote(sb, o instanceof Enum ? ((Enum<?>) o).name() : o.toString()); }
		else if (o instanceof Map)
		{
			sb.append('{');
			boolean first = true;
			for (Map.Entry<String, Object> e : ((Map<String, Object>) o).entrySet())
			{
				if (!first) { sb.append(','); }
				first = false;
				quote(sb, e.getKey());
				sb.append(':');
				write(sb, e.getValue());
			}
			sb.append('}');
		}
		else if (o instanceof List)
		{
			sb.append('[');
			boolean first = true;
			for (Object e : (List<Object>) o)
			{
				if (!first) { sb.append(','); }
				first = false;
				write(sb, e);
			}
			sb.append(']');
		}
		else if (o instanceof long[]) { long[] a = (long[]) o; sb.append('['); for (int i = 0; i < a.length; i++) { if (i > 0) sb.append(','); write(sb, Long.valueOf(a[i])); } sb.append(']'); }
		else if (o instanceof int[]) { int[] a = (int[]) o; sb.append('['); for (int i = 0; i < a.length; i++) { if (i > 0) sb.append(','); sb.append(a[i]); } sb.append(']'); }
		else if (o instanceof short[]) { short[] a = (short[]) o; sb.append('['); for (int i = 0; i < a.length; i++) { if (i > 0) sb.append(','); sb.append(a[i]); } sb.append(']'); }
		else if (o instanceof byte[]) { byte[] a = (byte[]) o; sb.append('['); for (int i = 0; i < a.length; i++) { if (i > 0) sb.append(','); sb.append(a[i]); } sb.append(']'); }
		else if (o instanceof double[]) { double[] a = (double[]) o; sb.append('['); for (int i = 0; i < a.length; i++) { if (i > 0) sb.append(','); write(sb, Double.valueOf(a[i])); } sb.append(']'); }
		else if (o instanceof boolean[]) { boolean[] a = (boolean[]) o; sb.append('['); for (int i = 0; i < a.length; i++) { if (i > 0) sb.append(','); sb.append(a[i]); } sb.append(']'); }
		else if (o instanceof Object[]) { Object[] a = (Object[]) o; sb.append('['); for (int i = 0; i < a.length; i++) { if (i > 0) sb.append(','); write(sb, a[i]); } sb.append(']'); }
		else { throw new IllegalArgumentException("Unsupported JSON type: " + o.getClass()); }
	}

	private static void quote(StringBuilder sb, String s)
	{
		sb.append('"');
		for (int i = 0; i < s.length(); i++)
		{
			char c = s.charAt(i);
			switch (c)
			{
				case '"': sb.append("\\\""); break;
				case '\\': sb.append("\\\\"); break;
				case '\n': sb.append("\\n"); break;
				case '\r': sb.append("\\r"); break;
				case '\t': sb.append("\\t"); break;
				default:
					if (c < 0x20 || c > 0x7e) { sb.append(String.format("\\u%04x", (int) c)); }
					else { sb.append(c); }
			}
		}
		sb.append('"');
	}
}
