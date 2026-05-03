-- Test helper binary — used by all tests as the spawned program
target("test_helper")
    set_kind("binary")
    add_files("test_helper.cpp")

-- Lifecycle harness — links against collab-process so the test can spawn
-- it and observe what happens to its library-spawned children when the
-- harness itself dies (SIGKILL'd, TerminateProcess'd, or exits normally).
target("lifecycle_harness")
    set_kind("binary")
    add_deps("collab-process", "test_helper")
    add_files("lifecycle_harness.cpp")

-- Main test binary
target("collab-process-tests")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_deps("collab-process", "test_helper", "lifecycle_harness")
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
        "test_spawn_errors.cpp",
        "test_dotenv.cpp",
        "test_signals.cpp",
        "test_lifecycle.cpp",
        "test_stdin_pipe.cpp"
    )

    -- Pass the build output dir so tests can find test_helper
    on_load(function (target)
        local builddir = path.join(target:targetdir())
        target:add("defines", 'TEST_BUILD_DIR="' .. builddir:gsub("\\", "/") .. '"')
    end)

    add_tests("default")
