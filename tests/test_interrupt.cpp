#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

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

namespace {

// Shared state between the test and the I/O callback thread. Held by
// shared_ptr so the callback keeps it alive after spawn_ready returns.
struct ReadyState {
    std::mutex m;
    std::condition_variable cv;
    std::string captured;
    bool ready = false;
};

// Spawn a helper mode that prints READY once its SIGINT handler is installed.
// Returns the running process and a handle to the ready-state.
auto spawn_ready(std::string mode)
    -> std::pair<std::expected<RunningProcess, SpawnError>, std::shared_ptr<ReadyState>> {

    auto state = std::make_shared<ReadyState>();

    CommandConfig config;
    config.program = helper_path();
    config.args = {std::move(mode)};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;
    config.interruptible = true;

    IoCallbacks callbacks;
    callbacks.on_stdout = [state](std::string_view chunk) {
        std::lock_guard lock(state->m);
        state->captured.append(chunk);
        if (!state->ready && state->captured.find("READY") != std::string::npos) {
            state->ready = true;
            state->cv.notify_all();
        }
    };

    auto proc = collab::process::spawn(config, std::move(callbacks));
    return {std::move(proc), state};
}

auto wait_for_ready(ReadyState& state,
                    std::chrono::milliseconds timeout = 5s) -> bool {
    std::unique_lock lock(state.m);
    return state.cv.wait_for(lock, timeout, [&] { return state.ready; });
}

}  // namespace

// ── 1. Child with handler — receives SIGINT, exits with known code ─

TEST_CASE("conpty: cmd /c echo goes through pseudoconsole", "[interrupt]") {
    CommandConfig config;
    config.program = "cmd.exe";
    config.args = {"/c", "echo", "HELLO_CONPTY"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.interruptible = true;

    std::string captured;
    std::mutex mtx;
    IoCallbacks cb;
    cb.on_stdout = [&](std::string_view chunk) {
        std::lock_guard lock(mtx);
        captured.append(chunk);
    };

    auto result = collab::process::run(std::move(config), std::move(cb));
    REQUIRE(result.has_value());

    std::lock_guard lock(mtx);
    std::string hex;
    for (unsigned char c : captured) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02x ", c);
        hex += buf;
    }
    UNSCOPED_INFO("result.stdout_content=[" << result->stdout_content << "]");
    UNSCOPED_INFO("captured " << captured.size() << " bytes hex: " << hex);
    CHECK(captured.find("HELLO_CONPTY") != std::string::npos);
}

TEST_CASE("interrupt: diagnostic — wait works with kill", "[interrupt][diag]") {
    auto [proc, state] = spawn_ready("sigint-exit");
    REQUIRE(proc.has_value());
    REQUIRE(wait_for_ready(*state));

    // Use kill instead of interrupt to verify the wait path is OK.
    CHECK(proc->kill());

    auto result = proc->wait();
    REQUIRE(result.has_value());
    UNSCOPED_INFO("kill-path exit_code: "
        << (result->exit_code ? std::to_string(*result->exit_code) : "nullopt"));
}

TEST_CASE("interrupt: diagnostic — stdin pipe reaches child", "[interrupt][diag]") {
    // Confirm that bytes we write via interrupt()'s pipe actually reach child stdin.
    // Spawn `test_helper stdin` which reads lines and echoes them.
    CommandConfig config;
    config.program = helper_path();
    config.args = {"stdin"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.interruptible = true;
    config.stdin_mode = CommandConfig::StdinMode::content;
    config.stdin_content = "HELLO_STDIN\n";
    config.timeout = std::chrono::milliseconds{3000};

    std::string captured;
    std::mutex mtx;
    IoCallbacks cb;
    cb.on_stdout = [&](std::string_view chunk) {
        std::lock_guard lock(mtx);
        captured.append(chunk);
    };

    auto result = collab::process::run(std::move(config), std::move(cb));
    REQUIRE(result.has_value());

    std::lock_guard lock(mtx);
    UNSCOPED_INFO("captured: [" << captured << "]");
    CHECK(captured.find("HELLO_STDIN") != std::string::npos);
}

TEST_CASE("interrupt: delivers signal to child with installed handler", "[interrupt]") {
    auto [proc, state] = spawn_ready("sigint-exit");
    REQUIRE(proc.has_value());
    REQUIRE(wait_for_ready(*state));

    CHECK(proc->interrupt());

    // Poll for up to 3 seconds to let the handler fire and the child exit.
    auto polled = proc->wait_for(3s);
    if (!polled) {
        // Still running — dump what we've captured so far, then clean up.
        std::lock_guard lock(state->m);
        UNSCOPED_INFO("post-interrupt still running — captured bytes: "
            << state->captured.size()
            << " content=[" << state->captured << "]");
        proc->kill();
        FAIL("child didn't exit within 3s after interrupt()");
    }
    CHECK(polled->stdout_content.find("GOT_SIGINT") != std::string::npos);
    CHECK(polled->exit_code == 42);
}

// ── 2. Child with default handler — terminated by interrupt ─────────

TEST_CASE("interrupt: terminates child with default handler", "[interrupt]") {
    auto [proc, state] = spawn_ready("sigint-default");
    REQUIRE(proc.has_value());
    REQUIRE(wait_for_ready(*state));

    CHECK(proc->interrupt());

    auto result = proc->wait();
    REQUIRE(result.has_value());
    // Default SIGINT behavior terminates — exit code is platform-specific:
    //   Unix: 128 + SIGINT (2) = 130
    //   Windows: STATUS_CONTROL_C_EXIT = 0xC000013A (signed: -1073741510)
#ifdef _WIN32
    CHECK(result->exit_code.has_value());
    CHECK(*result->exit_code != 0);
#else
    CHECK(result->exit_code == 130);
#endif
}

// ── 3. Child that ignores — still running after interrupt ───────────

TEST_CASE("interrupt: is cooperative, child may catch and ignore", "[interrupt]") {
    auto [proc, state] = spawn_ready("sigint-ignore");
    REQUIRE(proc.has_value());
    REQUIRE(wait_for_ready(*state));

    CHECK(proc->interrupt());

    // Give the child time to run its handler.
    auto deadline = std::chrono::steady_clock::now() + 2s;
    bool saw_handler = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(state->m);
            if (state->captured.find("GOT_SIGINT") != std::string::npos) {
                saw_handler = true;
                break;
            }
        }
        std::this_thread::sleep_for(50ms);
    }
    CHECK(saw_handler);
    CHECK(proc->is_alive());

    // Clean up — child won't exit on its own.
    proc->kill();
}

// ── 4. Already-exited process — interrupt returns false ─────────────

TEST_CASE("interrupt: returns false on already-exited process", "[interrupt]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"echo", "done"};
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());
    auto result = proc->wait();
    REQUIRE(result.has_value());
    REQUIRE(result->ok());

    // Process has exited — interrupt must return false and not crash.
    CHECK_FALSE(proc->interrupt());
}

// ── 5. Interrupt then wait returns intact Result ────────────────────

TEST_CASE("interrupt: wait() after interrupt returns full Result", "[interrupt]") {
    auto [proc, state] = spawn_ready("sigint-exit");
    REQUIRE(proc.has_value());
    REQUIRE(wait_for_ready(*state));

    CHECK(proc->interrupt());

    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 42);
    // Captured stdout should contain both READY and GOT_SIGINT.
    CHECK(result->stdout_content.find("READY") != std::string::npos);
    CHECK(result->stdout_content.find("GOT_SIGINT") != std::string::npos);
    CHECK_FALSE(result->timed_out);
}

// ── 6. Process tree — grandchild receives interrupt ─────────────────
//
// Deferred: shell-based test is brittle across platforms. Will add once
// a helper mode for "spawn-child-and-sleep" exists.

// ── 7. Detached process — interrupt unsupported ─────────────────────
//
// Deferred: needs design decision on whether detach + interrupt is in
// scope at all (detached children aren't in our group/job).
