#include "collab/process/result.hpp"

#include <string>

namespace collab::process {

auto SpawnError::what() const -> std::string {
    auto code = std::to_string(native_error);
    switch (kind) {
        case command_not_found:
            return "command not found (native error: " + code + ")";
        case permission_denied:
            return "permission denied (native error: " + code + ")";
        case pipe_creation_failed:
            return "pipe creation failed (native error: " + code + ")";
        case invalid_working_directory:
            return "invalid working directory (native error: " + code + ")";
        case platform_error:
            return "platform error: " + code;
    }
    return "unknown error: " + code;
}

}  // namespace collab::process
