#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <cstdlib>
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

// ── env_add ────────────────────────────────────────────────────

TEST_CASE("run: env_add sets an environment variable in the child", "[run][env]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "COLLAB_TEST_VAR"};
    config.env_add = {{"COLLAB_TEST_VAR", "hello_env"}};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("hello_env") != std::string::npos);
}

TEST_CASE("run: env_add with multiple variables", "[run][env]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "COLLAB_TEST_A"};
    config.env_add = {{"COLLAB_TEST_A", "alpha"}, {"COLLAB_TEST_B", "beta"}};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("alpha") != std::string::npos);
}

TEST_CASE("run: env_add overrides inherited variable", "[run][env]") {
    // PATH exists in every environment — override it with a known value
    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "COLLAB_OVERRIDE_TEST"};
    config.env_add = {{"COLLAB_OVERRIDE_TEST", "original"}};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->stdout_content.find("original") != std::string::npos);

    // Now override
    config.env_add = {{"COLLAB_OVERRIDE_TEST", "overridden"}};
    auto result2 = collab::process::run(config);
    REQUIRE(result2.has_value());
    CHECK(result2->stdout_content.find("overridden") != std::string::npos);
}

// ── env_remove ─────────────────────────────────────────────────

TEST_CASE("run: env_remove removes a variable from child environment", "[run][env]") {
    // Set a var, then remove it — child should not see it
    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "PATH"};
    config.env_remove = {"PATH"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    // test_helper returns exit code 1 when env var is not found
    CHECK(result->exit_code == 1);
}

// ── env_clear ──────────────────────────────────────────────────

TEST_CASE("run: env_clear starts with empty environment", "[run][env]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "PATH"};
    config.env_clear = true;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    // PATH should not exist in cleared env
    CHECK(result->exit_code == 1);
}

TEST_CASE("run: env_clear with env_add only has the added vars", "[run][env]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "COLLAB_ONLY_VAR"};
    config.env_clear = true;
    config.env_add = {{"COLLAB_ONLY_VAR", "i_exist"}};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("i_exist") != std::string::npos);
}
