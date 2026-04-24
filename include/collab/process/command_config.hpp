#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
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

    // Signal reachability. nullopt = infer from stream modes: if any stream
    // is redirected the child gets isolated for code-driven signalling; if
    // all streams inherit it shares the terminal's signal path so the
    // user's Ctrl+C reaches it naturally. Set explicitly to override.
    std::optional<bool> signalable;

    // Dotenv — load .env files into the child's environment
    bool dotenv = false;                   // false = no .env loading
};

struct IoCallbacks {
    collab::process::move_only_function<void(std::string_view)> on_stdout;
    collab::process::move_only_function<void(std::string_view)> on_stderr;
};

}  // namespace collab::process
