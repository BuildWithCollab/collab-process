#pragma once

#include <chrono>
#include <expected>
#include <memory>
#include <optional>

#include "collab/process/result.hpp"

namespace collab::process {

class RunningProcess {
public:
    RunningProcess(RunningProcess&&) noexcept;
    auto operator=(RunningProcess&&) noexcept -> RunningProcess&;

    // Destructor kills the process if still alive. Use detach() to release
    // ownership if you want the child to outlive this handle.
    ~RunningProcess();

    RunningProcess(const RunningProcess&) = delete;
    auto operator=(const RunningProcess&) -> RunningProcess& = delete;

    auto pid() const -> int;
    auto is_alive() const -> bool;

    auto wait() -> std::expected<Result, SpawnError>;
    auto wait_for(std::chrono::milliseconds timeout) -> std::optional<Result>;

    // Graceful: SIGTERM → grace → SIGKILL (Unix)
    //           CTRL_BREAK → grace → TerminateProcess (Windows)
    // Kills entire process tree via job object / process group.
    auto stop(std::chrono::milliseconds grace = std::chrono::seconds{5}) -> StopResult;

    // Immediate tree kill.
    auto kill() -> bool;

    // Send SIGINT (Unix) / CTRL_C_EVENT (Windows) to the child.
    // Cooperative: the child may install a handler to catch or ignore.
    // Returns false if the process is not alive or the signal could not be sent.
    auto interrupt() -> bool;

    // Release ownership — child survives. Returns PID for reconnection
    // via ProcessRef. Consumes the RunningProcess (&&-qualified).
    auto detach(this RunningProcess&& self) -> int;

    // Internal — used by spawn() to construct
    struct Impl;
    explicit RunningProcess(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace collab::process
