// no_console_harness — Windows-only harness that simulates a GUI host
// (parent process without an attached console) for the regression test on
// the "headless spawn pops a console window" bug.
//
// Flow:
//   1. The test runner spawns this harness with stdout_capture(). At this
//      point the harness still has the inherited console.
//   2. The harness calls FreeConsole() to detach itself. After this,
//      GetConsoleWindow() returns nullptr — the same condition a Qt GUI
//      app sees at startup.
//   3. The harness uses collab::process to spawn test_helper with
//      .headless().stdout_capture(). With the fix in place, the library
//      detects the absent parent console and applies CREATE_NO_WINDOW;
//      without the fix, Windows allocates a fresh console window for
//      test_helper.
//   4. The harness writes the captured "console=yes|no" line from
//      test_helper to its own stdout (a pipe to the test runner — the
//      pipe HANDLE survives FreeConsole). The test asserts "console=no".
//
// Args: <test_helper_path>

#ifdef _WIN32

#include <collab/process.hpp>

#include <windows.h>

#include <cstdio>
#include <iostream>
#include <string>

using namespace collab::process;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: no_console_harness <test_helper_path>\n");
        return 1;
    }

    if (!FreeConsole()) {
        // Not fatal if there was no console to begin with, but log it.
        std::fprintf(stderr, "FreeConsole failed: GLE=%lu\n", GetLastError());
    }
    if (GetConsoleWindow() != nullptr) {
        std::fprintf(stderr, "harness still has a console after FreeConsole\n");
        return 2;
    }

    auto result = Command(argv[1])
        .args({"console_status"})
        .headless()
        .stdout_capture()
        .stderr_discard()
        .run();

    if (!result) {
        std::fprintf(stderr, "spawn failed: %s\n", result.error().what().c_str());
        return 3;
    }
    if (!result->ok()) {
        std::fprintf(stderr, "test_helper exited %d\n",
                     result->exit_code.value_or(-1));
        return 4;
    }

    std::cout << result->stdout_content << std::flush;
    return 0;
}

#else

int main() { return 0; }  // Non-Windows: harness is unused.

#endif
