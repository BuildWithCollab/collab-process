#include <catch2/catch_test_macros.hpp>

#include <collab/process/process.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace collab::process;

// Path to the test_helper binary — lives in the same build output dir as this test.
static auto helper_path() -> std::string {
    // xmake puts all binaries in the same output directory.
    // Scan common build output patterns across platforms.
    std::vector<std::string> candidates;
    for (auto& dir : std::initializer_list<const char*>{
        "build/windows/x64/release",
        "build/windows/x64/debug",
        "build/linux/x86_64/release",
        "build/linux/x86_64/debug",
        "build/macosx/arm64/release",
        "build/macosx/arm64/debug",
        "build/macosx/x86_64/release",
        ".",
    }) {
        for (auto& name : {"test_helper.exe", "test_helper"}) {
            auto p = fs::path(dir) / name;
            if (fs::exists(p))
                return fs::absolute(p).string();
        }
    }
    return "test_helper";
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

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
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
