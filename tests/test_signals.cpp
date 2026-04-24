#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#endif

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

// Poll a file that test_helper's spawn_child mode writes with the grandchild
// PID. Returns the PID, or 0 on timeout.
static int wait_for_pid_file(const fs::path& path, std::chrono::milliseconds limit) {
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(path) && fs::file_size(path) > 0) {
            std::ifstream in(path);
            int pid = 0;
            in >> pid;
            if (pid > 0) return pid;
        }
        std::this_thread::sleep_for(50ms);
    }
    return 0;
}

static fs::path unique_pid_file() {
    auto p = fs::temp_directory_path() / fs::path{"collab_process_gc_pid_"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        + ".txt"};
    std::error_code ec;
    fs::remove(p, ec);
    return p;
}

// ── Interactive mode: terminate/interrupt throw ModeError ──────
//
// Stream modes are irrelevant — that's the bug the redesign fixed.
// The mode is set explicitly by Command::interactive() / Command::headless()
// (default interactive), not inferred from which streams were redirected.

TEST_CASE("signals: interactive + terminate() throws ModeError", "[signals][mode]") {
    auto proc = Command(helper_path())
        .args({"sleep", "10"})
        .stdout_capture()          // intentional: prove stream mode doesn't matter
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());

    CHECK_THROWS_AS(proc->terminate(), ModeError);
    CHECK(proc->is_alive());       // process was not signalled

    proc->kill();                  // cleanup
}

TEST_CASE("signals: interactive + interrupt() throws ModeError", "[signals][mode]") {
    auto proc = Command(helper_path())
        .args({"sleep", "10"})
        .stdout_capture()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());

    CHECK_THROWS_AS(proc->interrupt(), ModeError);
    CHECK(proc->is_alive());

    proc->kill();
}

TEST_CASE("signals: ModeError is a std::logic_error", "[signals][mode]") {
    auto proc = Command(helper_path())
        .args({"sleep", "10"})
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());

    bool caught_as_logic_error = false;
    try {
        (void)proc->terminate();
    } catch (const std::logic_error&) {
        caught_as_logic_error = true;
    }
    CHECK(caught_as_logic_error);

    proc->kill();
}

TEST_CASE("signals: ModeError::what() names the method and required mode", "[signals][mode]") {
    auto proc = Command(helper_path())
        .args({"sleep", "10"})
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());

    try {
        (void)proc->terminate();
        FAIL("terminate() should have thrown");
    } catch (const ModeError& e) {
        std::string msg = e.what();
        CHECK(msg.find("terminate") != std::string::npos);
        CHECK(msg.find("headless") != std::string::npos);
    }

    try {
        (void)proc->interrupt();
        FAIL("interrupt() should have thrown");
    } catch (const ModeError& e) {
        std::string msg = e.what();
        CHECK(msg.find("interrupt") != std::string::npos);
        CHECK(msg.find("headless") != std::string::npos);
    }

    proc->kill();
}

// ── Interactive mode: kill() and RAII still work ───────────────

TEST_CASE("signals: interactive + kill() terminates the child", "[signals][interactive][kill]") {
    auto proc = Command(helper_path())
        .args({"sleep", "10"})
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    CHECK(proc->kill());
    std::this_thread::sleep_for(200ms);
    CHECK_FALSE(proc->is_alive());
}

TEST_CASE("signals: interactive RAII kills on scope exit", "[signals][interactive][raii]") {
    int pid = 0;
    {
        auto proc = Command(helper_path())
            .args({"sleep", "10"})
            .stdout_discard()
            .stderr_discard()
            .spawn();
        REQUIRE(proc.has_value());
        pid = proc->pid();
        REQUIRE(pid > 0);
    }  // ~RunningProcess()

    // Give the reaper a beat.
    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!ProcessRef(pid).is_alive()) break;
        std::this_thread::sleep_for(25ms);
    }
    CHECK_FALSE(ProcessRef(pid).is_alive());
}

// ── Interactive mode: shared process group (Unix-only) ─────────
//
// Proves the default path keeps the child wired to the terminal. We can't
// directly signal the test runner's group without hitting ourselves, so we
// only check pgrp membership.

#ifndef _WIN32
TEST_CASE("signals: Unix interactive child shares parent's process group", "[signals][unix][interactive]") {
    auto proc = Command(helper_path())
        .args({"sleep", "10"})
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());

    pid_t child_pgid = ::getpgid(proc->pid());
    pid_t my_pgid = ::getpgid(0);
    CHECK(child_pgid == my_pgid);

    proc->kill();
}
#endif

// ── Headless mode: code-driven signals deliver ─────────────────

TEST_CASE("signals: headless + terminate() delivers signal → child exits 42", "[signals][headless][terminate]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    // Give the helper a moment to install its signal handlers.
    std::this_thread::sleep_for(200ms);

    REQUIRE(proc->terminate());

    auto result = proc->wait_for(3s);
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 42);
}

TEST_CASE("signals: headless + terminate() on signal_ignore → true but still alive",
          "[signals][headless][terminate]") {
    auto proc = Command(helper_path())
        .args({"signal_ignore"})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(200ms);

    CHECK(proc->terminate());          // syscall succeeded
    std::this_thread::sleep_for(500ms);
    CHECK(proc->is_alive());           // but target ignored it

    proc->kill();
}

TEST_CASE("signals: headless + terminate() after wait() reaped → false",
          "[signals][headless][terminate]") {
    auto proc = Command(helper_path())
        .args({"exit", "0"})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    [[maybe_unused]] auto _ = proc->wait();

    CHECK_FALSE(proc->terminate());    // target gone, false per bool contract
}

#ifndef _WIN32
TEST_CASE("signals: Unix headless + interrupt() delivers SIGINT → child exits 43",
          "[signals][unix][headless][interrupt]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(200ms);

    REQUIRE(proc->interrupt());

    auto result = proc->wait_for(3s);
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 43);
}
#endif

#ifdef _WIN32
TEST_CASE("signals: Windows headless + interrupt() returns false (no platform mapping)",
          "[signals][windows][headless][interrupt]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(200ms);

    // No platform-viable mapping — CTRL_C_EVENT is disabled for processes
    // in a new process group per MSDN. Must return false, not throw.
    CHECK_FALSE(proc->interrupt());

    proc->kill();
}
#endif

// ── Headless mode: kill() tree-kill ────────────────────────────

TEST_CASE("signals: headless + kill() terminates the child", "[signals][headless][kill]") {
    auto proc = Command(helper_path())
        .args({"sleep", "30"})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    CHECK(proc->kill());

    auto result = proc->wait_for(500ms);
    REQUIRE(result.has_value());
    CHECK_FALSE(proc->is_alive());
}

TEST_CASE("signals: headless + kill() tree-kills grandchild", "[signals][headless][kill]") {
    auto pid_file = unique_pid_file();

    auto proc = Command(helper_path())
        .args({"spawn_child", "30", pid_file.string()})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());

    int grandchild = wait_for_pid_file(pid_file, 3s);
    REQUIRE(grandchild > 0);

    CHECK(proc->kill());
    std::this_thread::sleep_for(500ms);

    ProcessRef gc(grandchild);
    CHECK_FALSE(gc.is_alive());

    std::error_code ec;
    fs::remove(pid_file, ec);
}

// ── Headless mode: isolated from parent's process group (Unix) ─

#ifndef _WIN32
TEST_CASE("signals: Unix headless child is in its own process group (pgid == pid)",
          "[signals][unix][headless]") {
    auto proc = Command(helper_path())
        .args({"sleep", "10"})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());

    pid_t pgid = ::getpgid(proc->pid());
    CHECK(pgid == proc->pid());

    proc->kill();
}
#endif

// ── Composition — graceful shutdown patterns ───────────────────

TEST_CASE("signals: compose terminate + wait_for reaps signal_trap child",
          "[signals][headless][compose]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(200ms);

    REQUIRE(proc->terminate());

    auto result = proc->wait_for(2s);
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 42);
}

TEST_CASE("signals: compose terminate then kill reaps signal_ignore child",
          "[signals][headless][compose]") {
    auto proc = Command(helper_path())
        .args({"signal_ignore"})
        .headless()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(200ms);

    REQUIRE(proc->terminate());
    CHECK_FALSE(proc->wait_for(500ms).has_value());  // still alive

    CHECK(proc->kill());
    auto result = proc->wait_for(500ms);
    REQUIRE(result.has_value());
    CHECK_FALSE(proc->is_alive());
}
