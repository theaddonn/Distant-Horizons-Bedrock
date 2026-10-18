package dumper;

import java.util.ArrayList;
import java.util.List;

/**
 * block_table: the headless block/biome table used by every suite that has palettes.
 * out.blocks[] = {serial,name,isAir,opacity(0..16),isSolid,isLiquid,lightEmission,materialId,isIce}; out.biomeCount.
 * Block serial N has getSerialString "B<N>", biome serial N has "M<N>". Wrapper equality is by serial.
 */
public class BlockTableSuite implements Suite
{
	@Override public String name() { return "block_table"; }

	@Override
	public List<Obj> cases()
	{
		List<Obj> l = new ArrayList<>();
		l.add(Obj.testCase("block_table", new Obj(), new Obj().p("blocks", Wrappers.blockTableJson()).p("biomeCount", Wrappers.BIOME_COUNT)));
		return l;
	}
}
