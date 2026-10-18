package dumper;

import com.seibel.distanthorizons.core.pos.DhChunkPos;
import com.seibel.distanthorizons.core.pos.blockPos.DhBlockPos;
import com.seibel.distanthorizons.core.util.LodUtil;
import com.seibel.distanthorizons.core.wrapperInterfaces.block.IBlockStateWrapper;
import com.seibel.distanthorizons.core.wrapperInterfaces.chunk.ChunkLightStorage;
import com.seibel.distanthorizons.core.wrapperInterfaces.chunk.IChunkWrapper;
import com.seibel.distanthorizons.core.wrapperInterfaces.misc.IMutableBlockPosWrapper;
import com.seibel.distanthorizons.core.wrapperInterfaces.world.IBiomeWrapper;
import com.seibel.distanthorizons.core.wrapperInterfaces.world.ILevelWrapper;

import java.util.ArrayList;

/**
 * Headless IChunkWrapper over a dense 16 x H x 16 array of block serials, mirroring the semantics of
 * common/.../wrappers/chunk/ChunkWrapper.java:
 * - section emptiness (16-high sections from minY, "hasOnlyAir") defines getMinNonEmptyHeight / getMaxNonEmptyHeight
 * - createDhHeightMaps(): scan down from getMaxNonEmptyHeight(); solid = first isSolid, lightBlocking = first opacity != 0
 *   (value is the world Y of that block; stays at minNonEmptyHeight if none found)
 * - getBlockState outside [minY,maxY) is air; getBiome clamps to the build height (4x4x4 biome cells, like vanilla)
 * - light via the real ChunkLightStorage (sky: above max = 15, below min = 0; block: 0 / 0)
 */
public class DumpChunk implements IChunkWrapper
{
	public final int chunkX, chunkZ, minY, maxY;
	/** index = relativeBlockPosToIndex(x, y, z) */
	public final int[] blocks;
	/** [qx*4+qz][qy] biome serial, qy = (y - minY) >> 2 */
	public final int[][] biomes;

	private final DhChunkPos pos;
	private final ChunkLightStorage skyLight, blockLight;
	private boolean skyCorrect, blockCorrect;
	private int[][] solidHeightMap, lightBlockingHeightMap;
	private Integer minNonEmpty, maxNonEmpty;
	private ArrayList<DhBlockPos> lightPosList;

	public DumpChunk(int chunkX, int chunkZ, int minY, int maxY)
	{
		this.chunkX = chunkX; this.chunkZ = chunkZ; this.minY = minY; this.maxY = maxY;
		this.pos = new DhChunkPos(chunkX, chunkZ);
		this.blocks = new int[16 * (maxY - minY) * 16];
		this.biomes = new int[16][(maxY - minY) >> 2];
		this.skyLight = ChunkLightStorage.createSkyLightStorage(minY, maxY);
		this.blockLight = ChunkLightStorage.createBlockLightStorage(minY, maxY);
	}

	public void setBlock(int x, int y, int z, int serial) { this.blocks[this.relativeBlockPosToIndex(x, y, z)] = serial; }
	public int blockSerial(int x, int y, int z) { return this.blocks[this.relativeBlockPosToIndex(x, y, z)]; }

	@Override public DhChunkPos getChunkPos() { return this.pos; }
	@Override public int getInclusiveMinBuildHeight() { return this.minY; }
	@Override public int getExclusiveMaxBuildHeight() { return this.maxY; }

	private boolean sectionEmpty(int sectionIndex)
	{
		int y0 = this.minY + sectionIndex * 16;
		for (int z = 0; z < 16; z++)
		{
			for (int y = y0; y < y0 + 16; y++)
			{
				for (int x = 0; x < 16; x++)
				{
					if (!Wrappers.block(this.blocks[this.relativeBlockPosToIndex(x, y, z)]).isAir) { return false; }
				}
			}
		}
		return true;
	}

	@Override
	public int getMinNonEmptyHeight()
	{
		if (this.minNonEmpty == null)
		{
			int v = this.minY;
			int sections = (this.maxY - this.minY) / 16;
			for (int i = 0; i < sections; i++)
			{
				if (!this.sectionEmpty(i)) { v = this.minY + i * 16; break; }
			}
			this.minNonEmpty = v;
		}
		return this.minNonEmpty;
	}

	@Override
	public int getMaxNonEmptyHeight()
	{
		if (this.maxNonEmpty == null)
		{
			int v = this.maxY;
			int sections = (this.maxY - this.minY) / 16;
			for (int i = sections - 1; i >= 0; i--)
			{
				v = this.minY + i * 16 + 16;
				if (!this.sectionEmpty(i)) { break; }
			}
			this.maxNonEmpty = v;
		}
		return this.maxNonEmpty;
	}

	@Override
	public void createDhHeightMaps()
	{
		this.solidHeightMap = new int[16][16];
		this.lightBlockingHeightMap = new int[16][16];
		for (int x = 0; x < 16; x++)
		{
			for (int z = 0; z < 16; z++)
			{
				int minInclusive = this.getMinNonEmptyHeight();
				int solidHeight = minInclusive;
				int lightBlockingHeight = minInclusive;
				int y = this.getMaxNonEmptyHeight();
				IBlockStateWrapper block = this.getBlockState(x, y, z);
				while (y > minInclusive && (solidHeight == minInclusive || lightBlockingHeight == minInclusive))
				{
					if (solidHeight == minInclusive && block.isSolid()) { solidHeight = y; }
					if (lightBlockingHeight == minInclusive && block.getOpacity() != LodUtil.BLOCK_FULLY_TRANSPARENT) { lightBlockingHeight = y; }
					y--;
					block = this.getBlockState(x, y, z);
				}
				this.solidHeightMap[x][z] = solidHeight;
				this.lightBlockingHeightMap[x][z] = lightBlockingHeight;
			}
		}
	}

	@Override
	public int getSolidHeightMapValue(int xRel, int zRel)
	{
		if (this.solidHeightMap == null) { throw new IllegalStateException("height maps not created (vanilla fallback not modelled)"); }
		return this.solidHeightMap[xRel][zRel];
	}

	@Override
	public int getLightBlockingHeightMapValue(int xRel, int zRel)
	{
		if (this.lightBlockingHeightMap == null) { throw new IllegalStateException("height maps not created (vanilla fallback not modelled)"); }
		return this.lightBlockingHeightMap[xRel][zRel];
	}

	@Override public int getMinBlockX() { return this.pos.getMinBlockX(); }
	@Override public int getMinBlockZ() { return this.pos.getMinBlockZ(); }

	@Override public void setIsDhBlockLightCorrect(boolean v) { this.blockCorrect = v; }
	@Override public void setIsDhSkyLightCorrect(boolean v) { this.skyCorrect = v; }
	@Override public boolean isDhBlockLightingCorrect() { return this.blockCorrect; }
	@Override public boolean isDhSkyLightCorrect() { return this.skyCorrect; }

	@Override public int getDhSkyLight(int x, int y, int z) { return this.skyLight.get(x, y, z); }
	@Override public void setDhSkyLight(int x, int y, int z, int v) { this.skyLight.set(x, y, z, v); }
	@Override public void clearDhSkyLighting() { this.skyLight.clear(); }
	@Override public int getDhBlockLight(int x, int y, int z) { return this.blockLight.get(x, y, z); }
	@Override public void setDhBlockLight(int x, int y, int z, int v) { this.blockLight.set(x, y, z, v); }
	@Override public void clearDhBlockLighting() { this.blockLight.clear(); }

	@Override
	public ArrayList<DhBlockPos> getWorldBlockLightPosList()
	{
		if (this.lightPosList == null)
		{
			this.lightPosList = new ArrayList<>();
			for (int y = this.minY; y < this.maxY; y++)
			{
				for (int x = 0; x < 16; x++)
				{
					for (int z = 0; z < 16; z++)
					{
						if (Wrappers.block(this.blocks[this.relativeBlockPosToIndex(x, y, z)]).lightEmission > 0)
						{
							this.lightPosList.add(new DhBlockPos(this.getMinBlockX() + x, y, this.getMinBlockZ() + z));
						}
					}
				}
			}
		}
		return this.lightPosList;
	}

	@Override
	public IBlockStateWrapper getBlockState(int x, int y, int z)
	{
		if (y < this.minY || y >= this.maxY) { return Wrappers.block(0); }
		return Wrappers.block(this.blocks[this.relativeBlockPosToIndex(x, y, z)]);
	}

	@Override
	public IBlockStateWrapper getBlockState(int x, int y, int z, IMutableBlockPosWrapper mcBlockPos, IBlockStateWrapper guess)
	{ return this.getBlockState(x, y, z); }

	@Override
	public IMutableBlockPosWrapper getMutableBlockPosWrapper()
	{ return Stub.of(IMutableBlockPosWrapper.class).build(); }

	@Override
	public IBiomeWrapper getBiome(int x, int y, int z)
	{
		int qy = (y - this.minY) >> 2;
		qy = Math.max(0, Math.min(qy, ((this.maxY - this.minY) >> 2) - 1));
		return Wrappers.biome(this.biomes[(x >> 2) * 4 + (z >> 2)][qy]);
	}

	@Override public IChunkWrapper copy() { return this; }
	@Override public IChunkWrapper copyWithLevel(ILevelWrapper levelWrapper) { return this; }
	@Override public String toString() { return "DumpChunk[" + this.chunkX + "," + this.chunkZ + "]"; }
}
