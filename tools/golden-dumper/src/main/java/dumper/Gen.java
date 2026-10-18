package dumper;

import java.util.Random;

/** Seeded random helpers shared by suites. */
public final class Gen
{
	public final Random r;
	public Gen(long seed) { this.r = new Random(seed); }

	public static final int POS_MIN = -(1 << 27);
	public static final int POS_MAX = (1 << 27) - 1;
	private static final int[] POS_EXTREMES = { POS_MIN, POS_MAX, POS_MIN + 1, POS_MAX - 1, 0, 1, -1, 63, 64, -63, -64, 2, -2 };

	/** signed 28-bit value, ~25% extremes, ~20% small, rest uniform */
	public int pos28()
	{
		int k = r.nextInt(100);
		if (k < 25) { return POS_EXTREMES[r.nextInt(POS_EXTREMES.length)]; }
		if (k < 45) { return r.nextInt(2001) - 1000; }
		return r.nextInt(1 << 28) - (1 << 27);
	}

	/** any int, biased to extremes / small values */
	public int anyInt()
	{
		int k = r.nextInt(100);
		if (k < 10) { return new int[] { Integer.MIN_VALUE, Integer.MAX_VALUE, 0, -1, 1, Integer.MIN_VALUE + 1, Integer.MAX_VALUE - 1 }[r.nextInt(7)]; }
		if (k < 35) { return r.nextInt(4001) - 2000; }
		if (k < 60) { return pos28(); }
		return r.nextInt();
	}

	public long anyLong()
	{
		int k = r.nextInt(100);
		if (k < 8) { return new long[] { 0L, -1L, Long.MIN_VALUE, Long.MAX_VALUE, 1L }[r.nextInt(5)]; }
		return r.nextLong();
	}

	/** detail level 0..127: ~60% realistic (0..24), rest full range (overflow behaviour) */
	public byte detail() { return (byte) (r.nextInt(10) < 6 ? r.nextInt(25) : r.nextInt(128)); }
}
