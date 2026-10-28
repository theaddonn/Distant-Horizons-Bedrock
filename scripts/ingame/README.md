# In-game harness

Build (from the repo/worktree root; `-P .` matters inside a git worktree nested in another xmake project):

    export PATH=~/.local/share/amtool/bin:~/.local/bin:$PATH
    xmake f -P . -p windows -a x64 -m release --target_type=client --toolchain=clang-cl --sdk=~/.local/share/amtool/winsysroot -y
    xmake -P . -y

Scripts (all in `scripts/`):

| script | what |
|---|---|
| `deploy.sh client` | copies `bin/DistantHorizons/*` into `$GDK/builds/Retail-1.26.5101-Levi/mods/DistantHorizons/`, prints/compares sha256 |
| `wait-for-log.sh <regex> <timeout_s> [--from-start]` | polls `mods/DistantHorizons/logs/dh.log` for lines written after the call |
| `screenshot.sh <name>` | captures the game window (`import -window <id>`; the X root is black under XWayland) to `artifacts/screenshots/<name>-<ts>.png` |
| `ingame-test.sh <scenario>` | user-driven checklist runner for `scripts/ingame/<scenario>.sh`; never launches or kills the game |
| `run-game.sh` / `stop-game.sh` | automated launch/kill (do not use while someone is at the display) |

`ingame-test.sh smoke`: deploys, tells you to launch `mc-run.sh Retail-1.26.5101-Levi` yourself, waits for `DH loaded` and
`level: join`, then asks you to confirm the HUD and takes the screenshot.

Facts learned about this machine

* The build resumes the last world automatically after startup (observed: `level: join` ~25 s after launch with no clicks),
  so no click sequence is needed.
* The renderer backend is **Vulkan** (RenderDoc overlay text "Capturing Vulkan", `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` in
  `mc-run.sh`); the overlay also binds F12 / PrtScrn to a RenderDoc capture.
* The game window is 2547x1343 at (254,365) on a 7680x2160 X screen; `import -window root` returns black.
* LeviLamina logger output is not in `logs/latest.log`; read `mods/DistantHorizons/logs/dh.log` (rotates at 5 MB, one `.1` backup).

HUD keys: F7 toggles the HUD, F8 writes `hud-dump: {...}` to dh.log. Spike keys: see `the internal render-spikes spike notes`.
