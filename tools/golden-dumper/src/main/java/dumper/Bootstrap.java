package dumper;

/** Headless setup for DH core classes. See README.md for what had to be bound. */
public final class Bootstrap
{
	private static boolean done = false;

	public static synchronized void init()
	{
		if (done) { return; }
		done = true;
		// Nothing bound yet; filled in below if static initialisers demand it.
	}
}
