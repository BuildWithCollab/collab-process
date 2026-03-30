#include "collab/process/util.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace collab::process {

auto write_temp_file(std::string_view content, std::string_view prefix)
    -> std::expected<fs::path, std::error_code> {
    // Generate a unique suffix to avoid collisions
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<unsigned> dist(0, 0xFFFFFF);

    std::ostringstream suffix;
    suffix << std::hex << std::setfill('0') << std::setw(6) << dist(rng);

    auto filename = std::string("collab_") + std::string(prefix) + "_" + suffix.str() + ".txt";
    auto path = fs::temp_directory_path() / filename;

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return std::unexpected(
            std::make_error_code(std::errc::permission_denied));
    }

    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f) {
        return std::unexpected(
            std::make_error_code(std::errc::io_error));
    }

    f.close();
    return path;
}

}  // namespace collab::process
