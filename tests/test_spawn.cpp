#include <catch2/catch_test_macros.hpp>

#include <collab/process/process.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace collab::process;
using namespace std::chrono_literals;

static auto helper_path() -> std::string {
    auto dir = fs::path(TEST_BUILD_DIR);
#ifdef _WIN32
    return (dir / "test_helper.exe").string();
#else
    return (dir / "test_helper").string();
#endif
}

// ── spawn basics ───────────────────────────────────────────────

TEST_CASE("spawn: returns a RunningProcess for a valid command", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "2"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());
    CHECK(proc->pid() > 0);

    // Cleanup
    proc->kill();
}

TEST_CASE("spawn: returns error for invalid command", "[spawn]") {
    CommandConfig config;
    config.program = "not_a_real_program_xyz";

    auto proc = collab::process::spawn(config);
    REQUIRE_FALSE(proc.has_value());
    CHECK(proc.error().kind == SpawnError::command_not_found);
}

// ── pid ────────────────────────────────────────────────────────

TEST_CASE("spawn: pid() returns a positive process ID", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "2"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());
    CHECK(proc->pid() > 0);

    proc->kill();
}

// ── is_alive ───────────────────────────────────────────────────

TEST_CASE("spawn: is_alive() returns true for a running process", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "10"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());
    CHECK(proc->is_alive());

    proc->kill();
}

TEST_CASE("spawn: is_alive() returns false after process exits", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"exit", "0"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());

    // Wait for it to finish
    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK_FALSE(proc->is_alive());
}

// ── wait ───────────────────────────────────────────────────────

TEST_CASE("spawn: wait() returns Result with captured stdout", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"echo", "spawned output"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());

    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "spawned output");
}

TEST_CASE("spawn: wait() returns non-zero exit code", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"exit", "7"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());

    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 7);
    CHECK_FALSE(result->ok());
}

// ── wait_for ───────────────────────────────────────────────────

TEST_CASE("spawn: wait_for() returns result when process finishes in time", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"echo", "fast"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());

    auto result = proc->wait_for(5000ms);
    REQUIRE(result.has_value());  // optional has a value — process finished
    CHECK(result->ok());
    CHECK(result->stdout_content == "fast");
}

TEST_CASE("spawn: wait_for() returns nullopt when process is still running", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "30"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());

    auto start = std::chrono::steady_clock::now();
    auto result = proc->wait_for(500ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    // nullopt = "not done yet"
    CHECK_FALSE(result.has_value());
    CHECK(elapsed < 5s);

    // Process should still be alive — wait_for doesn't kill
    CHECK(proc->is_alive());

    proc->kill();
}

// ── stop (graceful shutdown) ───────────────────────────────────

TEST_CASE("spawn: stop() terminates a running process", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "30"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());
    REQUIRE(proc->is_alive());

    auto stop_result = proc->stop(2s);
    CHECK((stop_result == StopResult::stopped_gracefully ||
           stop_result == StopResult::killed));
    CHECK_FALSE(proc->is_alive());
}

TEST_CASE("spawn: stop() on already-exited process returns not_running", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"exit", "0"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());

    // Wait for natural exit
    [[maybe_unused]] auto _ = proc->wait();

    auto stop_result = proc->stop();
    CHECK(stop_result == StopResult::not_running);
}

// ── kill (immediate) ───────────────────────────────────────────

// ── detach ─────────────────────────────────────────────────────

TEST_CASE("spawn: detach() releases ownership and returns PID", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "2"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());

    int pid = proc->pid();
    int detached_pid = std::move(*proc).detach();
    CHECK(detached_pid == pid);

    // Process should still be alive after detach — we released it
    ProcessRef ref(detached_pid);
    CHECK(ref.is_alive());

    // Cleanup
    ref.kill();
}

TEST_CASE("spawn: destructor kills a running process (RAII)", "[spawn]") {
    int pid;
    {
        CommandConfig config;
        config.program = helper_path();
        config.args = {"sleep", "30"};
        config.stdout_mode = CommandConfig::OutputMode::discard;
        config.stderr_mode = CommandConfig::OutputMode::discard;

        auto proc = collab::process::spawn(config);
        REQUIRE(proc.has_value());
        pid = proc->pid();
        CHECK(proc->is_alive());
        // proc goes out of scope here — destructor should kill
    }

    // CI runners may be slow to reap — give it time
    std::this_thread::sleep_for(500ms);
    ProcessRef ref(pid);
    CHECK_FALSE(ref.is_alive());
}

// ── kill (immediate) ───────────────────────────────────────────

TEST_CASE("spawn: kill() immediately terminates a running process", "[spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "30"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());
    REQUIRE(proc->is_alive());

    CHECK(proc->kill());

    // CI runners may be slow to reap — give it time
    std::this_thread::sleep_for(500ms);
    CHECK_FALSE(proc->is_alive());
}
