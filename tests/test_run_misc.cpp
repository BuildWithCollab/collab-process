#include <catch2/catch_test_macros.hpp>

#include <collab/process/process.hpp>

#include <chrono>
#include <filesystem>

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

// ── working_dir ────────────────────────────────────────────────

TEST_CASE("run: working_dir changes the child's working directory", "[run][working_dir]") {
    auto temp_dir = fs::temp_directory_path();

    CommandConfig config;
    config.program = helper_path();
    config.args = {"cwd"};
    config.working_dir = temp_dir;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());

    // Normalize both paths for comparison
    auto expected = fs::canonical(temp_dir).string();
    auto actual = result->stdout_content;
    // On Windows, canonical may differ in case or trailing slash — compare canonical forms
    auto actual_canonical = fs::canonical(fs::path(actual)).string();
    CHECK(actual_canonical == expected);
}

TEST_CASE("run: invalid working_dir returns an error", "[run][working_dir]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"cwd"};
    config.working_dir = "/this/path/definitely/does/not/exist";

    auto result = collab::process::run(config);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == SpawnError::invalid_working_directory);
}

// ── timeout ────────────────────────────────────────────────────

TEST_CASE("run: timeout kills a long-running process", "[run][timeout]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "30"};
    config.timeout = 1000ms;
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto start = std::chrono::steady_clock::now();
    auto result = collab::process::run(config);
    auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(result.has_value());
    CHECK(result->timed_out);
    CHECK_FALSE(result->ok());
    // Should have returned in ~1s, not 30s
    CHECK(elapsed < 10s);
}

TEST_CASE("run: fast command completes before timeout", "[run][timeout]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"echo", "quick"};
    config.timeout = 5000ms;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK_FALSE(result->timed_out);
    CHECK(result->stdout_content == "quick");
}

// ── flood / deadlock resistance ────────────────────────────────

TEST_CASE("run: large stdout does not deadlock", "[run][flood]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"flood", "1000000"};  // 1MB
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;
    config.timeout = 10000ms;  // safety net

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.size() >= 1000000);
}
