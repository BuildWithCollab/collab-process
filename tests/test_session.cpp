#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

#ifndef _WIN32
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

#ifndef _WIN32

TEST_CASE("session: new_session → child is its own session leader", "[session]") {
    auto proc = Command(helper_path())
        .args({"sleep", "30"})
        .new_session()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(100ms);  // let setsid() take effect

    int pid = proc->pid();
    CHECK(getsid(pid) == pid);

    proc->kill();
}

TEST_CASE("session: inherit → child stays in parent's session", "[session]") {
    auto proc = Command(helper_path())
        .args({"sleep", "30"})
        .inherit_session()
        .stdout_discard()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    std::this_thread::sleep_for(100ms);

    int pid = proc->pid();
    CHECK(getsid(pid) == getsid(0));

    proc->kill();
}

#endif  // !_WIN32

#ifdef _WIN32

TEST_CASE("session: new_session on Windows is a no-op", "[session]") {
    auto a = Command(helper_path())
        .args({"echo", "hello"})
        .new_session()
        .stdout_capture()
        .stderr_discard()
        .run();

    auto b = Command(helper_path())
        .args({"echo", "hello"})
        .inherit_session()
        .stdout_capture()
        .stderr_discard()
        .run();

    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->stdout_content == b->stdout_content);
    CHECK(a->stdout_content == "hello");
}

#endif  // _WIN32
