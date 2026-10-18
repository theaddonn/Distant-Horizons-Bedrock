package dumper;

import java.util.LinkedHashMap;
import java.util.concurrent.Callable;

/** Insertion-ordered JSON object builder. */
public final class Obj extends LinkedHashMap<String, Object>
{
	/** Chainable put. */
	public Obj p(String k, Object v) { super.put(k, v); return this; }

	/**
	 * Evaluates {@code f}; on success stores the value, on any Throwable stores {"throws":"<SimpleName>"}.
	 * Lets the C++ side verify that it rejects/asserts on exactly the same inputs.
	 */
	public Obj t(String k, Callable<Object> f)
	{
		super.put(k, attempt(f));
		return this;
	}

	public static Object attempt(Callable<Object> f)
	{
		try { return f.call(); }
		catch (Throwable e) { return new Obj().p("throws", e.getClass().getSimpleName()); }
	}

	/** A test case: {name, in, out}. */
	public static Obj testCase(String name, Obj in, Obj out)
	{
		return new Obj().p("name", name).p("in", in).p("out", out);
	}
}
