-- Host (Linux) test project for dh_core. Separate xmake project so its
-- configuration never collides with the Windows cross-build in the repo root.
--
--   cd tests
--   xmake f -p linux -a x86_64 -m debug --cc=clang-22 --cxx=clang++-22 --ld=clang++-22 -y
--   xmake && xmake run dh_core_tests

add_rules("mode.debug", "mode.release")

rule("dh.cxx")
    on_load(function (target)
        target:set("languages", "c++20")
    end)
rule_end()

includes("../src/core/xmake.lua")

target("dh_core_tests")
    set_kind("binary")
    set_languages("c++20")
    add_deps("dh_core")
    add_includedirs("third_party", ".")
    add_files("main.cpp", "unit/**.cpp", "integration/**.cpp")
    add_defines("DOCTEST_CONFIG_SUPER_FAST_ASSERTS")
    if is_mode("debug") then
        add_cxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer")
        add_ldflags("-fsanitize=address,undefined")
        add_defines("DH_DEBUG=1")
    end
    set_rundir("$(projectdir)")
