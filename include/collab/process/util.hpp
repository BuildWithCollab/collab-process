#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace collab::process {

// Resolve a command name to its full path via PATH.
auto find_executable(std::string_view name) -> std::optional<std::filesystem::path>;

// Check if a file is a PE executable (reads first 2 bytes for MZ magic).
// Always returns false on non-Windows platforms.
auto is_pe_executable(const std::filesystem::path& path) -> bool;

}  // namespace collab::process
