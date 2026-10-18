package dumper;

import com.seibel.distanthorizons.core.enums.EDhDirection;
import com.seibel.distanthorizons.core.wrapperInterfaces.world.IClientLevelWrapper;

/**
 * Headless IClientLevelWrapper:
 * getBlockColor(...) = 0xFF000000 | (hashColor(blockSerial, biomeSerial) & 0xFFFFFF)  where
 *   hashColor(b, m) = (b * 0x9E3779B1) ^ (m * 0x85EBCA6B)   (32-bit int arithmetic)
 * getDirtBlockColor() = getBlockColor of block 2 (dirt), biome 0
 * getShade(dir) = vanilla shades: UP 1.0, DOWN 0.5, NORTH/SOUTH 0.8, WEST/EAST 0.6
 */
public final class DumpClientLevel
{
	private DumpClientLevel() { }

	public static int hashColor(int blockSerial, int biomeSerial)
	{
		return 0xFF000000 | (((blockSerial * 0x9E3779B1) ^ (biomeSerial * 0x85EBCA6B)) & 0xFFFFFF);
	}

	public static float shade(EDhDirection d)
	{
		switch (d)
		{
			case UP: return 1.0f;
			case DOWN: return 0.5f;
			case NORTH:
			case SOUTH: return 0.8f;
			default: return 0.6f; // WEST, EAST
		}
	}

	public static IClientLevelWrapper create() { return create(-64, 320, false, false); }
	
	public static IClientLevelWrapper create(int minHeight, int maxHeight, boolean hasCeiling, boolean isEnd)
	{
		com.seibel.distanthorizons.core.wrapperInterfaces.world.IDimensionTypeWrapper dim = Stub.of(com.seibel.distanthorizons.core.wrapperInterfaces.world.IDimensionTypeWrapper.class)
				.val("hasCeiling", hasCeiling).val("isTheEnd", isEnd).build();
		return Stub.of(IClientLevelWrapper.class)
				.val("getMinHeight", minHeight).val("getMaxHeight", maxHeight).val("hasCeiling", hasCeiling).val("getDimensionType", dim)
				.val("getDhIdentifier", "dump").val("hasSkyLight", true)
				.on("getBlockColor", a -> hashColor(((Wrappers.DumpBlockState) a[3]).serial, ((Wrappers.DumpBiome) a[1]).serial))
				.on("getDirtBlockColor", a -> hashColor(2, 0))
				.on("getShade", a -> shade((EDhDirection) a[0]))
				.build();
	}
}
