#include "collab/process/result.hpp"

#include <format>

namespace collab::process {

auto SpawnError::what() const -> std::string {
    switch (kind) {
        case command_not_found:
            return std::format("command not found (native error: {})", native_error);
        case permission_denied:
            return std::format("permission denied (native error: {})", native_error);
        case pipe_creation_failed:
            return std::format("pipe creation failed (native error: {})", native_error);
        case invalid_working_directory:
            return std::format("invalid working directory (native error: {})", native_error);
        case platform_error:
            return std::format("platform error: {}", native_error);
    }
    return std::format("unknown error: {}", native_error);
}

}  // namespace collab::process
