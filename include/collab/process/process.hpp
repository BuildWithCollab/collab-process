#pragma once

// Umbrella include for collab::process

#include "collab/process/command.hpp"
#include "collab/process/command_config.hpp"
#include "collab/process/process_ref.hpp"
#include "collab/process/result.hpp"
#include "collab/process/running_process.hpp"
#include "collab/process/util.hpp"

#include <expected>

namespace collab::process {

// The engine — both Command and direct callers go through these.

auto run(CommandConfig config, IoCallbacks callbacks = {})
    -> std::expected<Result, SpawnError>;

auto spawn(CommandConfig config, IoCallbacks callbacks = {})
    -> std::expected<RunningProcess, SpawnError>;

// Fire-and-forget: spawn with no ownership. Returns the PID.
// The child is fully detached — no RAII kill, no destructor cleanup.
auto spawn_detached(CommandConfig config, IoCallbacks callbacks = {})
    -> std::expected<int, SpawnError>;

}  // namespace collab::process
