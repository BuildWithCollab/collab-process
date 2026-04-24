#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace collab::process;
using namespace std::chrono_literals;

static auto test_build_dir() -> fs::path { return fs::path(TEST_BUILD_DIR); }

static auto helper_path() -> std::string {
#ifdef _WIN32
    return (test_build_dir() / "test_helper.exe").string();
#else
    return (test_build_dir() / "test_helper").string();
#endif
}

static auto harness_path() -> std::string {
#ifdef _WIN32
    return (test_build_dir() / "lifecycle_harness.exe").string();
#else
    return (test_build_dir() / "lifecycle_harness").string();
#endif
}

// Wait up to `limit` for the predicate to hold — avoids flaky sleeps when
// the real deadline is "soon" but not exactly 500ms. Returns true if it
// became true before the deadline.
template <typename Pred>
static bool wait_until(std::chrono::milliseconds limit, Pred pred) {
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(25ms);
    }
    return pred();
}

// Harness prints the target PID as its first stdout line.
static int read_pid_from_harness_stdout(std::string_view captured) {
    int pid = 0;
    for (char c : captured) {
        if (c == '\n' || c == '\r') break;
        if (c >= '0' && c <= '9') pid = pid * 10 + (c - '0');
    }
    return pid;
}

// ── Orphan prevention on abrupt parent death ─────────────────────────────
//
// Unix-only: Windows already guarantees this via Job Object kill-on-close;
// the dedicated regression guard below covers that path.

#ifndef _WIN32
TEST_CASE("lifecycle: Unix child dies when parent process is SIGKILL'd",
          "[lifecycle][unix]") {
    // Spawn a harness that in turn library-spawns a long sleeper. Then
    // SIGKILL the harness. The sleeper must be gone within 500ms.
    auto harness = Command(harness_path())
        .args({"spawn", helper_path(), "60"})
        .stdout_capture()
        .stderr_discard()
        .spawn();
    REQUIRE(harness.has_value());

    // Wait for the harness to print the target PID. The harness flushes
    // after writing, so a brief sleep + is_alive loop is enough.
    int target = 0;
    wait_until(3s, [&] {
        // Peek by trying to read one byte at a time via a poll is heavy;
        // instead we rely on the harness to have printed + flushed.
        // Give the harness a moment, then read what's available via wait().
        return false;  // placeholder — we use wait() below
    });

    // Kill the harness directly. We're simulating abrupt parent death —
    // the parent here is the library-owning harness. SIGKILL to the
    // harness's target PID (what the test's RunningProcess::pid() returns)
    // is the "abrupt death" signal.
    int harness_pid = harness->pid();
    REQUIRE(harness_pid > 0);

    // Give the harness time to actually spawn the sleeper and flush.
    std::this_thread::sleep_for(300ms);

    // Read the PID the harness printed. Peek by killing the harness (which
    // closes its stdout) and reading the captured stream via wait().
    ::kill(harness_pid, SIGKILL);
    auto result = harness->wait();
    REQUIRE(result.has_value());
    target = read_pid_from_harness_stdout(result->stdout_content);
    REQUIRE(target > 0);

    // Sleeper must be gone within 500ms of harness death.
    bool gone = wait_until(500ms, [target] {
        return !ProcessRef(target).is_alive();
    });
    CHECK(gone);

    // Defensive cleanup if somehow still alive.
    if (ProcessRef(target).is_alive()) ProcessRef(target).kill();
}
#endif

// Windows regression: Job Object kill-on-close already handles this. The
// test below uses the harness to prove that the Windows behavior is
// preserved end-to-end, even though no supervisor is involved.

#ifdef _WIN32
TEST_CASE("lifecycle: Windows Job Object tree-kill on abrupt parent death",
          "[lifecycle][windows]") {
    auto harness = Command(harness_path())
        .args({"spawn", helper_path(), "60"})
        .stdout_capture()
        .stderr_discard()
        .spawn();
    REQUIRE(harness.has_value());

    std::this_thread::sleep_for(300ms);

    int harness_pid = harness->pid();
    REQUIRE(harness_pid > 0);

    // TerminateProcess on the harness itself. On Windows the library's
    // Job Object covers the harness's children via kill-on-close, but
    // we're testing the *harness's own* library-spawned sleeper: when
    // the harness dies, its Job Object closes, its sleeper dies.
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)harness_pid);
    REQUIRE(h != nullptr);
    TerminateProcess(h, 1);
    WaitForSingleObject(h, 2000);
    CloseHandle(h);

    auto result = harness->wait();
    REQUIRE(result.has_value());
    int target = read_pid_from_harness_stdout(result->stdout_content);
    REQUIRE(target > 0);

    bool gone = wait_until(1s, [target] {
        return !ProcessRef(target).is_alive();
    });
    CHECK(gone);

    if (ProcessRef(target).is_alive()) ProcessRef(target).kill();
}
#endif

// ── Detach preserves the child across all platforms ──────────────────────

TEST_CASE("lifecycle: spawn_detached child survives parent death",
          "[lifecycle][detach]") {
    std::atomic<int> target_pid{0};
    auto harness = Command(harness_path())
        .args({"spawn_detached", helper_path(), "10"})
        .stdout_capture()
        .stderr_capture()
        .stdout_callback([&](std::string_view chunk) {
            // Harness prints the target PID as its first line. Parse it as
            // soon as the callback fires so we can observe while harness
            // is still alive.
            if (target_pid.load() != 0) return;
            int pid = 0;
            for (char c : chunk) {
                if (c == '\n' || c == '\r') { target_pid.store(pid); return; }
                if (c >= '0' && c <= '9') pid = pid * 10 + (c - '0');
            }
        })
        .spawn();
    REQUIRE(harness.has_value());

    // Wait for the harness to print the PID (it sleeps briefly after
    // spawn_detached). While harness is still alive, verify sleeper is
    // alive too — catches the "detach didn't clear kill-on-close" case.
    bool got_pid = wait_until(3s, [&] { return target_pid.load() != 0; });
    REQUIRE(got_pid);
    int target = target_pid.load();
    CHECK(ProcessRef(target).is_alive());  // pre-harness-exit

    // Now let harness exit. Sleeper must survive.
    auto result = harness->wait();
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 0);

    // Dump harness diagnostics on failure so we can see what it observed.
    INFO("harness stderr: " << result->stderr_content);

    std::this_thread::sleep_for(300ms);
    CHECK(ProcessRef(target).is_alive());  // post-harness-exit

    ProcessRef(target).kill();
}

TEST_CASE("lifecycle: spawn() + detach() child survives parent death",
          "[lifecycle][detach]") {
    // This is the case that prctl(PR_SET_PDEATHSIG) alone cannot handle
    // on Linux — the library must use a supervisor to let detach() work
    // after observation.
    auto harness = Command(harness_path())
        .args({"spawn_observe_detach", helper_path(), "10"})
        .stdout_capture()
        .stderr_discard()
        .spawn();
    REQUIRE(harness.has_value());

    auto result = harness->wait();
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 0);

    int target = read_pid_from_harness_stdout(result->stdout_content);
    REQUIRE(target > 0);

    std::this_thread::sleep_for(300ms);
    CHECK(ProcessRef(target).is_alive());

    ProcessRef(target).kill();
}

// ── Graceful exit still works (regression guard) ─────────────────────────

TEST_CASE("lifecycle: graceful parent exit tears down non-detached child",
          "[lifecycle][graceful]") {
    int pid = 0;
    {
        auto proc = Command(helper_path())
            .args({"sleep", "30"})
            .stdout_discard()
            .stderr_discard()
            .spawn();
        REQUIRE(proc.has_value());
        pid = proc->pid();
        REQUIRE(pid > 0);
    }  // ~RunningProcess fires — kill via RAII.

    bool gone = wait_until(500ms, [pid] {
        return !ProcessRef(pid).is_alive();
    });
    CHECK(gone);
}

// ── Bool contract ────────────────────────────────────────────────────────

TEST_CASE("lifecycle: kill() returns false on already-exited process",
          "[lifecycle][bool]") {
    // kill() is unconditional — works in both interactive and headless.
    // Tested with default (interactive) mode because RAII teardown relies
    // on kill() working there.
    auto proc = Command(helper_path())
        .args({"exit", "0"})
        .stdout_discard()
        .stderr_discard()
        .spawn();
    REQUIRE(proc.has_value());

    auto result = proc->wait();
    REQUIRE(result.has_value());

    CHECK_FALSE(proc->kill());
}

TEST_CASE("lifecycle: terminate()/interrupt() return false on already-exited headless process",
          "[lifecycle][bool]") {
    // terminate()/interrupt() require headless mode (they throw ModeError
    // in interactive). Bool contract then applies: target gone → false.
    auto proc = Command(helper_path())
        .args({"exit", "0"})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();
    REQUIRE(proc.has_value());

    auto result = proc->wait();
    REQUIRE(result.has_value());

    CHECK_FALSE(proc->terminate());
    CHECK_FALSE(proc->interrupt());
}

// ── spawn_detached pgrp isolation on Unix ────────────────────────────────

#ifndef _WIN32
TEST_CASE("lifecycle: Unix spawn_detached child is in its own process group",
          "[lifecycle][unix][spawn_detached]") {
    // Default (all-inherit) config. spawn_detached() forces
    // Mode::headless so the child always gets its own pgrp regardless of
    // the caller's mode — prevents terminal Ctrl+C aimed at the dying
    // parent's pgrp from landing on the detached child.
    CommandConfig config;
    config.program = helper_path();
    config.args = {"sleep", "5"};
    // Leave all streams at inherit — the default.

    auto pid = collab::process::spawn_detached(config);
    REQUIRE(pid.has_value());
    REQUIRE(*pid > 0);

    // Own pgrp: getpgid(child) should equal child's own PID.
    pid_t gid = ::getpgid(*pid);
    CHECK(gid == *pid);

    ProcessRef(*pid).kill();
}
#endif

// ── API surface unchanged: pid() = target, wait() = target exit ──────────

#ifndef _WIN32
TEST_CASE("lifecycle: Unix pid() returns target PID, not supervisor",
          "[lifecycle][unix][api]") {
    // If pid() returned the supervisor's pid, a kill aimed at it would
    // terminate the supervisor (leaving the target alive, eventually
    // orphaning to PID 1). kill()ing the reported pid must take the
    // target down.
    auto proc = Command(helper_path())
        .args({"sleep", "30"})
        .stdout_discard()
        .stderr_discard()
        .spawn();
    REQUIRE(proc.has_value());

    int reported = proc->pid();
    REQUIRE(reported > 0);

    CHECK(proc->kill());
    std::this_thread::sleep_for(200ms);
    CHECK_FALSE(ProcessRef(reported).is_alive());
}

TEST_CASE("lifecycle: Unix wait() returns target's exit code, not supervisor's",
          "[lifecycle][unix][api]") {
    auto proc = Command(helper_path())
        .args({"exit", "42"})
        .stdout_discard()
        .stderr_discard()
        .spawn();
    REQUIRE(proc.has_value());

    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 42);
}
#endif
