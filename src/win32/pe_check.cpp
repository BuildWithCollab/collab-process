#include "collab/process/util.hpp"

#include <fstream>

namespace collab::process {

auto is_pe_executable(const std::filesystem::path& path) -> bool {
    std::ifstream f(path, std::ios::binary);
    char magic[2] = {};
    f.read(magic, 2);
    return f.good() && magic[0] == 'M' && magic[1] == 'Z';
}

}  // namespace collab::process
