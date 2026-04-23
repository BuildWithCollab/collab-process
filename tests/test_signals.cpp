#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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

// ── terminate() ────────────────────────────────────────────────

TEST_CASE("terminate: own group + signal_trap → child exits 42", "[signals][terminate]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .own_process_group()
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

TEST_CASE("terminate: own group + signal_ignore → true but still alive", "[signals][terminate]") {
    auto proc = Command(helper_path())
        .args({"signal_ignore"})
        .own_process_group()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(200ms);

    CHECK(proc->terminate());  // syscall succeeded
    std::this_thread::sleep_for(500ms);
    CHECK(proc->is_alive());   // but target ignored it

    proc->kill();
}

#ifdef _WIN32
TEST_CASE("terminate: inherit group on Windows → returns false", "[signals][terminate]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .inherit_process_group()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(200ms);

    // With no new process group, CTRL_BREAK has no group to target.
    CHECK_FALSE(proc->terminate());

    proc->kill();
}
#endif

#ifndef _WIN32
TEST_CASE("terminate: inherit group on Unix + signal_trap → child exits 42", "[signals][terminate]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .inherit_process_group()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(200ms);

    REQUIRE(proc->terminate());

    auto result = proc->wait_for(3s);
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 42);
}
#endif

TEST_CASE("terminate: after wait() has reaped the child → returns false", "[signals][terminate]") {
    auto proc = Command(helper_path())
        .args({"exit", "0"})
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    [[maybe_unused]] auto _ = proc->wait();

    CHECK_FALSE(proc->terminate());
}

// ── interrupt() ────────────────────────────────────────────────

#ifdef _WIN32
TEST_CASE("interrupt: Windows always returns false", "[signals][interrupt]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .own_process_group()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(200ms);

    CHECK_FALSE(proc->interrupt());

    proc->kill();
}
#endif

#ifndef _WIN32
TEST_CASE("interrupt: Unix own group + signal_trap → child exits 43", "[signals][interrupt]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .own_process_group()
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

TEST_CASE("interrupt: Unix inherit group + signal_trap → child exits 43", "[signals][interrupt]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .inherit_process_group()
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

// ── kill() ─────────────────────────────────────────────────────

TEST_CASE("kill: then wait_for returns Result and is_alive is false", "[signals][kill]") {
    auto proc = Command(helper_path())
        .args({"sleep", "30"})
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    CHECK(proc->kill());

    auto result = proc->wait_for(500ms);
    REQUIRE(result.has_value());
    CHECK_FALSE(proc->is_alive());
}

TEST_CASE("kill: own group + spawn_child → grandchild also dies", "[signals][kill]") {
    auto pid_file = unique_pid_file();

    auto proc = Command(helper_path())
        .args({"spawn_child", "30", pid_file.string()})
        .own_process_group()
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

#ifndef _WIN32
TEST_CASE("kill: Unix inherit group + spawn_child → grandchild survives", "[signals][kill]") {
    auto pid_file = unique_pid_file();

    auto proc = Command(helper_path())
        .args({"spawn_child", "30", pid_file.string()})
        .inherit_process_group()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());

    int grandchild = wait_for_pid_file(pid_file, 3s);
    REQUIRE(grandchild > 0);

    CHECK(proc->kill());
    std::this_thread::sleep_for(500ms);

    ProcessRef gc(grandchild);
    CHECK(gc.is_alive());  // documents the tradeoff — no group to cascade

    gc.kill();  // cleanup
    std::error_code ec;
    fs::remove(pid_file, ec);
}
#endif

#ifdef _WIN32
TEST_CASE("kill: Windows inherit group + spawn_child → grandchild dies via job", "[signals][kill]") {
    auto pid_file = unique_pid_file();

    auto proc = Command(helper_path())
        .args({"spawn_child", "30", pid_file.string()})
        .inherit_process_group()
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
#endif

// ── Composition (what stop() used to do) ───────────────────────

TEST_CASE("compose: terminate + wait_for reaps signal_trap child with exit 42", "[signals][compose]") {
    auto proc = Command(helper_path())
        .args({"signal_trap"})
        .own_process_group()
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

TEST_CASE("compose: terminate then kill reaps signal_ignore child", "[signals][compose]") {
    auto proc = Command(helper_path())
        .args({"signal_ignore"})
        .own_process_group()
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
