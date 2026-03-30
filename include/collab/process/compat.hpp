#pragma once

// C++23 feature polyfills for compilers with incomplete library support.

#include <functional>

namespace collab::process {

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
template <typename Sig>
using move_only_function = std::move_only_function<Sig>;
#else
// Fallback: std::function (loses the non-copyable guarantee, but compiles)
template <typename Sig>
using move_only_function = std::function<Sig>;
#endif

}  // namespace collab::process
