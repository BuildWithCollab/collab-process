#pragma once

#include <chrono>
#include <expected>
#include <memory>

#include "collab/process/result.hpp"

namespace collab::process {

class RunningProcess {
public:
    RunningProcess(RunningProcess&&) noexcept;
    auto operator=(RunningProcess&&) noexcept -> RunningProcess&;
    ~RunningProcess();

    RunningProcess(const RunningProcess&) = delete;
    auto operator=(const RunningProcess&) -> RunningProcess& = delete;

    auto pid() const -> int;
    auto is_alive() const -> bool;

    auto wait() -> std::expected<Result, SpawnError>;
    auto wait_for(std::chrono::milliseconds timeout) -> std::expected<Result, SpawnError>;

    // Graceful: SIGTERM → grace → SIGKILL (Unix)
    //           CTRL_BREAK → grace → TerminateProcess (Windows)
    // Kills entire process tree via job object / process group.
    auto stop(std::chrono::milliseconds grace = std::chrono::seconds{5}) -> StopResult;

    // Immediate tree kill.
    auto kill() -> bool;

    // Internal — used by spawn() to construct
    struct Impl;
    explicit RunningProcess(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace collab::process
