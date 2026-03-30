#pragma once

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

struct Result {
    std::string stdout_content;
    std::string stderr_content;
    int exit_code = -1;
    bool timed_out = false;

    auto ok() const -> bool { return exit_code == 0 && !timed_out; }
};

enum class StopResult {
    stopped_gracefully,
    killed,
    not_running,
    failed
};

}  // namespace collab::process
