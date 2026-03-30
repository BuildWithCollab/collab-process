#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace collab::process {

// Resolve a command name to its full path via PATH.
auto find_executable(std::string_view name) -> std::optional<std::filesystem::path>;

// Check if a file is a PE executable (reads first 2 bytes for MZ magic).
// Always returns false on non-Windows platforms.
auto is_pe_executable(const std::filesystem::path& path) -> bool;

// Write content to a uniquely-named temp file. No collisions.
auto write_temp_file(std::string_view content, std::string_view prefix = "proc")
    -> std::expected<std::filesystem::path, std::error_code>;

}  // namespace collab::process
