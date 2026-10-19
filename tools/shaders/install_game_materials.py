#!/usr/bin/env python3
"""Install or remove DH materials in the game's data/renderer/materials directory.

The compiled material loader reads data/renderer/materials/materials.index.json and the .material.bin files
directly from disk (not through ResourcePackManager, so the MaterialInjector hook never sees them; 2026-10-19).
Until a loader-level hook exists the index is patched on disk, reversibly:

  install <materials_dir> <file.material.bin>...   back up materials.index.json once (.dh-backup), add an entry
                                                   {"name": <stem>, "path": <stem>} per file, copy the files
  restore <materials_dir>                          put the backup back and delete the DH_*.material.bin files

Idempotent: re-running install never duplicates entries or overwrites the backup.
"""
import json
import pathlib
import shutil
import sys


def load_index(p: pathlib.Path):
    return json.loads(p.read_text(encoding="utf-8"))


def install(mdir: pathlib.Path, files: list[pathlib.Path]) -> int:
    index = mdir / "materials.index.json"
    backup = mdir / "materials.index.json.dh-backup"
    if not index.is_file():
        print(f"no index at {index}", file=sys.stderr)
        return 1
    if not backup.exists():
        shutil.copy2(index, backup)
        print(f"backed up {index.name} -> {backup.name}")
    doc = load_index(index)
    mats = doc.setdefault("materials", [])
    names = {m.get("name") for m in mats}
    added = []
    for f in files:
        stem = f.name[: -len(".material.bin")] if f.name.endswith(".material.bin") else f.stem
        shutil.copy2(f, mdir / f.name)
        if stem not in names:
            mats.append({"name": stem, "path": stem})
            names.add(stem)
            added.append(stem)
    index.write_text(json.dumps(doc, indent=3) + "\n", encoding="utf-8")
    print(f"installed {len(files)} file(s) into {mdir}; new index entries: {added or 'none (already present)'}")
    return 0


def restore(mdir: pathlib.Path) -> int:
    index = mdir / "materials.index.json"
    backup = mdir / "materials.index.json.dh-backup"
    if backup.exists():
        shutil.copy2(backup, index)
        backup.unlink()
        print(f"restored {index.name} from backup")
    else:
        print("no backup present; index left untouched")
    removed = 0
    for f in mdir.glob("DH_*.material.bin"):
        f.unlink()
        removed += 1
    print(f"removed {removed} DH_*.material.bin file(s)")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) < 3 or argv[1] not in ("install", "restore"):
        print(__doc__, file=sys.stderr)
        return 2
    mdir = pathlib.Path(argv[2])
    if not mdir.is_dir():
        print(f"not a directory: {mdir}", file=sys.stderr)
        return 1
    if argv[1] == "restore":
        return restore(mdir)
    files = [pathlib.Path(a) for a in argv[3:]]
    if not files or not all(f.is_file() for f in files):
        print("install needs at least one existing .material.bin", file=sys.stderr)
        return 1
    return install(mdir, files)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
