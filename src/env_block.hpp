#pragma once

#include "collab/process/command_config.hpp"

#include <string>
#include <vector>

namespace collab::process::detail {

// Build a null-terminated environment block for CreateProcessW (Windows)
// or a vector of "KEY=VALUE" strings for execve (Unix).
// Never modifies the parent process environment.
auto build_env_block(const CommandConfig& config) -> std::vector<std::string>;

}  // namespace collab::process::detail
