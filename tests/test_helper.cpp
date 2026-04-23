// test_helper — multi-mode test binary for collab-process tests
//
// Usage:
//   test_helper echo <text>         Print text to stdout
//   test_helper stderr <text>       Print text to stderr
//   test_helper both <out> <err>    Print to both streams
//   test_helper stdin               Read stdin, echo to stdout
//   test_helper env <VAR>           Print value of env var
//   test_helper exit <code>         Exit with given code
//   test_helper sleep <seconds>     Sleep then exit 0
//   test_helper cwd                 Print current working directory
//   test_helper flood <bytes>       Write N bytes to stdout (for deadlock tests)
//   test_helper sigint-exit         Install SIGINT handler: print GOT_SIGINT, exit 42
//   test_helper sigint-ignore       Install SIGINT handler: print GOT_SIGINT, keep running
//   test_helper sigint-default      No handler — default SIGINT action

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#define WRITE_STDOUT(buf, n) _write(1, buf, static_cast<unsigned int>(n))
#else
#include <unistd.h>
#define WRITE_STDOUT(buf, n) ::write(STDOUT_FILENO, buf, n)
#endif

namespace {

constexpr char kGotSigint[] = "GOT_SIGINT";

void print_got_sigint() {
    // Async-signal-safe write (iostream is not safe from a handler)
    WRITE_STDOUT(kGotSigint, sizeof(kGotSigint) - 1);
}

#ifdef _WIN32
BOOL WINAPI sigint_exit_handler(DWORD type) {
    if (type == CTRL_C_EVENT) {
        print_got_sigint();
        std::_Exit(42);
    }
    return FALSE;
}

BOOL WINAPI sigint_ignore_handler(DWORD type) {
    if (type == CTRL_C_EVENT) {
        print_got_sigint();
        return TRUE;  // handled — don't terminate
    }
    return FALSE;
}
#else
volatile std::sig_atomic_t got_sigint = 0;

void sigint_exit_sig_handler(int) {
    print_got_sigint();
    std::_Exit(42);
}

void sigint_ignore_sig_handler(int) {
    print_got_sigint();
    got_sigint = 1;
}
#endif

}  // namespace

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

    if (mode == "sigint-exit") {
#ifdef _WIN32
        SetConsoleCtrlHandler(sigint_exit_handler, TRUE);
#else
        std::signal(SIGINT, sigint_exit_sig_handler);
#endif
        // Signal readiness, then park waiting for the signal.
        std::cout << "READY" << std::flush;
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    if (mode == "sigint-ignore") {
#ifdef _WIN32
        SetConsoleCtrlHandler(sigint_ignore_handler, TRUE);
#else
        std::signal(SIGINT, sigint_ignore_sig_handler);
#endif
        std::cout << "READY" << std::flush;
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    if (mode == "sigint-default") {
        // No handler installed — default SIGINT action (terminate).
        std::cout << "READY" << std::flush;
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    std::cerr << "unknown mode: " << mode << std::endl;
    return 1;
}
