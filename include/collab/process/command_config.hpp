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

    // Behavior
    std::chrono::milliseconds timeout{0};  // 0 = no timeout
    bool detached = false;                 // child survives parent

    // Dotenv — load .env files into the child's environment
    bool dotenv = false;                   // false = no .env loading

    // Interruptible — opt in to a spawn path that supports interrupt() (Ctrl+C).
    // Windows: spawns the child under a ConPTY so a real CTRL_C_EVENT can be
    //   delivered by writing 0x03 to the console's input. Because ConPTY is a
    //   terminal, stdout and stderr are combined into a single stream — all
    //   captured output lands in Result::stdout_content, stderr_content is empty.
    // Unix: no-op. interrupt() works via killpg regardless.
    bool interruptible = false;
};

struct IoCallbacks {
    collab::process::move_only_function<void(std::string_view)> on_stdout;
    collab::process::move_only_function<void(std::string_view)> on_stderr;
};

}  // namespace collab::process
