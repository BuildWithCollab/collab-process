-- Test helper binary — used by all tests as the spawned program
target("test_helper")
    set_kind("binary")
    add_files("test_helper.cpp")

-- Main test binary
target("collab-process-tests")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_deps("collab-process", "test_helper")
    add_packages("catch2")
    add_files("test_run.cpp")

    -- Add platform-specific test files
    if is_plat("windows") then
        -- add_files("test_win32.cpp")  -- uncomment when ready
    end

    -- Pass the build output dir so tests can find test_helper
    on_load(function (target)
        local builddir = path.join(target:targetdir())
        target:add("defines", 'TEST_BUILD_DIR="' .. builddir:gsub("\\", "/") .. '"')
    end)

    add_tests("default")
