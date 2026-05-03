// Integration tests for StdinMode::pipe + RunningProcess::write_stdin /
// close_stdin. Covers configuration, basic writes, EOF semantics, contract
// violations, runtime I/O errors, callback round-trips (the JSON-RPC use
// case), thread safety, detach, POSIX SIGPIPE preservation, backpressure,
// RAII teardown, and stderr-alongside regression.

#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>
#include <collab/process/mode_error.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
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

template <typename F>
static auto poll_until(std::chrono::milliseconds timeout, F&& pred) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

// ── Group A — Configuration / opt-in ────────────────────────────

TEST_CASE("stdin_pipe: builder method sets StdinMode::pipe", "[stdin_pipe][config]") {
    auto cmd = Command(helper_path()).args({"stdin_count"}).stdin_pipe();
    // The builder is meant to be consumed on execute, but we can inspect
    // the underlying config via spawn() returning a working pipe.
    auto proc = std::move(cmd).stdout_capture().stderr_discard().spawn();
    REQUIRE(proc.has_value());
    auto write = proc->write_stdin("");
    CHECK(write.has_value());  // empty write on a configured pipe = success
    proc->close_stdin();
    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
}

TEST_CASE("stdin_pipe: CommandConfig{StdinMode::pipe} produces a writable pipe", "[stdin_pipe][config]") {
    CommandConfig config;
    config.program = helper_path();
    config.args = {"stdin_count"};
    config.stdin_mode = CommandConfig::StdinMode::pipe;
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto proc = collab::process::spawn(config);
    REQUIRE(proc.has_value());
    auto write = proc->write_stdin("hi");
    CHECK(write.has_value());
    proc->close_stdin();
    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("got 2 bytes") != std::string::npos);
}

// ── Group B — Basic write semantics ─────────────────────────────

TEST_CASE("stdin_pipe: single write round-trip via stdin_echo", "[stdin_pipe][basic]") {
    std::mutex mtx;
    std::string received;
    std::atomic<int> chunks{0};

    auto proc = Command(helper_path()).args({"stdin_echo"})
        .stdin_pipe()
        .stdout_capture()
        .stdout_callback([&](std::string_view chunk) {
            std::lock_guard lock(mtx);
            received += chunk;
            chunks.fetch_add(1);
        })
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    auto write = proc->write_stdin("hello\n");
    REQUIRE(write.has_value());

    // Wait for the response to come back via the callback.
    REQUIRE(poll_until(3s, [&] {
        std::lock_guard lock(mtx);
        return received.find("echo: hello") != std::string::npos;
    }));

    proc->close_stdin();
    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
}

TEST_CASE("stdin_pipe: multiple sequential writes preserved in order", "[stdin_pipe][basic]") {
    auto proc = Command(helper_path()).args({"stdin_count_lines"})
        .stdin_pipe()
        .stdout_capture()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    REQUIRE(proc->write_stdin("first\n").has_value());
    REQUIRE(proc->write_stdin("second\n").has_value());
    REQUIRE(proc->write_stdin("third\n").has_value());
    proc->close_stdin();

    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("got 3 lines") != std::string::npos);
}

TEST_CASE("stdin_pipe: empty string_view is no-op success", "[stdin_pipe][basic]") {
    auto proc = Command(helper_path()).args({"stdin_count"})
        .stdin_pipe()
        .stdout_capture()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    auto r1 = proc->write_stdin(std::string_view{});
    auto r2 = proc->write_stdin("");
    CHECK(r1.has_value());
    CHECK(r2.has_value());

    proc->close_stdin();
    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("got 0 bytes") != std::string::npos);
}

TEST_CASE("stdin_pipe: binary / non-UTF-8 bytes pass through unchanged", "[stdin_pipe][basic]") {
    auto proc = Command(helper_path()).args({"stdin_count"})
        .stdin_pipe()
        .stdout_capture()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    const char raw[] = {char(0x00), char(0xff), char(0x01), char(0x80), char(0x7f)};
    auto r = proc->write_stdin(std::string_view{raw, sizeof(raw)});
    REQUIRE(r.has_value());
    proc->close_stdin();

    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("got 5 bytes") != std::string::npos);
}

TEST_CASE("stdin_pipe: write larger than PIPE_BUF (8 KB) delivered intact", "[stdin_pipe][basic]") {
    auto proc = Command(helper_path()).args({"stdin_count"})
        .stdin_pipe()
        .stdout_capture()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    constexpr size_t big = 8 * 1024;
    std::string buf(big, 'A');
    auto r = proc->write_stdin(buf);
    REQUIRE(r.has_value());
    proc->close_stdin();

    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("got " + std::to_string(big) + " bytes") != std::string::npos);
}

TEST_CASE("stdin_pipe: 4 MB write across many kernel buffer cycles", "[stdin_pipe][basic][large]") {
    auto proc = Command(helper_path()).args({"stdin_count"})
        .stdin_pipe()
        .stdout_capture()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    constexpr size_t megs = 4 * 1024 * 1024;
    std::string buf(megs, 'X');
    auto r = proc->write_stdin(buf);
    REQUIRE(r.has_value());
    proc->close_stdin();

    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("got " + std::to_string(megs) + " bytes") != std::string::npos);
}

// ── Group C — close_stdin semantics ─────────────────────────────

TEST_CASE("stdin_pipe: close_stdin causes child to see EOF", "[stdin_pipe][close]") {
    auto proc = Command(helper_path()).args({"stdin_count"})
        .stdin_pipe()
        .stdout_capture()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    REQUIRE(proc->write_stdin("12345").has_value());
    proc->close_stdin();

    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("got 5 bytes") != std::string::npos);
}

TEST_CASE("stdin_pipe: pending writes complete before close_stdin", "[stdin_pipe][close]") {
    auto proc = Command(helper_path()).args({"stdin_count"})
        .stdin_pipe()
        .stdout_capture()
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    constexpr size_t total = 256 * 1024;
    std::string buf(total, 'Z');
    REQUIRE(proc->write_stdin(buf).has_value());
    proc->close_stdin();

    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("got " + std::to_string(total) + " bytes") != std::string::npos);
}

// ── Group D — ModeError contract violations ─────────────────────

TEST_CASE("stdin_pipe: write_stdin throws ModeError on inherit mode", "[stdin_pipe][mode]") {
    auto proc = Command(helper_path()).args({"sleep", "5"})
        .stdout_discard().stderr_discard().spawn();
    REQUIRE(proc.has_value());
    CHECK_THROWS_AS(proc->write_stdin("x"), ModeError);
    proc->kill();
}

TEST_CASE("stdin_pipe: write_stdin throws ModeError on content mode", "[stdin_pipe][mode]") {
    auto proc = Command(helper_path()).args({"stdin"})
        .stdin_string("preset\n")
        .stdout_capture().stderr_discard().spawn();
    REQUIRE(proc.has_value());
    CHECK_THROWS_AS(proc->write_stdin("more"), ModeError);
    (void)proc->wait();
}

TEST_CASE("stdin_pipe: write_stdin throws ModeError on file mode", "[stdin_pipe][mode]") {
    auto temp = fs::temp_directory_path() / "collab_pipe_modeerr.txt";
    { std::ofstream f(temp); f << "x\n"; }

    auto proc = Command(helper_path()).args({"stdin"})
        .stdin_file(temp)
        .stdout_capture().stderr_discard().spawn();
    REQUIRE(proc.has_value());
    CHECK_THROWS_AS(proc->write_stdin("more"), ModeError);
    (void)proc->wait();
    fs::remove(temp);
}

TEST_CASE("stdin_pipe: write_stdin throws ModeError on closed mode", "[stdin_pipe][mode]") {
    auto proc = Command(helper_path()).args({"stdin"})
        .stdin_close()
        .stdout_capture().stderr_discard().spawn();
    REQUIRE(proc.has_value());
    CHECK_THROWS_AS(proc->write_stdin("x"), ModeError);
    (void)proc->wait();
}

TEST_CASE("stdin_pipe: close_stdin throws ModeError on non-pipe modes", "[stdin_pipe][mode]") {
    auto proc = Command(helper_path()).args({"sleep", "5"})
        .stdout_discard().stderr_discard().spawn();
    REQUIRE(proc.has_value());
    CHECK_THROWS_AS(proc->close_stdin(), ModeError);
    proc->kill();
}

TEST_CASE("stdin_pipe: write_stdin after close_stdin throws ModeError", "[stdin_pipe][mode]") {
    auto proc = Command(helper_path()).args({"stdin_count"})
        .stdin_pipe()
        .stdout_capture().stderr_discard().spawn();
    REQUIRE(proc.has_value());
    proc->close_stdin();
    CHECK_THROWS_AS(proc->write_stdin("x"), ModeError);
    (void)proc->wait();
}

TEST_CASE("stdin_pipe: second close_stdin throws ModeError", "[stdin_pipe][mode]") {
    auto proc = Command(helper_path()).args({"stdin_count"})
        .stdin_pipe()
        .stdout_capture().stderr_discard().spawn();
    REQUIRE(proc.has_value());
    proc->close_stdin();
    CHECK_THROWS_AS(proc->close_stdin(), ModeError);
    (void)proc->wait();
}

TEST_CASE("stdin_pipe: ModeError is a std::logic_error", "[stdin_pipe][mode]") {
    auto proc = Command(helper_path()).args({"sleep", "5"})
        .stdout_discard().stderr_discard().spawn();
    REQUIRE(proc.has_value());
    bool caught_as_logic = false;
    try {
        (void)proc->write_stdin("x");
    } catch (const std::logic_error&) {
        caught_as_logic = true;
    }
    CHECK(caught_as_logic);
    proc->kill();
}

TEST_CASE("stdin_pipe: ModeError::what() names the method", "[stdin_pipe][mode]") {
    auto proc = Command(helper_path()).args({"sleep", "5"})
        .stdout_discard().stderr_discard().spawn();
    REQUIRE(proc.has_value());

    try {
        (void)proc->write_stdin("x");
        FAIL("expected ModeError");
    } catch (const ModeError& e) {
        std::string msg = e.what();
        CHECK(msg.find("write_stdin") != std::string::npos);
        CHECK(msg.find("pipe") != std::string::npos);
    }

    try {
        proc->close_stdin();
        FAIL("expected ModeError");
    } catch (const ModeError& e) {
        std::string msg = e.what();
        CHECK(msg.find("close_stdin") != std::string::npos);
    }

    proc->kill();
}

// ── Group E — WriteError runtime failures ───────────────────────

TEST_CASE("stdin_pipe: write_stdin after kill returns broken_pipe", "[stdin_pipe][error]") {
    auto proc = Command(helper_path()).args({"stdin_echo"})
        .stdin_pipe()
        .stdout_discard()
        .stderr_discard()
        .spawn();
    REQUIRE(proc.has_value());

    proc->kill();
    // Give the kernel a moment to tear down the read end so subsequent
    // writes fail deterministically rather than landing in the pipe buffer.
    REQUIRE(poll_until(2s, [&] { return !proc->is_alive(); }));
    std::this_thread::sleep_for(50ms);

    // Write enough bytes that even a partially-still-open buffer will
    // overflow and surface broken_pipe. The first small write may succeed
    // before the pipe tears down on Windows; loop a few times to converge.
    bool got_broken = false;
    for (int i = 0; i < 20 && !got_broken; ++i) {
        std::string blob(64 * 1024, 'B');
        auto r = proc->write_stdin(blob);
        if (!r.has_value()) {
            CHECK(r.error().kind == WriteError::broken_pipe);
            got_broken = true;
        }
    }
    CHECK(got_broken);
}

TEST_CASE("stdin_pipe: WriteError::what() returns a non-empty diagnostic", "[stdin_pipe][error]") {
    auto proc = Command(helper_path()).args({"stdin_echo"})
        .stdin_pipe()
        .stdout_discard().stderr_discard().spawn();
    REQUIRE(proc.has_value());

    proc->kill();
    REQUIRE(poll_until(2s, [&] { return !proc->is_alive(); }));
    std::this_thread::sleep_for(50ms);

    for (int i = 0; i < 20; ++i) {
        std::string blob(64 * 1024, 'B');
        auto r = proc->write_stdin(blob);
        if (!r.has_value()) {
            std::string msg = r.error().what();
            CHECK_FALSE(msg.empty());
            return;
        }
    }
    FAIL("never observed broken_pipe error");
}

// ── Group F — Concurrent-with-reads / callback round-trip ───────

TEST_CASE("stdin_pipe: write_stdin from inside on_stdout callback (JSON-RPC pattern)",
          "[stdin_pipe][callback]") {
    constexpr int target_responses = 5;
    std::atomic<int> responses{0};
    std::mutex mtx;
    std::string received;
    RunningProcess* proc_ptr = nullptr;

    auto cb = [&](std::string_view chunk) {
        std::lock_guard lock(mtx);
        received += chunk;
        // Each "echo: <line>\n" we see is one response. Count newlines.
        for (char c : chunk) {
            if (c == '\n') {
                int seen = ++responses;
                if (seen < target_responses && proc_ptr) {
                    std::string next = "req" + std::to_string(seen) + "\n";
                    (void)proc_ptr->write_stdin(next);
                }
            }
        }
    };

    auto proc = Command(helper_path()).args({"stdin_echo"})
        .stdin_pipe()
        .stdout_capture()
        .stdout_callback(std::move(cb))
        .stderr_discard()
        .spawn();

    REQUIRE(proc.has_value());
    proc_ptr = &(*proc);

    // Kick off the conversation. The callback drives subsequent rounds.
    REQUIRE(proc_ptr->write_stdin("req0\n").has_value());

    REQUIRE(poll_until(5s, [&] { return responses.load() >= target_responses; }));

    proc_ptr->close_stdin();
    auto result = proc_ptr->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());

    std::lock_guard lock(mtx);
    CHECK(received.find("echo: req0") != std::string::npos);
    CHECK(received.find("echo: req4") != std::string::npos);
}

// ── Group G — Thread safety ─────────────────────────────────────

TEST_CASE("stdin_pipe: concurrent writes from two threads serialize cleanly",
          "[stdin_pipe][concurrency]") {
    auto proc = Command(helper_path()).args({"stdin_count"})
        .stdin_pipe()
        .stdout_capture().stderr_discard().spawn();
    REQUIRE(proc.has_value());

    constexpr size_t per_thread = 64 * 1024;
    std::string a_buf(per_thread, 'A');
    std::string b_buf(per_thread, 'B');

    std::thread ta([&] { (void)proc->write_stdin(a_buf); });
    std::thread tb([&] { (void)proc->write_stdin(b_buf); });
    ta.join();
    tb.join();

    proc->close_stdin();
    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("got " + std::to_string(2 * per_thread) + " bytes") != std::string::npos);
}

TEST_CASE("stdin_pipe: concurrent line writers produce no split lines",
          "[stdin_pipe][concurrency]") {
    auto proc = Command(helper_path()).args({"stdin_passthrough"})
        .stdin_pipe()
        .stdout_capture().stderr_discard().spawn();
    REQUIRE(proc.has_value());

    constexpr int per_thread = 200;
    auto writer = [&](char tag) {
        for (int i = 0; i < per_thread; ++i) {
            // Each call is one whole line; the mutex guarantees no other
            // thread's bytes splice in between.
            std::string line;
            line.push_back(tag);
            line += ":line:";
            line += std::to_string(i);
            line.push_back('\n');
            (void)proc->write_stdin(line);
        }
    };

    std::thread ta(writer, 'A');
    std::thread tb(writer, 'B');
    ta.join();
    tb.join();

    proc->close_stdin();
    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());

    // stdin_passthrough echoes each line as "<i>:<line>\n". Count lines —
    // any spliced line would still be a line, so this catches missing data
    // but not interleaving directly. Check for the marker fragment though.
    int a_count = 0, b_count = 0;
    for (size_t pos = 0; (pos = result->stdout_content.find(":A:line:", pos)) != std::string::npos; ++pos)
        ++a_count;
    for (size_t pos = 0; (pos = result->stdout_content.find(":B:line:", pos)) != std::string::npos; ++pos)
        ++b_count;
    CHECK(a_count == per_thread);
    CHECK(b_count == per_thread);
}

// ── Group H — Detach ────────────────────────────────────────────

TEST_CASE("stdin_pipe: detach closes stdin so child sees EOF", "[stdin_pipe][detach]") {
    auto proc = Command(helper_path()).args({"stdin_count"})
        .stdin_pipe()
        .stdout_discard()  // detached child loses our pipe readers; don't capture
        .stderr_discard()
        .spawn();
    REQUIRE(proc.has_value());
    REQUIRE(proc->write_stdin("abcde").has_value());

    int pid = std::move(*proc).detach();
    CHECK(pid > 0);

    // Detached child should reach EOF on stdin and exit cleanly.
    auto ref = ProcessRef(pid);
    CHECK(poll_until(3s, [&] { return !ref.is_alive(); }));
}

// ── Group I — POSIX SIGPIPE ─────────────────────────────────────

#ifndef _WIN32

extern "C" void test_sigpipe_marker_handler(int) {}

TEST_CASE("stdin_pipe: POSIX broken pipe does not crash via SIGPIPE", "[stdin_pipe][posix]") {
    auto proc = Command(helper_path()).args({"stdin_echo"})
        .stdin_pipe()
        .stdout_discard().stderr_discard().spawn();
    REQUIRE(proc.has_value());

    proc->kill();
    REQUIRE(poll_until(2s, [&] { return !proc->is_alive(); }));
    std::this_thread::sleep_for(50ms);

    bool got_broken = false;
    for (int i = 0; i < 20 && !got_broken; ++i) {
        std::string blob(64 * 1024, 'C');
        auto r = proc->write_stdin(blob);
        if (!r.has_value()) {
            CHECK(r.error().kind == WriteError::broken_pipe);
            got_broken = true;
        }
    }
    CHECK(got_broken);
    // If SIGPIPE had killed us, we would not reach this line.
    CHECK(true);
}

TEST_CASE("stdin_pipe: POSIX user SIGPIPE handler preserved across broken-pipe write",
          "[stdin_pipe][posix]") {
    struct sigaction installed{}, original{};
    installed.sa_handler = test_sigpipe_marker_handler;
    sigemptyset(&installed.sa_mask);
    installed.sa_flags = 0;
    REQUIRE(::sigaction(SIGPIPE, &installed, &original) == 0);

    {
        auto proc = Command(helper_path()).args({"stdin_echo"})
            .stdin_pipe()
            .stdout_discard().stderr_discard().spawn();
        REQUIRE(proc.has_value());

        proc->kill();
        REQUIRE(poll_until(2s, [&] { return !proc->is_alive(); }));
        std::this_thread::sleep_for(50ms);

        for (int i = 0; i < 20; ++i) {
            std::string blob(64 * 1024, 'D');
            auto r = proc->write_stdin(blob);
            if (!r.has_value()) break;
        }
    }

    struct sigaction current{};
    REQUIRE(::sigaction(SIGPIPE, nullptr, &current) == 0);
    CHECK(current.sa_handler == test_sigpipe_marker_handler);

    // Restore original.
    ::sigaction(SIGPIPE, &original, nullptr);
}

#endif  // !_WIN32

// ── Group J — Backpressure ──────────────────────────────────────

TEST_CASE("stdin_pipe: slow child drain blocks parent's write correctly", "[stdin_pipe][backpressure]") {
    // Child reads a 4KB chunk every 30ms. A 256 KB write requires the
    // child to drain the kernel pipe buffer multiple times before
    // write_stdin can return.
    auto proc = Command(helper_path()).args({"stdin_slow_drain", "30"})
        .stdin_pipe()
        .stdout_capture().stderr_discard().spawn();
    REQUIRE(proc.has_value());

    constexpr size_t total = 256 * 1024;
    std::string buf(total, 'S');

    auto t0 = std::chrono::steady_clock::now();
    auto r = proc->write_stdin(buf);
    auto elapsed = std::chrono::steady_clock::now() - t0;
    REQUIRE(r.has_value());

    proc->close_stdin();
    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("got " + std::to_string(total) + " bytes") != std::string::npos);

    // Loose timing: should take meaningfully longer than instant.
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    CHECK(ms >= 100);  // generous lower bound — actual is several hundred ms
}

// ── Group K — Destructor / RAII ─────────────────────────────────

TEST_CASE("stdin_pipe: destructor cleans up open stdin without hanging",
          "[stdin_pipe][raii]") {
    auto t0 = std::chrono::steady_clock::now();
    {
        auto proc = Command(helper_path()).args({"stdin_echo"})
            .stdin_pipe()
            .stdout_discard().stderr_discard().spawn();
        REQUIRE(proc.has_value());
        REQUIRE(proc->write_stdin("a\n").has_value());
        // Don't call close_stdin or wait — let the destructor handle it.
    }
    auto elapsed = std::chrono::steady_clock::now() - t0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    // Destructor should kill the child + close fds well under 5 seconds.
    CHECK(ms < 5000);
}

// ── Group L — Stderr alongside ─────────────────────────────────

TEST_CASE("stdin_pipe: round-trip with stdout + stderr both captured",
          "[stdin_pipe][regression]") {
    std::mutex mtx;
    std::string out_received;
    std::string err_received;
    std::atomic<int> out_lines{0};
    std::atomic<int> err_lines{0};

    auto proc = Command(helper_path()).args({"stdin_echo_with_stderr"})
        .stdin_pipe()
        .stdout_capture()
        .stderr_capture()
        .stdout_callback([&](std::string_view chunk) {
            std::lock_guard lock(mtx);
            out_received += chunk;
            for (char c : chunk) if (c == '\n') out_lines.fetch_add(1);
        })
        .stderr_callback([&](std::string_view chunk) {
            std::lock_guard lock(mtx);
            err_received += chunk;
            for (char c : chunk) if (c == '\n') err_lines.fetch_add(1);
        })
        .spawn();

    REQUIRE(proc.has_value());
    for (int i = 0; i < 5; ++i)
        REQUIRE(proc->write_stdin("x" + std::to_string(i) + "\n").has_value());

    REQUIRE(poll_until(3s, [&] {
        return out_lines.load() >= 5 && err_lines.load() >= 5;
    }));

    proc->close_stdin();
    auto result = proc->wait();
    REQUIRE(result.has_value());
    CHECK(result->ok());

    std::lock_guard lock(mtx);
    CHECK(out_received.find("echo: x0") != std::string::npos);
    CHECK(out_received.find("echo: x4") != std::string::npos);
    CHECK(err_received.find("err: x0") != std::string::npos);
    CHECK(err_received.find("err: x4") != std::string::npos);
}
