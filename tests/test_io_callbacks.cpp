#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <filesystem>
#include <mutex>
#include <string>

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

// ── on_stdout callback ─────────────────────────────────────────

TEST_CASE("callbacks: on_stdout fires with data from child", "[callbacks]") {
    std::mutex mtx;
    std::string received;

    CommandConfig config;
    config.program = helper_path();
    config.args = {"echo", "callback_data"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    IoCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view chunk) {
        std::lock_guard lock(mtx);
        received += chunk;
    };

    auto result = collab::process::run(config, std::move(callbacks));
    REQUIRE(result.has_value());
    CHECK(result->ok());

    std::lock_guard lock(mtx);
    CHECK(received.find("callback_data") != std::string::npos);
}

// ── on_stderr callback ─────────────────────────────────────────

TEST_CASE("callbacks: on_stderr fires with stderr data", "[callbacks]") {
    std::mutex mtx;
    std::string received;

    CommandConfig config;
    config.program = helper_path();
    config.args = {"stderr", "err_callback"};
    config.stdout_mode = CommandConfig::OutputMode::discard;
    config.stderr_mode = CommandConfig::OutputMode::capture;

    IoCallbacks callbacks;
    callbacks.on_stderr = [&](std::string_view chunk) {
        std::lock_guard lock(mtx);
        received += chunk;
    };

    auto result = collab::process::run(config, std::move(callbacks));
    REQUIRE(result.has_value());

    std::lock_guard lock(mtx);
    CHECK(received.find("err_callback") != std::string::npos);
}

// ── both callbacks simultaneously ──────────────────────────────

TEST_CASE("callbacks: both on_stdout and on_stderr fire", "[callbacks]") {
    std::mutex mtx;
    std::string out_received;
    std::string err_received;

    CommandConfig config;
    config.program = helper_path();
    config.args = {"both", "STDOUT_CB", "STDERR_CB"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::capture;

    IoCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view chunk) {
        std::lock_guard lock(mtx);
        out_received += chunk;
    };
    callbacks.on_stderr = [&](std::string_view chunk) {
        std::lock_guard lock(mtx);
        err_received += chunk;
    };

    auto result = collab::process::run(config, std::move(callbacks));
    REQUIRE(result.has_value());

    std::lock_guard lock(mtx);
    CHECK(out_received.find("STDOUT_CB") != std::string::npos);
    CHECK(err_received.find("STDERR_CB") != std::string::npos);
}

// ── callback with large output ─────────────────────────────────

TEST_CASE("callbacks: on_stdout handles large output without deadlock", "[callbacks][flood]") {
    std::mutex mtx;
    size_t total_bytes = 0;

    CommandConfig config;
    config.program = helper_path();
    config.args = {"flood", "500000"};  // 500KB
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;
    config.timeout = std::chrono::milliseconds{10000};

    IoCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view chunk) {
        std::lock_guard lock(mtx);
        total_bytes += chunk.size();
    };

    auto result = collab::process::run(config, std::move(callbacks));
    REQUIRE(result.has_value());
    CHECK(result->ok());

    std::lock_guard lock(mtx);
    CHECK(total_bytes >= 500000);
}

// ── no callback is fine ────────────────────────────────────────

TEST_CASE("callbacks: empty IoCallbacks doesn't crash", "[callbacks]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"echo", "no_callback"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    IoCallbacks callbacks;  // both null

    auto result = collab::process::run(config, std::move(callbacks));
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content == "no_callback");
}
