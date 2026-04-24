#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace collab::process;

static auto helper_path() -> std::string {
    auto dir = fs::path(TEST_BUILD_DIR);
#ifdef _WIN32
    return (dir / "test_helper.exe").string();
#else
    return (dir / "test_helper").string();
#endif
}

// ── stdin_content ──────────────────────────────────────────────

TEST_CASE("run: stdin_content is piped to child process", "[run][stdin]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"stdin"};
    config.stdin_mode = CommandConfig::StdinMode::content;
    config.stdin_content = "hello from stdin";
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("hello from stdin") != std::string::npos);
}

TEST_CASE("run: stdin_content with multiple lines", "[run][stdin]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"stdin"};
    config.stdin_mode = CommandConfig::StdinMode::content;
    config.stdin_content = "line one\nline two\nline three";
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("line one") != std::string::npos);
    CHECK(result->stdout_content.find("line two") != std::string::npos);
    CHECK(result->stdout_content.find("line three") != std::string::npos);
}

TEST_CASE("run: stdin_closed with stdin mode produces no output", "[run][stdin]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"stdin"};
    config.stdin_mode = CommandConfig::StdinMode::closed;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.empty());
}

// ── stdin_path ─────────────────────────────────────────────────

TEST_CASE("run: stdin_path pipes file contents to child", "[run][stdin]") {
    auto temp = fs::temp_directory_path() / "collab_stdin_test.txt";
    {
        std::ofstream f(temp, std::ios::binary);
        f << "file content here\n";
    }

    CommandConfig config;
    config.program = helper_path();
    config.args = {"stdin"};
    config.stdin_mode = CommandConfig::StdinMode::file;
    config.stdin_path = temp;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("file content here") != std::string::npos);

    fs::remove(temp);
}

// ── stdin_closed ───────────────────────────────────────────────

TEST_CASE("run: stdin_closed causes immediate EOF on child stdin", "[run][stdin]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"stdin"};
    config.stdin_mode = CommandConfig::StdinMode::closed;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.empty());
}
