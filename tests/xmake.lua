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
    add_files(
        "test_run.cpp",
        "test_run_stdin.cpp",
        "test_run_env.cpp",
        "test_run_misc.cpp",
        "test_spawn.cpp",
        "test_command.cpp",
        "test_process_ref.cpp",
        "test_utilities.cpp",
        "test_io_callbacks.cpp",
        "test_spawn_errors.cpp"
    )

    -- Pass the build output dir so tests can find test_helper
    on_load(function (target)
        local builddir = path.join(target:targetdir())
        target:add("defines", 'TEST_BUILD_DIR="' .. builddir:gsub("\\", "/") .. '"')
    end)

    add_tests("default")
