package dumper;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.util.HashMap;
import java.util.Map;
import java.util.function.Function;

/**
 * Dynamic-proxy stub for large wrapper interfaces. Unimplemented methods return a zero value
 * (0 / false / null / empty string), so only the methods the code under test calls need an implementation.
 * Default methods of the interface are invoked for real unless overridden by name.
 */
public final class Stub
{
	private Stub() { }

	public static final class Builder<T>
	{
		private final Class<T> iface;
		private final Map<String, Function<Object[], Object>> impl = new HashMap<>();
		Builder(Class<T> iface) { this.iface = iface; }

		/** Implements every overload with this name. */
		public Builder<T> on(String method, Function<Object[], Object> f) { this.impl.put(method, f); return this; }
		public Builder<T> val(String method, Object v) { this.impl.put(method, a -> v); return this; }

		@SuppressWarnings("unchecked")
		public T build()
		{
			InvocationHandler h = (proxy, m, args) ->
			{
				Function<Object[], Object> f = this.impl.get(m.getName());
				if (f != null) { return f.apply(args == null ? new Object[0] : args); }
				if (m.getDeclaringClass() == Object.class)
				{
					switch (m.getName())
					{
						case "hashCode": return System.identityHashCode(proxy);
						case "equals": return proxy == args[0];
						default: return this.iface.getSimpleName() + "@stub";
					}
				}
				if (m.isDefault())
				{
					return java.lang.invoke.MethodHandles.privateLookupIn(m.getDeclaringClass(), java.lang.invoke.MethodHandles.lookup())
							.unreflectSpecial(m, m.getDeclaringClass()).bindTo(proxy).invokeWithArguments(args == null ? new Object[0] : args);
				}
				return zero(m.getReturnType());
			};
			return (T) Proxy.newProxyInstance(this.iface.getClassLoader(), new Class<?>[] { this.iface }, h);
		}
	}

	public static <T> Builder<T> of(Class<T> iface) { return new Builder<>(iface); }

	static Object zero(Class<?> t)
	{
		if (t == boolean.class) return false;
		if (t == int.class) return 0;
		if (t == long.class) return 0L;
		if (t == byte.class) return (byte) 0;
		if (t == short.class) return (short) 0;
		if (t == float.class) return 0f;
		if (t == double.class) return 0.0;
		if (t == char.class) return (char) 0;
		if (t == String.class) return "";
		return null;
	}
}
