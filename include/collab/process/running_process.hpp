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

    // Send a termination request. Returns true iff the syscall succeeded.
    // No waiting, no escalation — compose with wait_for()/kill() yourself.
    // Only works when the child was spawned signalable (any redirected
    // stream, or signalable(true) explicitly); returns false otherwise.
    auto terminate() -> bool;

    // Send an interrupt request. Returns true iff the syscall succeeded.
    // Unix-only: always returns false on Windows, where CTRL_C_EVENT can
    // only target the whole console and is disabled for processes in a
    // new process group per MSDN.
    auto interrupt() -> bool;

    // Immediate tree kill.
    auto kill() -> bool;

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
