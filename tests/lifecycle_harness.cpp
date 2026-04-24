// lifecycle_harness — child process used by test_lifecycle to exercise
// abrupt-parent-death scenarios. It uses the collab-process library to
// spawn a target, prints the target's PID to stdout (the test reads it to
// know who to check afterward), and then either waits on the target or
// exits depending on the mode.
//
// modes:
//   spawn <helper_path> <secs>
//       Command(helper_path).args({"sleep", secs}).spawn() + wait().
//       Prints target PID. Used for "child dies when harness SIGKILL'd"
//       tests — the test kills this harness and the library's lifecycle
//       supervisor is expected to take the target with it.
//
//   spawn_detached <helper_path> <secs>
//       spawn_detached(). Prints PID. Exits 0 immediately. The target must
//       survive harness exit (that's the whole point of detached).
//
//   spawn_observe_detach <helper_path> <secs>
//       spawn() captured, then detach(). Prints PID. Exits 0. This is the
//       "observe-then-detach" case that prctl alone cannot handle on Linux
//       and is the reason the library uses a supervisor instead.

#include <collab/process.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace collab::process;
using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "usage: lifecycle_harness <mode> <helper_path> <secs>\n";
        return 1;
    }
    std::string mode        = argv[1];
    std::string helper_path = argv[2];
    std::string secs        = argv[3];

    if (mode == "spawn") {
        auto proc = Command(helper_path).args({"sleep", secs})
            .stdout_discard().stderr_discard().spawn();
        if (!proc) { std::cerr << "spawn failed\n"; return 2; }
        std::cout << proc->pid() << std::endl;
        std::cout.flush();
        // Block on wait so the harness stays alive until killed externally.
        (void)proc->wait();
        return 0;
    }

    if (mode == "spawn_detached") {
        auto pid = Command(helper_path).args({"sleep", secs})
            .stdout_discard().stderr_discard().spawn_detached();
        if (!pid) { std::cerr << "spawn_detached failed\n"; return 2; }
        std::cout << *pid << std::endl;
        std::cout.flush();
        std::this_thread::sleep_for(500ms);
        return 0;
    }

    if (mode == "spawn_observe_detach") {
        auto proc = Command(helper_path).args({"sleep", secs})
            .stdout_capture().stderr_discard().spawn();
        if (!proc) { std::cerr << "spawn failed\n"; return 2; }
        int pid = proc->pid();
        std::cout << pid << std::endl;
        std::cout.flush();
        (void)std::move(*proc).detach();
        return 0;
    }

    std::cerr << "unknown mode: " << mode << "\n";
    return 1;
}
