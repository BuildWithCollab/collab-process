#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

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

// Helper to create a temp directory with a .env file
struct TempEnvDir {
    fs::path dir;

    explicit TempEnvDir(const std::string& env_content) {
        dir = fs::temp_directory_path() / ("collab_dotenv_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        std::ofstream(dir / ".env") << env_content;
    }

    ~TempEnvDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    TempEnvDir(const TempEnvDir&) = delete;
    TempEnvDir& operator=(const TempEnvDir&) = delete;
};

// ── Basic .env loading via CommandConfig ────────────────────────

TEST_CASE("dotenv: loads .env vars into child environment via CommandConfig", "[dotenv][run]") {
    TempEnvDir env_dir("DOTENV_TEST_BASIC=hello_from_dotenv\n");

    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "DOTENV_TEST_BASIC"};
    config.working_dir = env_dir.dir;
    config.dotenv = true;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "hello_from_dotenv");
}

// ── dotenv=false by default ────────────────────────────────────

TEST_CASE("dotenv: disabled by default — .env vars not loaded", "[dotenv][run]") {
    TempEnvDir env_dir("DOTENV_TEST_DISABLED=should_not_see\n");

    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "DOTENV_TEST_DISABLED"};
    config.working_dir = env_dir.dir;
    // config.dotenv is false by default
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    // test_helper exits 1 when env var is not found
    CHECK(result->exit_code == 1);
}

// ── Multiple vars in .env ──────────────────────────────────────

TEST_CASE("dotenv: loads multiple vars from .env file", "[dotenv][run]") {
    TempEnvDir env_dir("DOTENV_MULTI_A=alpha\nDOTENV_MULTI_B=beta\n");

    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "DOTENV_MULTI_A"};
    config.working_dir = env_dir.dir;
    config.dotenv = true;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result_a = collab::process::run(config);
    REQUIRE(result_a.has_value());
    CHECK(result_a->ok());
    CHECK(result_a->stdout_content == "alpha");

    config.args = {"env", "DOTENV_MULTI_B"};
    auto result_b = collab::process::run(config);
    REQUIRE(result_b.has_value());
    CHECK(result_b->ok());
    CHECK(result_b->stdout_content == "beta");
}

// ── Explicit env_add overrides dotenv vars ──────────────────────

TEST_CASE("dotenv: explicit env_add overrides .env values", "[dotenv][run]") {
    TempEnvDir env_dir("DOTENV_OVERRIDE=from_file\n");

    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "DOTENV_OVERRIDE"};
    config.working_dir = env_dir.dir;
    config.dotenv = true;
    config.env_add = {{"DOTENV_OVERRIDE", "from_config"}};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "from_config");
}

// ── Dotenv with env_clear ──────────────────────────────────────

TEST_CASE("dotenv: works with env_clear — only dotenv + env_add vars present", "[dotenv][run]") {
    TempEnvDir env_dir("DOTENV_CLEAR_TEST=survived\n");

    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "DOTENV_CLEAR_TEST"};
    config.working_dir = env_dir.dir;
    config.dotenv = true;
    config.env_clear = true;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "survived");
}

// ── Quoted values in .env ──────────────────────────────────────

TEST_CASE("dotenv: handles quoted values in .env", "[dotenv][run]") {
    TempEnvDir env_dir("DOTENV_QUOTED=\"hello world\"\n");

    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "DOTENV_QUOTED"};
    config.working_dir = env_dir.dir;
    config.dotenv = true;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "hello world");
}

// ── Comments in .env ───────────────────────────────────────────

TEST_CASE("dotenv: ignores comments in .env", "[dotenv][run]") {
    TempEnvDir env_dir("# This is a comment\nDOTENV_COMMENT_TEST=visible\n# Another comment\n");

    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "DOTENV_COMMENT_TEST"};
    config.working_dir = env_dir.dir;
    config.dotenv = true;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "visible");
}

// ── Variable expansion in .env ─────────────────────────────────

TEST_CASE("dotenv: expands variable references in .env", "[dotenv][run]") {
    TempEnvDir env_dir("DOTENV_BASE=hello\nDOTENV_EXPANDED=${DOTENV_BASE}_world\n");

    CommandConfig config;
    config.program = helper_path();
    config.args = {"env", "DOTENV_EXPANDED"};
    config.working_dir = env_dir.dir;
    config.dotenv = true;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "hello_world");
}

// ── No .env file — dotenv=true but no file present ─────────────

TEST_CASE("dotenv: no .env file present — runs normally with no extra vars", "[dotenv][run]") {
    // Create a temp dir with NO .env file
    auto dir = fs::temp_directory_path() / ("collab_dotenv_empty_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);

    CommandConfig config;
    config.program = helper_path();
    config.args = {"echo", "still_works"};
    config.working_dir = dir;
    config.dotenv = true;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);

    std::error_code ec;
    fs::remove_all(dir, ec);

    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "still_works");
}

// ── Command fluent builder: .dotenv() ──────────────────────────

TEST_CASE("dotenv: fluent .dotenv() loads vars", "[dotenv][command]") {
    TempEnvDir env_dir("DOTENV_FLUENT_TEST=fluent_works\n");

    auto result = Command(helper_path())
        .args({"env", "DOTENV_FLUENT_TEST"})
        .working_directory(env_dir.dir)
        .dotenv()
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "fluent_works");
}

TEST_CASE("dotenv: fluent .dotenv() with env() override", "[dotenv][command]") {
    TempEnvDir env_dir("DOTENV_FLUENT_OVR=from_file\n");

    auto result = Command(helper_path())
        .args({"env", "DOTENV_FLUENT_OVR"})
        .working_directory(env_dir.dir)
        .dotenv()
        .env("DOTENV_FLUENT_OVR", "from_fluent")
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "from_fluent");
}

// ── Dotenv with spawn() ────────────────────────────────────────

TEST_CASE("dotenv: works with spawn()", "[dotenv][spawn]") {
    TempEnvDir env_dir("DOTENV_SPAWN_TEST=spawned\n");

    auto proc = Command(helper_path())
        .args({"env", "DOTENV_SPAWN_TEST"})
        .working_directory(env_dir.dir)
        .dotenv()
        .stdout_capture()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "spawned");
}
