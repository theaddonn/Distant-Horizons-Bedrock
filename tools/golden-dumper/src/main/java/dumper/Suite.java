package dumper;

import java.util.List;

/** One golden-vector file. Adding a suite = one class implementing this + one line in {@link Main}. */
public interface Suite
{
	/** @return the file stem, e.g. "pos" gives tests/golden/pos.gv.json */
	String name();
	/** @return list of {name, in, out} case objects; must be deterministic. */
	List<Obj> cases() throws Exception;
}
