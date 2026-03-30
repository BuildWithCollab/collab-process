#include "collab/process/util.hpp"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace collab::process {

auto find_executable(std::string_view name) -> std::optional<fs::path> {
    // If it's already an absolute path or contains a separator, check directly
    auto as_path = fs::path{name};
    if (as_path.has_parent_path()) {
        if (fs::exists(as_path))
            return as_path;
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;

#ifdef _WIN32
    constexpr char delim = ';';
    const std::vector<std::string> extensions = {"", ".exe", ".cmd", ".bat"};
#else
    constexpr char delim = ':';
    const std::vector<std::string> extensions = {""};
#endif

    std::string path_str = path_env;
    std::istringstream stream(path_str);
    std::string dir;

    while (std::getline(stream, dir, delim)) {
        if (dir.empty()) continue;
        for (auto& ext : extensions) {
            auto candidate = fs::path(dir) / (std::string(name) + ext);
            if (fs::exists(candidate))
                return candidate;
        }
    }

    return std::nullopt;
}

}  // namespace collab::process
