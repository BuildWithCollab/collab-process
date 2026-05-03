#pragma once

#include <optional>
#include <string>

namespace collab::process {

struct SpawnError {
    enum Kind {
        command_not_found,
        permission_denied,
        pipe_creation_failed,
        invalid_working_directory,
        platform_error
    };

    Kind kind;
    int native_error = 0;

    auto what() const -> std::string;
};

// Returned by RunningProcess::write_stdin for runtime I/O failures.
// Contract violations (write to a non-pipe stdin, write after close_stdin,
// double close_stdin) throw ModeError instead — see running_process.hpp.
struct WriteError {
    enum Kind {
        broken_pipe,    // child closed stdin or has exited
        platform_error  // unexpected I/O failure (EIO, etc.)
    };

    Kind kind;
    int native_error = 0;  // errno (POSIX) or GetLastError() (Windows)

    auto what() const -> std::string;
};

struct Result {
    std::string stdout_content;
    std::string stderr_content;
    std::optional<int> exit_code;
    bool timed_out = false;

    auto ok() const -> bool { return exit_code == 0 && !timed_out; }
};

}  // namespace collab::process
