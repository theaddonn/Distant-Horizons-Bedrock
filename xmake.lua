add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client", "server")
option_end()

add_requires("levilamina 26.51.6", {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript 0.6.1")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

-- Compiler settings shared by dh_core and both mod DLLs so they link cleanly.
rule("dh.cxx")
    on_load(function (target)
        target:set("languages", "c++20")
        if target:is_plat("windows") then
            target:add("defines", "NOMINMAX", "UNICODE", "_HAS_CXX23=1")
            target:set("exceptions", "none") -- To avoid conflicts with /EHa.
            target:add("cxflags", "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
            target:add("cxflags",
                "/EHs",
                "-Wno-microsoft-cast",
                "-Wno-invalid-offsetof",
                "-Wno-c++2b-extensions",
                "-Wno-microsoft-include",
                "-Wno-overloaded-virtual",
                "-Wno-ignored-qualifiers",
                "-Wno-missing-field-initializers",
                "-Wno-potentially-evaluated-expression",
                "-Wno-pragma-system-header-outside-header",
                {tools = {"clang_cl"}})
            target:set("toolchains", "clang-cl")
        end
    end)
rule_end()

-- Platform-independent Distant Horizons core (static library, no Bedrock headers).
includes("src/core/xmake.lua")

local mod_name = is_config("target_type", "server") and "DistantHorizonsServer" or "DistantHorizons"
local mod_dir  = is_config("target_type", "server") and "src/server" or "src/client"

target(mod_name)
    add_rules("dh.cxx")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker", {modName = mod_name, modVersion = "0.1.0", modPlatform = get_config("target_type")})
    add_deps("dh_core")
    add_packages("levilamina")
    set_kind("shared")
    set_symbols("debug")
    add_headerfiles("src/bedrock_common/**.h", mod_dir .. "/**.h")
    add_files("src/bedrock_common/**.cpp", mod_dir .. "/**.cpp")
    add_includedirs("src")
