#include "collab/process/util.hpp"

namespace collab::process {

auto is_pe_executable(const std::filesystem::path&) -> bool {
    // PE is a Windows concept. On Unix, always false.
    return false;
}

}  // namespace collab::process
