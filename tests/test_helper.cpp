// test_helper — multi-mode test binary for collab-process tests
//
// Usage:
//   test_helper echo <text>          Print text to stdout
//   test_helper stderr <text>        Print text to stderr
//   test_helper both <out> <err>     Print to both streams
//   test_helper stdin                Read stdin, echo to stdout
//   test_helper env <VAR>            Print value of env var
//   test_helper exit <code>          Exit with given code
//   test_helper sleep <seconds>      Sleep then exit 0
//   test_helper cwd                  Print current working directory
//   test_helper flood <bytes>        Write N bytes to stdout (for deadlock tests)
//   test_helper signal_trap          Handle termination/interrupt signals.
//                                    Exit 42 on SIGTERM / CTRL_BREAK,
//                                    exit 43 on SIGINT  / CTRL_C.
//                                    Otherwise sleep 30s.
//   test_helper signal_ignore        Ignore the same set of signals. Sleep 30s.
//   test_helper spawn_child <secs> [pid_file]
//                                    Spawn a sleep <secs> grandchild (same
//                                    binary, sleep mode) and wait on it.
//                                    If pid_file is given, write the
//                                    grandchild PID to that file — lets tests
//                                    observe the PID without needing live
//                                    pipe reading, which the current impl
//                                    only does during wait().
//   test_helper stdin_echo           Read lines, write "echo: <line>\n"
//                                    flushed after every line, until EOF.
//                                    For round-trip / interleaved tests.
//   test_helper stdin_count          Read all stdin to EOF, write
//                                    "got <N> bytes\n", exit 0.
//                                    For close_stdin EOF + integrity tests.
//   test_helper stdin_count_lines    Read lines to EOF, write
//                                    "got <N> lines\n", exit 0.
//   test_helper stdin_passthrough    Read lines, write "<i>:<line>\n"
//                                    flushed after every line. Sequence
//                                    number lets tests detect interleaved
//                                    bytes from concurrent writers.
//   test_helper stdin_slow_drain <ms_per_chunk>
//                                    Read 4KB chunks, sleep <ms> between,
//                                    write "got <N> bytes\n" at EOF. For
//                                    backpressure tests.
//   test_helper stdin_echo_with_stderr
//                                    Per line, write "echo: <line>\n" to
//                                    stdout AND "err: <line>\n" to stderr,
//                                    both flushed.
//   test_helper console_status       Windows: print "console=yes" if
//                                    GetConsoleWindow() returns non-null,
//                                    "console=no" otherwise. Used to verify
//                                    CREATE_NO_WINDOW is applied for
//                                    headless children of console-less
//                                    parents (e.g. GUI hosts). Non-Windows
//                                    builds print "console=na".

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// ── Signal handling ──────────────────────────────────────────────────────
//
// Handlers installed by signal_trap: map platform signals onto two exit codes
// so tests can verify *which* signal was delivered, not just that something
// terminated the child.

#ifdef _WIN32

static BOOL WINAPI ctrl_handler_trap(DWORD ctrl_type) {
    if (ctrl_type == CTRL_BREAK_EVENT) {
        ExitProcess(42);
    }
    if (ctrl_type == CTRL_C_EVENT) {
        ExitProcess(43);
    }
    return FALSE;
}

static BOOL WINAPI ctrl_handler_ignore(DWORD ctrl_type) {
    // Returning TRUE tells Windows we've handled it — do not terminate.
    if (ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_C_EVENT)
        return TRUE;
    return FALSE;
}

static void install_trap_handlers() {
    SetConsoleCtrlHandler(ctrl_handler_trap, TRUE);
}

static void install_ignore_handlers() {
    SetConsoleCtrlHandler(ctrl_handler_ignore, TRUE);
}

#else  // POSIX

extern "C" void trap_sigterm(int) { _exit(42); }
extern "C" void trap_sigint(int)  { _exit(43); }

static void install_trap_handlers() {
    ::signal(SIGTERM, trap_sigterm);
    ::signal(SIGINT,  trap_sigint);
}

static void install_ignore_handlers() {
    ::signal(SIGTERM, SIG_IGN);
    ::signal(SIGINT,  SIG_IGN);
}

#endif

// ── Grandchild spawning ──────────────────────────────────────────────────
//
// Self-spawn with "sleep <secs>" so we don't depend on /bin/sleep or
// Windows' timeout.exe. Returns grandchild PID via stdout; caller waits.

static void write_pid_file(const char* path, int pid) {
    if (!path) return;
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "%d", pid);
    std::fclose(f);
}

#ifdef _WIN32

static int spawn_grandchild(const char* self_path, int seconds, const char* pid_file) {
    std::string cmd = std::string("\"") + self_path
                    + "\" sleep " + std::to_string(seconds);
    std::wstring wcmd(cmd.begin(), cmd.end());

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::cerr << "spawn_grandchild: CreateProcessW failed, GLE="
                  << GetLastError() << "\n";
        return 1;
    }
    CloseHandle(pi.hThread);

    write_pid_file(pid_file, static_cast<int>(pi.dwProcessId));

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    return 0;
}

#else

static int spawn_grandchild(const char* self_path, int seconds, const char* pid_file) {
    pid_t child = fork();
    if (child < 0) {
        std::perror("fork");
        return 1;
    }
    if (child == 0) {
        std::string secs = std::to_string(seconds);
        execl(self_path, self_path, "sleep", secs.c_str(), (char*)nullptr);
        _exit(127);
    }

    write_pid_file(pid_file, static_cast<int>(child));

    int status = 0;
    waitpid(child, &status, 0);
    return 0;
}

#endif

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "usage: test_helper <mode> [args...]" << std::endl;
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "echo" && argc >= 3) {
        std::cout << argv[2];
        return 0;
    }

    if (mode == "stderr" && argc >= 3) {
        std::cerr << argv[2];
        return 0;
    }

    if (mode == "both" && argc >= 4) {
        std::cout << argv[2];
        std::cerr << argv[3];
        return 0;
    }

    if (mode == "stdin") {
        std::string line;
        while (std::getline(std::cin, line))
            std::cout << line << "\n";
        return 0;
    }

    if (mode == "env" && argc >= 3) {
        const char* val = std::getenv(argv[2]);
        if (val)
            std::cout << val;
        return val ? 0 : 1;
    }

    if (mode == "exit" && argc >= 3) {
        return std::atoi(argv[2]);
    }

    if (mode == "sleep" && argc >= 3) {
        int seconds = std::atoi(argv[2]);
        std::this_thread::sleep_for(std::chrono::seconds{seconds});
        return 0;
    }

    if (mode == "cwd") {
        std::cout << std::filesystem::current_path().string();
        return 0;
    }

    if (mode == "flood" && argc >= 3) {
        int bytes = std::atoi(argv[2]);
        std::string chunk(1024, 'X');
        int written = 0;
        while (written < bytes) {
            int to_write = std::min(static_cast<int>(chunk.size()), bytes - written);
            std::cout.write(chunk.data(), to_write);
            written += to_write;
        }
        return 0;
    }

    if (mode == "signal_trap") {
        install_trap_handlers();
        std::this_thread::sleep_for(std::chrono::seconds{30});
        return 0;
    }

    if (mode == "signal_ignore") {
        install_ignore_handlers();
        std::this_thread::sleep_for(std::chrono::seconds{30});
        return 0;
    }

    if (mode == "spawn_child" && argc >= 3) {
        const char* pid_file = argc >= 4 ? argv[3] : nullptr;
        return spawn_grandchild(argv[0], std::atoi(argv[2]), pid_file);
    }

    if (mode == "stdin_echo") {
        std::string line;
        while (std::getline(std::cin, line))
            std::cout << "echo: " << line << "\n" << std::flush;
        return 0;
    }

    if (mode == "stdin_count") {
        // Binary-safe byte counter; reads in chunks until EOF. Without
        // binary mode on Windows, \r\n→\n translation would distort byte
        // counts when tests write CRLF or arbitrary binary data.
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        std::size_t total = 0;
        char buf[4096];
        while (std::cin.read(buf, sizeof(buf)) || std::cin.gcount() > 0) {
            total += static_cast<std::size_t>(std::cin.gcount());
            if (std::cin.eof()) break;
        }
        std::cout << "got " << total << " bytes\n" << std::flush;
        return 0;
    }

    if (mode == "stdin_count_lines") {
        std::string line;
        std::size_t lines = 0;
        while (std::getline(std::cin, line))
            ++lines;
        std::cout << "got " << lines << " lines\n" << std::flush;
        return 0;
    }

    if (mode == "stdin_passthrough") {
        std::string line;
        std::size_t i = 0;
        while (std::getline(std::cin, line))
            std::cout << i++ << ":" << line << "\n" << std::flush;
        return 0;
    }

    if (mode == "stdin_slow_drain" && argc >= 3) {
        int ms = std::atoi(argv[2]);
        // Read in 4KB chunks with a sleep between each — used by tests to
        // verify the parent's write_stdin blocks correctly when the kernel
        // pipe buffer fills up.
#ifdef _WIN32
        // Windows console mode would line-buffer stdin; binary mode avoids
        // CRLF translation that breaks byte counts.
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        std::size_t total = 0;
        char buf[4096];
        for (;;) {
            std::cin.read(buf, sizeof(buf));
            std::streamsize n = std::cin.gcount();
            if (n <= 0) break;
            total += static_cast<std::size_t>(n);
            std::this_thread::sleep_for(std::chrono::milliseconds{ms});
            if (std::cin.eof()) break;
        }
        std::cout << "got " << total << " bytes\n" << std::flush;
        return 0;
    }

    if (mode == "stdin_echo_with_stderr") {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::cout << "echo: " << line << "\n" << std::flush;
            std::cerr << "err: "  << line << "\n" << std::flush;
        }
        return 0;
    }

    if (mode == "console_status") {
#ifdef _WIN32
        std::cout << (GetConsoleWindow() ? "console=yes" : "console=no")
                  << std::flush;
#else
        std::cout << "console=na" << std::flush;
#endif
        return 0;
    }

    std::cerr << "unknown mode: " << mode << std::endl;
    return 1;
}
