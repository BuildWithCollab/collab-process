#pragma once

#include "collab/process/compat.hpp"
#include "collab/process/command_config.hpp"
#include "collab/process/result.hpp"
#include "collab/process/running_process.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace collab::process::detail {

// Platform-specific spawn parameters — prepared by the shared layer.
struct SpawnParams {
    std::filesystem::path resolved_program;
    std::vector<std::string> args;
    std::filesystem::path working_dir;
    std::vector<std::string> env_entries;  // "KEY=VALUE" strings

    // I/O modes
    CommandConfig::OutputMode stdout_mode = CommandConfig::OutputMode::inherit;
    CommandConfig::OutputMode stderr_mode = CommandConfig::OutputMode::inherit;
    bool stderr_merge = false;

    // Stdin
    std::string stdin_content;
    std::filesystem::path stdin_path;
    bool stdin_closed = false;

    // Behavior
    bool detached = false;
    bool needs_cmd_wrapper = false;  // Windows: resolved target is not a PE

    // Callbacks (moved in, not copied)
    collab::process::move_only_function<void(std::string_view)> on_stdout;
    collab::process::move_only_function<void(std::string_view)> on_stderr;
};

// Platform implementations — one in win32/, one in unix/
auto platform_spawn(SpawnParams params)
    -> std::expected<std::unique_ptr<RunningProcess::Impl>, SpawnError>;

}  // namespace collab::process::detail
