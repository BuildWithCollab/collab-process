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

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

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

    std::cerr << "unknown mode: " << mode << std::endl;
    return 1;
}
