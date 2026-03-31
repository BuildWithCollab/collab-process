#include <catch2/catch_test_macros.hpp>

#include <collab/process/process.hpp>

#include <filesystem>
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

// ── Basic fluent chain (rvalue) ────────────────────────────────

TEST_CASE("command: basic rvalue chain captures stdout", "[command]") {
    auto result = Command(helper_path())
        .args({"echo", "fluent"})
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "fluent");
}

// ── Named builder with std::move ───────────────────────────────

TEST_CASE("command: named builder with explicit move", "[command]") {
    auto cmd = Command(helper_path());
    cmd.args({"echo", "named"});
    cmd.stdout_capture();
    cmd.stderr_discard();

    auto result = std::move(cmd).run();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "named");
}

// ── arg() vs args() ───────────────────────────────────────────

TEST_CASE("command: single arg() builds arguments", "[command]") {
    auto result = Command(helper_path())
        .arg("echo")
        .arg("single_arg")
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->stdout_content == "single_arg");
}

TEST_CASE("command: args() with vector", "[command]") {
    std::vector<std::string> arguments = {"echo", "from_vector"};
    auto result = Command(helper_path())
        .args(std::move(arguments))
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->stdout_content == "from_vector");
}

// ── Stdout modes ───────────────────────────────────────────────

TEST_CASE("command: stdout_discard suppresses output", "[command]") {
    auto result = Command(helper_path())
        .args({"echo", "hidden"})
        .stdout_discard()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->stdout_content.empty());
}

// ── Stderr modes ───────────────────────────────────────────────

TEST_CASE("command: stderr_capture captures stderr", "[command]") {
    auto result = Command(helper_path())
        .args({"stderr", "oops"})
        .stdout_discard()
        .stderr_capture()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->stderr_content == "oops");
}

TEST_CASE("command: stderr_merge sends stderr to stdout", "[command]") {
    auto result = Command(helper_path())
        .args({"both", "OUT", "ERR"})
        .stdout_capture()
        .stderr_merge()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->stdout_content.find("OUT") != std::string::npos);
    CHECK(result->stdout_content.find("ERR") != std::string::npos);
}

// ── Stdin ──────────────────────────────────────────────────────

TEST_CASE("command: stdin_string pipes content", "[command][stdin]") {
    auto result = Command(helper_path())
        .args({"stdin"})
        .stdin_string("piped input")
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("piped input") != std::string::npos);
}

TEST_CASE("command: stdin_close causes immediate EOF", "[command][stdin]") {
    auto result = Command(helper_path())
        .args({"stdin"})
        .stdin_close()
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.empty());
}

// ── Environment ────────────────────────────────────────────────

TEST_CASE("command: env() sets a variable", "[command][env]") {
    auto result = Command(helper_path())
        .args({"env", "CMD_TEST_VAR"})
        .env("CMD_TEST_VAR", "works")
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("works") != std::string::npos);
}

TEST_CASE("command: env_remove() removes a variable", "[command][env]") {
    auto result = Command(helper_path())
        .args({"env", "PATH"})
        .env_remove("PATH")
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->exit_code == 1);
}

TEST_CASE("command: env_clear() starts with empty env", "[command][env]") {
    auto result = Command(helper_path())
        .args({"env", "PATH"})
        .env_clear()
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->exit_code == 1);
}

// ── Working directory ──────────────────────────────────────────

TEST_CASE("command: working_directory() changes cwd", "[command][working_dir]") {
    auto temp_dir = fs::temp_directory_path();

    auto result = Command(helper_path())
        .args({"cwd"})
        .working_directory(temp_dir)
        .stdout_capture()
        .stderr_capture()
        .run();

    REQUIRE(result.has_value());
    INFO("exit_code: " << (result->exit_code ? std::to_string(*result->exit_code) : "nullopt"));
    INFO("stdout: [" << result->stdout_content << "]");
    INFO("stderr: [" << result->stderr_content << "]");
    REQUIRE(result->ok());

    auto expected = fs::canonical(temp_dir).string();
    REQUIRE_FALSE(result->stdout_content.empty());
    auto actual_canonical = fs::canonical(fs::path(result->stdout_content)).string();
    CHECK(actual_canonical == expected);
}

// ── Timeout ────────────────────────────────────────────────────

TEST_CASE("command: timeout() kills a slow process", "[command][timeout]") {
    auto start = std::chrono::steady_clock::now();
    auto result = Command(helper_path())
        .args({"sleep", "30"})
        .timeout(1000ms)
        .stdout_discard()
        .stderr_discard()
        .run();
    auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(result.has_value());
    CHECK(result->timed_out);
    CHECK_FALSE(result->ok());
    CHECK(elapsed < 10s);
}

// ── spawn() via Command ────────────────────────────────────────

TEST_CASE("command: spawn() returns a RunningProcess", "[command][spawn]") {
    auto proc = Command(helper_path())
        .args({"sleep", "2"})
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    CHECK(proc->pid() > 0);
    CHECK(proc->is_alive());

    proc->kill();
}

// ── Exit codes ─────────────────────────────────────────────────

TEST_CASE("command: captures exit code", "[command]") {
    auto result = Command(helper_path())
        .args({"exit", "42"})
        .stdout_discard()
        .stderr_discard()
        .run();

    REQUIRE(result.has_value());
    CHECK(result->exit_code == 42);
    CHECK_FALSE(result->ok());
}

// ── Error cases ────────────────────────────────────────────────

TEST_CASE("command: invalid program returns command_not_found", "[command]") {
    auto result = Command("not_a_real_program_xyz")
        .stdout_discard()
        .run();

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == SpawnError::command_not_found);
}
