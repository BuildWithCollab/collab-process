#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

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

// ── live streaming via spawn() ─────────────────────────────────
//
// The README promises IoCallbacks "Fire on the I/O thread as data arrives."
// This test verifies that contract for spawn() — we never call wait() or
// wait_for(), yet the callback must fire.

TEST_CASE("callbacks: on_stdout fires live for spawn() without wait", "[callbacks][spawn]") {
    using namespace std::chrono_literals;

    std::mutex mtx;
    std::string received;
    std::atomic<int> chunks{0};

    auto proc = Command(helper_path())
        .args({"echo", "live_streamed"})
        .stdout_capture()
        .stdout_callback([&](std::string_view chunk) {
            std::lock_guard lock(mtx);
            received += chunk;
            chunks.fetch_add(1);
        })
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (chunks.load() > 0) break;
        std::this_thread::sleep_for(20ms);
    }

    REQUIRE(chunks.load() > 0);

    std::lock_guard lock(mtx);
    CHECK(received.find("live_streamed") != std::string::npos);
}

TEST_CASE("callbacks: on_stderr fires live for spawn() without wait", "[callbacks][spawn]") {
    using namespace std::chrono_literals;

    std::mutex mtx;
    std::string received;
    std::atomic<int> chunks{0};

    auto proc = Command(helper_path())
        .args({"stderr", "live_err"})
        .stdout_discard()
        .stderr_capture()
        .stderr_callback([&](std::string_view chunk) {
            std::lock_guard lock(mtx);
            received += chunk;
            chunks.fetch_add(1);
        })
        .spawn();

    REQUIRE(proc.has_value());

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (chunks.load() > 0) break;
        std::this_thread::sleep_for(20ms);
    }

    REQUIRE(chunks.load() > 0);

    std::lock_guard lock(mtx);
    CHECK(received.find("live_err") != std::string::npos);
}

TEST_CASE("callbacks: both on_stdout and on_stderr fire live for spawn()", "[callbacks][spawn]") {
    using namespace std::chrono_literals;

    std::mutex mtx;
    std::string out_received;
    std::string err_received;
    std::atomic<int> out_chunks{0};
    std::atomic<int> err_chunks{0};

    auto proc = Command(helper_path())
        .args({"both", "LIVE_OUT", "LIVE_ERR"})
        .stdout_capture()
        .stderr_capture()
        .stdout_callback([&](std::string_view chunk) {
            std::lock_guard lock(mtx);
            out_received += chunk;
            out_chunks.fetch_add(1);
        })
        .stderr_callback([&](std::string_view chunk) {
            std::lock_guard lock(mtx);
            err_received += chunk;
            err_chunks.fetch_add(1);
        })
        .spawn();

    REQUIRE(proc.has_value());

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (out_chunks.load() > 0 && err_chunks.load() > 0) break;
        std::this_thread::sleep_for(20ms);
    }

    REQUIRE(out_chunks.load() > 0);
    REQUIRE(err_chunks.load() > 0);

    std::lock_guard lock(mtx);
    CHECK(out_received.find("LIVE_OUT") != std::string::npos);
    CHECK(err_received.find("LIVE_ERR") != std::string::npos);
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
