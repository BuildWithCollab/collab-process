-- Test helper binary — used by all tests as the spawned program
target("test_helper")
    set_kind("binary")
    add_files("test_helper.cpp")

-- Main test binary
target("collab-process-tests")
    set_kind("binary")
    add_deps("collab-process", "test_helper")
    add_packages("catch2")
    add_files("test_run.cpp")

    -- Add platform-specific test files
    if is_plat("windows") then
        -- add_files("test_win32.cpp")  -- uncomment when ready
    end

    add_tests("default", { runargs = {} })
