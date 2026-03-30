#include "collab/process/process.hpp"
#include "collab/process/command.hpp"
#include "env_block.hpp"
#include "platform.hpp"
#include "running_process_impl.hpp"

namespace collab::process {

auto run(CommandConfig config, IoCallbacks callbacks)
    -> std::expected<Result, SpawnError> {
    // Resolve the program
    auto resolved = find_executable(config.program);
    if (!resolved)
        return std::unexpected(SpawnError{SpawnError::command_not_found, 0});

    // Validate working directory
    if (!config.working_dir.empty() && !std::filesystem::is_directory(config.working_dir))
        return std::unexpected(SpawnError{SpawnError::invalid_working_directory, 0});

    // Determine if we need cmd /c wrapper (Windows: non-PE target)
    bool needs_cmd_wrapper = false;
#ifdef _WIN32
    needs_cmd_wrapper = !is_pe_executable(*resolved);
#endif

    // Build env block
    auto env_entries = detail::build_env_block(config);

    // Prepare platform params
    detail::SpawnParams params{
        .resolved_program = std::move(*resolved),
        .args = std::move(config.args),
        .working_dir = std::move(config.working_dir),
        .env_entries = std::move(env_entries),
        .stdout_mode = config.stdout_mode,
        .stderr_mode = config.stderr_mode,
        .stderr_merge = config.stderr_merge,
        .stdin_content = std::move(config.stdin_content),
        .stdin_path = std::move(config.stdin_path),
        .stdin_closed = config.stdin_closed,
        .detached = config.detached,
        .needs_cmd_wrapper = needs_cmd_wrapper,
        .on_stdout = std::move(callbacks.on_stdout),
        .on_stderr = std::move(callbacks.on_stderr),
    };

    // Spawn
    auto impl = detail::platform_spawn(std::move(params));
    if (!impl)
        return std::unexpected(impl.error());

    // Create the RunningProcess handle
    auto proc = RunningProcess(std::move(*impl));

    // For blocking run: wait with optional timeout
    if (config.timeout.count() > 0)
        return proc.wait_for(config.timeout);
    else
        return proc.wait();
}

auto spawn(CommandConfig config, IoCallbacks callbacks)
    -> std::expected<RunningProcess, SpawnError> {
    // Resolve the program
    auto resolved = find_executable(config.program);
    if (!resolved)
        return std::unexpected(SpawnError{SpawnError::command_not_found, 0});

    // Validate working directory
    if (!config.working_dir.empty() && !std::filesystem::is_directory(config.working_dir))
        return std::unexpected(SpawnError{SpawnError::invalid_working_directory, 0});

    bool needs_cmd_wrapper = false;
#ifdef _WIN32
    needs_cmd_wrapper = !is_pe_executable(*resolved);
#endif

    auto env_entries = detail::build_env_block(config);

    detail::SpawnParams params{
        .resolved_program = std::move(*resolved),
        .args = std::move(config.args),
        .working_dir = std::move(config.working_dir),
        .env_entries = std::move(env_entries),
        .stdout_mode = config.stdout_mode,
        .stderr_mode = config.stderr_mode,
        .stderr_merge = config.stderr_merge,
        .stdin_content = std::move(config.stdin_content),
        .stdin_path = std::move(config.stdin_path),
        .stdin_closed = config.stdin_closed,
        .detached = config.detached,
        .needs_cmd_wrapper = needs_cmd_wrapper,
        .on_stdout = std::move(callbacks.on_stdout),
        .on_stderr = std::move(callbacks.on_stderr),
    };

    auto impl = detail::platform_spawn(std::move(params));
    if (!impl)
        return std::unexpected(impl.error());

    return RunningProcess(std::move(*impl));
}

// Command fluent builder delegates
auto Command::run(this Command&& self) -> std::expected<Result, SpawnError> {
    return collab::process::run(std::move(self.config_), std::move(self.callbacks_));
}

auto Command::spawn(this Command&& self) -> std::expected<RunningProcess, SpawnError> {
    return collab::process::spawn(std::move(self.config_), std::move(self.callbacks_));
}

}  // namespace collab::process
