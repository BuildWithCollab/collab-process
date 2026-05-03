#include "collab/process/result.hpp"

#include <fmt/format.h>

namespace collab::process {

auto SpawnError::what() const -> std::string {
    switch (kind) {
        case command_not_found:
            return fmt::format("command not found (native error: {})", native_error);
        case permission_denied:
            return fmt::format("permission denied (native error: {})", native_error);
        case pipe_creation_failed:
            return fmt::format("pipe creation failed (native error: {})", native_error);
        case invalid_working_directory:
            return fmt::format("invalid working directory (native error: {})", native_error);
        case platform_error:
            return fmt::format("platform error: {}", native_error);
    }
    return fmt::format("unknown error: {}", native_error);
}

auto WriteError::what() const -> std::string {
    switch (kind) {
        case broken_pipe:
            return fmt::format("broken pipe — child closed stdin or has exited (native error: {})", native_error);
        case platform_error:
            return fmt::format("platform error writing to stdin: {}", native_error);
    }
    return fmt::format("unknown write error: {}", native_error);
}

}  // namespace collab::process
