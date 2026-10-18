-- dh_core: the platform-independent Distant Horizons core.
--
-- Included by the root xmake.lua (Windows cross-build into the mod DLLs) and by
-- tests/xmake.lua (Linux host build for unit tests). Rules:
--   * no LeviLamina / Bedrock (mc/, ll/) headers — glue code lives outside src/core
--   * no exceptions: use dh::Result / DH_ASSERT
--   * -ffp-contract=off so host and MSVC builds agree on float results

target("dh_core")
    set_kind("static")
    set_languages("c++20")
    add_rules("dh.cxx")
    add_includedirs(path.join(os.scriptdir()), {public = true})
    add_headerfiles(path.join(os.scriptdir(), "dh/**.h"))
    add_files(path.join(os.scriptdir(), "dh/**.cpp"))
    set_exceptions("none")
    if is_plat("windows") then
        add_cxflags("/fp:precise", "-ffp-contract=off", {tools = {"clang_cl"}})
    else
        add_cxflags("-ffp-contract=off", "-Wall", "-Wextra")
    end
    if is_mode("debug") then
        add_defines("DH_DEBUG=1")
    end
