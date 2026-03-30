#include "collab/process/process_ref.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

namespace collab::process {

ProcessRef::ProcessRef(int pid) : pid_(pid) {}

auto ProcessRef::pid() const -> int { return pid_; }

auto ProcessRef::is_alive() const -> bool {
    if (pid_ <= 0) return false;

#ifdef _WIN32
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid_);
    if (!proc) return false;
    DWORD exit_code;
    GetExitCodeProcess(proc, &exit_code);
    CloseHandle(proc);
    return exit_code == STILL_ACTIVE;
#else
    return ::kill(pid_, 0) == 0;
#endif
}

auto ProcessRef::kill() -> bool {
    if (pid_ <= 0) return false;

#ifdef _WIN32
    HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid_);
    if (!proc) return false;
    BOOL ok = TerminateProcess(proc, 1);
    CloseHandle(proc);
    return ok != 0;
#else
    return ::kill(pid_, SIGKILL) == 0;
#endif
}

}  // namespace collab::process
