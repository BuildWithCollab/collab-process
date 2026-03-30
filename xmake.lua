add_rules("mode.release")

-- Default to release simply because failed ASSERT causes annoying popups in debug mode :P
set_defaultmode("release")
set_languages("c++23")

option("build_tests")
    set_default(true)
    set_showmenu(true)
    set_description("Build test targets")
option_end()

if get_config("build_tests") then
    add_requires("catch2 3.x")
end

target("collab-process")
    set_kind("static")
    add_headerfiles("include/(**.hpp)")
    add_includedirs("include", { public = true })

    -- Shared source files
    add_files("src/*.cpp")

    -- Platform-specific source files
    if is_plat("windows") then
        add_files("src/win32/*.cpp")
    else
        add_files("src/unix/*.cpp")
    end

    -- Internal headers (src/ has private headers like platform.hpp)
    add_includedirs("src", { private = true })

if get_config("build_tests") then
    includes("tests/xmake.lua")
end
