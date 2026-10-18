package dumper;

import com.seibel.distanthorizons.core.dependencyInjection.SingletonInjector;
import com.seibel.distanthorizons.core.wrapperInterfaces.IWrapperFactory;
import com.seibel.distanthorizons.core.wrapperInterfaces.block.IBlockStateWrapper;
import com.seibel.distanthorizons.core.wrapperInterfaces.world.IBiomeWrapper;

import java.io.IOException;

/**
 * Headless setup for DH core classes. See README.md ("Bindings") for what is bound and why.
 * Safe to call from any suite; runs once.
 */
public final class Bootstrap
{
	private static boolean done = false;

	public static synchronized void init()
	{
		if (done) { return; }
		done = true;

		// IWrapperFactory: FullDataPointIdMap and LodDataBuilder read it in static initialisers
		// (air block, water block, (de)serialising palette entries).
		IWrapperFactory factory = Stub.of(IWrapperFactory.class)
				.on("getAirBlockStateWrapper", a -> Wrappers.block(0))
				.on("getWaterBlockStateWrapper", a -> Wrappers.block(4))
				.on("getPlainsBiomeWrapper", a -> Wrappers.biome(0))
				.on("deserializeBlockStateWrapper", a -> parseBlock((String) a[0]))
				.on("deserializeBiomeWrapper", a -> parseBiome((String) a[0]))
				.build();
		SingletonInjector.INSTANCE.bind(IWrapperFactory.class, factory);

		Extra.bindAll();
	}

	private static IBlockStateWrapper parseBlock(String s)
	{
		if (s.startsWith("B")) { return Wrappers.block(Integer.parseInt(s.substring(1))); }
		throw new IllegalArgumentException("bad block serial " + s);
	}

	private static IBiomeWrapper parseBiome(String s)
	{
		if (s.startsWith("M")) { return Wrappers.biome(Integer.parseInt(s.substring(1))); }
		throw new IllegalArgumentException("bad biome serial " + s);
	}
}
