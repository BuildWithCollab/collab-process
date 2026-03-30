#pragma once

#include "collab/process/result.hpp"
#include "collab/process/running_process.hpp"

#include <chrono>
#include <expected>
#include <string>

namespace collab::process {

// Platform-specific implementation details for RunningProcess.
// Defined differently per platform (win32/running_process_impl.cpp, unix/running_process_impl.cpp).
struct RunningProcess::Impl {
    virtual ~Impl() = default;

    virtual auto pid() const -> int = 0;
    virtual auto is_alive() const -> bool = 0;
    virtual auto wait() -> std::expected<Result, SpawnError> = 0;
    virtual auto wait_for(std::chrono::milliseconds timeout) -> std::expected<Result, SpawnError> = 0;
    virtual auto stop(std::chrono::milliseconds grace) -> StopResult = 0;
    virtual auto kill() -> bool = 0;
};

}  // namespace collab::process
