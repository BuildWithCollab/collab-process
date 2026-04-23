#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "collab/process/compat.hpp"

namespace collab::process {

struct CommandConfig {
    // What to run
    std::string program;
    std::vector<std::string> args;

    // Where to run
    std::filesystem::path working_dir;

    // Environment — additions/removals applied on top of parent env
    std::vector<std::pair<std::string, std::string>> env_add;
    std::vector<std::string> env_remove;
    bool env_clear = false;  // if true: start empty, only env_add vars

    // Stdout / Stderr
    enum class OutputMode { inherit, capture, discard };
    OutputMode stdout_mode = OutputMode::inherit;
    OutputMode stderr_mode = OutputMode::inherit;
    bool stderr_merge = false;  // merge stderr into stdout stream

    // Stdin
    enum class StdinMode { inherit, content, file, closed };
    StdinMode stdin_mode = StdinMode::inherit;
    std::string stdin_content;           // read when mode == content
    std::filesystem::path stdin_path;    // read when mode == file

    // Process group — controls signal-routing / tree-kill scoping.
    // inherit: join the parent's process group (child receives the same
    //   console-wide signals as the parent).
    // own: child becomes a process-group leader (Unix setpgid / Windows
    //   CREATE_NEW_PROCESS_GROUP). Required for group-scoped signal delivery.
    enum class ProcessGroup { inherit, own };

    // Session — Unix-only concept. Controls whether the child calls setsid()
    // to become its own session leader (detaches from the controlling terminal).
    // No-op on Windows.
    enum class Session { inherit, new_session };

    // Behavior
    std::chrono::milliseconds timeout{0};  // 0 = no timeout
    ProcessGroup process_group = ProcessGroup::inherit;
    Session session = Session::inherit;

    // Dotenv — load .env files into the child's environment
    bool dotenv = false;                   // false = no .env loading
};

struct IoCallbacks {
    collab::process::move_only_function<void(std::string_view)> on_stdout;
    collab::process::move_only_function<void(std::string_view)> on_stderr;
};

}  // namespace collab::process
