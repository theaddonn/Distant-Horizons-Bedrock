# Building

Three independent builds: the client DLL, the server DLL (both cross-compiled from Linux to Windows x64 with clang-cl), and the Linux host unit tests for `dh_core`.

## Prerequisites

- xmake, clang-cl toolchain and Windows sysroot under `~/.local/share/amtool/` (`bin/`, `winsysroot/`); xmake itself may be in `~/.local/bin`.
- For host tests: `clang-22` / `clang++-22`.

## Client / server DLL

```
scripts/build.sh client    # -> bin/DistantHorizons/DistantHorizons.dll
scripts/build.sh server    # -> bin/DistantHorizonsServer/DistantHorizonsServer.dll
```

The script runs `xmake f -P . -p windows -a x64 -m release --target_type=<client|server> --toolchain=clang-cl --sdk=~/.local/share/amtool/winsysroot -y` followed by `xmake -P . -y`, exits non-zero on failure and prints the output directory (DLL, PDB, `manifest.json`).

Client and server are two separate configurations of the root project; never configure both at once. After building one, the next `scripts/build.sh` run reconfigures. Leave the root config on `client` when you are done.

Server target status: the `levilamina` server package and `bedrockdata` server download and configure fine, and `DistantHorizonsServer.dll` builds and links (verified with LeviLamina 26.51.6).

Known harmless warning: `cannot match add_files("src/bedrock_common/**.cpp")` until `src/bedrock_common` has sources.

## Host unit tests (dh_core, Linux, ASan + UBSan)

```
scripts/test-host.sh [doctest args]
```

Equivalent to:

```
cd tests
xmake f -P . -p linux -a x86_64 -m debug --cc=clang-22 --cxx=clang++-22 --ld=clang++-22 -y
xmake -P . -y
xmake run -P . dh_core_tests
```

### The `-P .` gotcha

`tests/` is a separate xmake project, but xmake searches upward for the project root. Without `-P .` it picks the repository-root `xmake.lua` (the Windows cross-build) instead. Always pass `-P .` for every `xmake` invocation inside `tests/`. The same applies to `scripts/build.sh` when the repo sits inside another xmake project's directory tree (for example a git worktree under `.claude/worktrees/`), so that script also passes `-P .`.

Tests run with `$(projectdir)` (= `tests/`) as working directory, so golden files resolve as `golden/<suite>.gv.json`.

## Golden vectors

`tests/golden_loader.h` loads `tests/golden/<suite>.gv.json`. Use `DH_GOLDEN_SUITE_OR_SKIP(s, "suite")` at the top of a test case so suites that have not been dumped yet are skipped instead of failing.

The `propagate` replay skips its slow cases (all `*_k8_*` and a few others, ~60 s under ASan) by default; run the full set with `DH_GOLDEN_SLOW=1 scripts/test-host.sh -tc="*propagate*"`.

## Vendored dependencies

Header-only dependencies of `dh_core` live in `src/core/third_party/` (see `THIRD_PARTY.md` there) and are a public include dir of `dh_core`, so both the Windows and host builds use the same headers.

## Packaging and release

```
scripts/check-version.sh                 # xmake.lua, tooth.json, Version.h, core Version.cpp, CHANGELOG agree
scripts/package.sh client|server <out>   # DistantHorizons-<client|server>-<version>.zip from bin/ + dist/ + data/
```

The version lives in `xmake.lua` (`DH_VERSION`); change it there and in `tooth.json`, `src/client/Version.h` (fallback) and
`src/core/dh/Version.cpp`, then add a CHANGELOG heading. `package.sh` needs `zip` and a prior `scripts/build.sh` of the
target. The LOD shader `dist/materials/DH_Terrain.material.bin` is prebuilt and committed because CI cannot build it; rebuild
it when `tools/shaders/src/` changes (`dist/README.md`). CI (`.github/workflows/build.yml`) builds both DLLs, runs the host tests
and uploads the packaged zips; pushing a tag `vX.Y.Z` (`release.yml`) attaches both zips to the GitHub release.

## Artifacts

- `bin/DistantHorizons/` and `bin/DistantHorizonsServer/` (mod DLL, PDB, manifest).
- `build/` per-configuration object output; `tests/build/` for host tests.
