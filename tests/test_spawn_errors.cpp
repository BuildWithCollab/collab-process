#include <catch2/catch_test_macros.hpp>

#include <collab/process/process.hpp>

#include <filesystem>

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

// ── command_not_found ──────────────────────────────────────────

TEST_CASE("errors: command_not_found for nonexistent program", "[errors]") {
    CommandConfig config;
    config.program = "definitely_not_a_real_command_xyz_12345";

    auto result = collab::process::run(config);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == SpawnError::command_not_found);
}

// ── invalid_working_directory ──────────────────────────────────

TEST_CASE("errors: invalid_working_directory for nonexistent path", "[errors]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"echo", "hi"};
    config.working_dir = "/this/path/does/not/exist/at/all";

    auto result = collab::process::run(config);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == SpawnError::invalid_working_directory);
}

// ── SpawnError::what() ─────────────────────────────────────────

TEST_CASE("errors: what() returns a non-empty message for command_not_found", "[errors]") {
    CommandConfig config;
    config.program = "bogus_program_xyz";

    auto result = collab::process::run(config);
    REQUIRE_FALSE(result.has_value());

    auto msg = result.error().what();
    CHECK_FALSE(msg.empty());
    // Should mention the error kind or be descriptive
    INFO("what() returned: " << msg);
}

TEST_CASE("errors: what() returns a non-empty message for invalid_working_directory", "[errors]") {
    CommandConfig config;
    config.program = helper_path();
    config.working_dir = "/nonexistent/dir";

    auto result = collab::process::run(config);
    REQUIRE_FALSE(result.has_value());

    auto msg = result.error().what();
    CHECK_FALSE(msg.empty());
    INFO("what() returned: " << msg);
}

// ── SpawnError via spawn() ─────────────────────────────────────

TEST_CASE("errors: spawn() also returns command_not_found", "[errors][spawn]") {
    CommandConfig config;
    config.program = "bogus_program_xyz";

    auto proc = collab::process::spawn(config);
    REQUIRE_FALSE(proc.has_value());
    CHECK(proc.error().kind == SpawnError::command_not_found);
}

TEST_CASE("errors: spawn() returns invalid_working_directory", "[errors][spawn]") {
    CommandConfig config;
    config.program = helper_path();
    config.working_dir = "/nonexistent/dir";

    auto proc = collab::process::spawn(config);
    REQUIRE_FALSE(proc.has_value());
    CHECK(proc.error().kind == SpawnError::invalid_working_directory);
}
