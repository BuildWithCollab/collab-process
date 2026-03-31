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

// ── Construction ───────────────────────────────────────────────

TEST_CASE("process_ref: stores the PID it was given", "[process_ref]") {
    ProcessRef ref(12345);
    CHECK(ref.pid() == 12345);
}

// ── is_alive with a real process ───────────────────────────────

TEST_CASE("process_ref: is_alive() returns true for a running process", "[process_ref]") {
    // Spawn a real process to get a valid PID
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "10"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());

    auto pid = proc->pid();
    ProcessRef ref(pid);
    CHECK(ref.is_alive());

    proc->kill();
}

TEST_CASE("process_ref: is_alive() returns false for a bogus PID", "[process_ref]") {
    // PID 99999999 almost certainly doesn't exist
    ProcessRef ref(99999999);
    CHECK_FALSE(ref.is_alive());
}

// ── kill ───────────────────────────────────────────────────────

TEST_CASE("process_ref: kill() terminates a running process", "[process_ref]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "30"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());

    auto pid = proc->pid();
    ProcessRef ref(pid);
    CHECK(ref.kill());

    std::this_thread::sleep_for(200ms);
    CHECK_FALSE(ref.is_alive());
}

TEST_CASE("process_ref: kill() on a dead process returns false", "[process_ref]") {
    ProcessRef ref(99999999);
    CHECK_FALSE(ref.kill());
}
