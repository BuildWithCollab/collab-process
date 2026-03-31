#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace collab::process;

// test_helper lives in the xmake build output directory.
// TEST_BUILD_DIR is passed as a define from xmake.lua.
static auto helper_path() -> std::string {
    auto dir = fs::path(TEST_BUILD_DIR);
#ifdef _WIN32
    return (dir / "test_helper.exe").string();
#else
    return (dir / "test_helper").string();
#endif
}

TEST_CASE("run: captures stdout from a simple command", "[run]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"echo", "hello world"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "hello world");
}

TEST_CASE("run: captures stderr when stderr_capture is set", "[run]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"stderr", "oops"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::capture;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->stderr_content == "oops");
}

TEST_CASE("run: stdout is empty when mode is discard", "[run]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"echo", "should not appear"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    INFO("stdout_content was: [" << result->stdout_content << "]");
    CHECK(result->stdout_content.empty());
}

TEST_CASE("run: stderr merges into stdout when stderr_merge is true", "[run]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"both", "OUT", "ERR"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_merge = true;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    // Both should be in stdout_content
    CHECK(result->stdout_content.find("OUT") != std::string::npos);
    CHECK(result->stdout_content.find("ERR") != std::string::npos);
}

TEST_CASE("run: returns exit code 0 on success", "[run]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"exit", "0"};
    config.stdout_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 0);
    CHECK(result->ok());
}

TEST_CASE("run: returns non-zero exit code on failure", "[run]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"exit", "42"};
    config.stdout_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 42);
    CHECK_FALSE(result->ok());
}

TEST_CASE("run: returns command_not_found for bogus program", "[run]") {
    CommandConfig config;
    config.program = "definitely_not_a_real_command_xyz_12345";

    auto result = collab::process::run(config);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == SpawnError::command_not_found);
}

TEST_CASE("run: Result::ok() is false when timed out", "[run]") {
    Result r;
    r.exit_code = 0;
    r.timed_out = true;
    CHECK_FALSE(r.ok());
}

TEST_CASE("run: Result::ok() is false when exit_code is nullopt", "[run]") {
    Result r;
    // Default exit_code is nullopt
    CHECK_FALSE(r.ok());
}
