#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    // Stdin — no enum, determined by what you set:
    //   nothing          → inherit (passthrough from terminal)
    //   stdin_content    → pipe string in
    //   stdin_path       → pipe file in
    //   stdin_closed     → close immediately
    std::string stdin_content;
    std::filesystem::path stdin_path;
    bool stdin_closed = false;

    // Behavior
    std::chrono::milliseconds timeout{0};  // 0 = no timeout
    bool detached = false;                 // child survives parent
};

struct IoCallbacks {
    std::move_only_function<void(std::string_view)> on_stdout;
    std::move_only_function<void(std::string_view)> on_stderr;
};

}  // namespace collab::process
