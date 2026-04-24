#pragma once

#include <stdexcept>

namespace collab::process {

// Thrown when a signal method is called in a mode that cannot deliver it —
// specifically terminate() or interrupt() on an interactive-mode process.
// Interactive children share the parent's process group; the terminal owns
// their signals. Calling the code-driven terminate()/interrupt() on such a
// handle is a programming error (contract violation), not a runtime signal
// failure — it is never a transient condition, so it is a logic_error.
class ModeError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

}  // namespace collab::process
