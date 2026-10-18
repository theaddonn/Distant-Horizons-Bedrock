package dumper;

import com.seibel.distanthorizons.core.wrapperInterfaces.block.IBlockStateWrapper;
import com.seibel.distanthorizons.core.wrapperInterfaces.world.IBiomeWrapper;

import java.awt.Color;
import java.util.ArrayList;
import java.util.List;

/** Headless wrapper implementations. Equality/hash are by serial number, like DH's real wrappers (by serial string). */
public final class Wrappers
{
	private Wrappers() { }

	public static final class DumpBlockState implements IBlockStateWrapper
	{
		public final int serial;
		public final boolean isAir, isSolid, isLiquid, isIce;
		public final int opacity, lightEmission;
		public final byte materialId;

		public DumpBlockState(int serial, boolean isAir, int opacity, boolean isSolid, boolean isLiquid, int lightEmission, int materialId, boolean isIce)
		{
			this.serial = serial; this.isAir = isAir; this.opacity = opacity; this.isSolid = isSolid; this.isLiquid = isLiquid;
			this.lightEmission = lightEmission; this.materialId = (byte) materialId; this.isIce = isIce;
		}

		@Override public String getSerialString() { return (this.serial >= 17 && this.serial <= 19) ? "minecraft:snow_STATE_{layers:" + (this.serial - 16) + "}" : "B" + this.serial; }
		@Override public int getOpacity() { return this.opacity; }
		@Override public int getLightEmission() { return this.lightEmission; }
		@Override public byte getMaterialId() { return this.materialId; }
		@Override public boolean isAir() { return this.isAir; }
		@Override public boolean isSolid() { return this.isSolid; }
		@Override public boolean isLiquid() { return this.isLiquid; }
		@Override public boolean isBeaconBlock() { return false; }
		@Override public boolean isBeaconTintBlock() { return false; }
		@Override public boolean allowsBeaconBeamPassage() { return false; }
		@Override public boolean isBeaconBaseBlock() { return false; }
		@Override public boolean isIceBlock() { return this.isIce; }
		@Override public boolean renderTexture() { return true; }
		@Override public boolean useBottomTextureForSides() { return false; }
		@Override public boolean alwaysRasterizeTexture() { return false; }
		@Override public boolean allowApiColorOverride() { return false; }
		@Override public boolean allowApiTextureOverride() { return false; }
		@Override public Color getMapColor() { return Color.MAGENTA; }
		@Override public Color getBeaconTintColor() { return Color.MAGENTA; }
		@Override public Object getWrappedMcObject() { return this; }
		@Override public int hashCode() { return Integer.hashCode(this.serial) * 31 + 7; }
		@Override public boolean equals(Object o) { return o instanceof DumpBlockState && ((DumpBlockState) o).serial == this.serial; }
		@Override public String toString() { return "Block" + this.serial; }
	}

	public static final class DumpBiome implements IBiomeWrapper
	{
		public final int serial;
		public DumpBiome(int serial) { this.serial = serial; }
		@Override public String getName() { return "biome" + this.serial; }
		@Override public String getSerialString() { return "M" + this.serial; }
		@Override public boolean isColdBiome() { return false; }
		@Override public Object getWrappedMcObject() { return this; }
		@Override public int hashCode() { return Integer.hashCode(this.serial) * 17 + 3; }
		@Override public boolean equals(Object o) { return o instanceof DumpBiome && ((DumpBiome) o).serial == this.serial; }
		@Override public String toString() { return "Biome" + this.serial; }
	}

	// ---- the global block table; serial -> attributes. Documented in README and dumped to block_table.gv.json ----

	public static final String[] BLOCK_NAMES = {
			"air", "stone", "dirt", "grass_block", "water", "ice", "glass", "leaves",
			"torch", "snow_layer", "poppy", "glowstone", "lava", "stairs", "deepslate", "cave_air", "sand",
			"snow_layers_1", "snow_layers_2", "snow_layers_3", "snow_block", "lily_pad", "kelp", "seagrass", "barrier" };
	/** resource names used by the CSV lists / deserialisation (serial -> name) */
	public static final String[] RESOURCE_NAMES = {
			"minecraft:air", "minecraft:stone", "minecraft:dirt", "minecraft:grass_block", "minecraft:water", "minecraft:ice", "minecraft:glass", "minecraft:oak_leaves",
			"minecraft:torch", "minecraft:snow_layer", "minecraft:poppy", "minecraft:glowstone", "minecraft:lava", "minecraft:oak_stairs", "minecraft:deepslate", "minecraft:cave_air", "minecraft:sand",
			"minecraft:snow_STATE_{layers:1}", "minecraft:snow_STATE_{layers:2}", "minecraft:snow_STATE_{layers:3}", "minecraft:snow_block", "minecraft:lily_pad", "minecraft:kelp", "minecraft:seagrass", "minecraft:barrier" };
	/** the four renderer CSV lists (stand-ins for the Config strings, which are read through the wrapper factory) */
	public static String ignoredRenderBlockCsv = "", ignoredRenderCaveBlockCsv = "", waterSurfaceBlockReplacementCsv = "", waterSubSurfaceBlockReplacementCsv = "";
	
	/** block by resource name; unknown names give air, like a failed deserialisation */
	public static DumpBlockState byResourceName(String name)
	{
		String n = name.trim();
		for (int i = 0; i < RESOURCE_NAMES.length; i++) { if (RESOURCE_NAMES[i].equals(n)) { return BLOCKS[i]; } }
		return BLOCKS[0];
	}
	
	/** CSV -> block set (as WrapperFactory does: each entry deserialised, unknown -> air) */
	public static it.unimi.dsi.fastutil.objects.ObjectOpenHashSet<com.seibel.distanthorizons.core.wrapperInterfaces.block.IBlockStateWrapper> csvSet(String csv, boolean includeAir)
	{
		it.unimi.dsi.fastutil.objects.ObjectOpenHashSet<com.seibel.distanthorizons.core.wrapperInterfaces.block.IBlockStateWrapper> set = new it.unimi.dsi.fastutil.objects.ObjectOpenHashSet<>();
		if (includeAir) { set.add(BLOCKS[0]); }
		for (String e : csv.split(",")) { if (!e.trim().isEmpty()) { set.add(byResourceName(e)); } }
		if (!includeAir) { set.remove(BLOCKS[0]); }
		return set;
	}
	// serial, isAir, opacity, isSolid, isLiquid, lightEmission, materialId, isIce
	private static final int[][] BLOCK_DEFS = {
			{ 0, 1, 0, 0, 0, 0, 14, 0 },
			{ 1, 0, 16, 1, 0, 0, 2, 0 },
			{ 2, 0, 16, 1, 0, 0, 5, 0 },
			{ 3, 0, 16, 1, 0, 0, 13, 0 },
			{ 4, 0, 3, 0, 1, 0, 12, 0 },
			{ 5, 0, 3, 1, 0, 0, 0, 1 },
			{ 6, 0, 0, 1, 0, 0, 0, 0 },
			{ 7, 0, 1, 1, 0, 0, 1, 0 },
			{ 8, 0, 0, 0, 0, 14, 3, 0 },
			{ 9, 0, 2, 0, 0, 0, 8, 0 },
			{ 10, 0, 0, 0, 0, 0, 13, 0 },
			{ 11, 0, 16, 1, 0, 15, 15, 0 },
			{ 12, 0, 16, 0, 1, 15, 6, 0 },
			{ 13, 0, 16, 0, 0, 0, 3, 0 },
			{ 14, 0, 16, 1, 0, 0, 7, 0 },
			{ 15, 1, 0, 0, 0, 0, 14, 0 },
			{ 16, 0, 16, 1, 0, 0, 9, 0 },
			{ 17, 0, 1, 0, 0, 0, 8, 0 },
			{ 18, 0, 2, 0, 0, 0, 8, 0 },
			{ 19, 0, 3, 0, 0, 0, 8, 0 },
			{ 20, 0, 16, 1, 0, 0, 8, 0 },
			{ 21, 0, 0, 0, 0, 0, 13, 0 },
			{ 22, 0, 1, 0, 0, 0, 13, 0 },
			{ 23, 0, 1, 0, 0, 0, 13, 0 },
			{ 24, 0, 0, 1, 0, 0, 0, 0 },
	};
	public static final int BLOCK_COUNT = BLOCK_DEFS.length;
	/** blocks 0..16 existed in wave 1; older suites keep drawing only from these so their golden files stay byte-identical */
	public static final int CLASSIC_BLOCK_COUNT = 17;
	public static final int BIOME_COUNT = 4;

	private static final DumpBlockState[] BLOCKS = new DumpBlockState[BLOCK_DEFS.length];
	private static final DumpBiome[] BIOMES = new DumpBiome[BIOME_COUNT];
	static
	{
		for (int[] d : BLOCK_DEFS) { BLOCKS[d[0]] = new DumpBlockState(d[0], d[1] != 0, d[2], d[3] != 0, d[4] != 0, d[5], d[6], d[7] != 0); }
		for (int i = 0; i < BIOME_COUNT; i++) { BIOMES[i] = new DumpBiome(i); }
	}

	public static DumpBlockState block(int serial) { return BLOCKS[serial]; }
	public static DumpBiome biome(int serial) { return BIOMES[serial]; }

	public static List<Object> blockTableJson()
	{
		List<Object> l = new ArrayList<>();
		for (DumpBlockState b : BLOCKS)
		{
			l.add(new Obj().p("serial", b.serial).p("name", BLOCK_NAMES[b.serial]).p("resourceName", RESOURCE_NAMES[b.serial]).p("isAir", b.isAir).p("opacity", b.opacity)
					.p("isSolid", b.isSolid).p("isLiquid", b.isLiquid).p("lightEmission", b.lightEmission)
					.p("materialId", (int) b.materialId).p("isIce", b.isIce));
		}
		return l;
	}
}
