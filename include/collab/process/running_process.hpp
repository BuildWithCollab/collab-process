#pragma once

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

#include "collab/process/result.hpp"

namespace collab::process {

class RunningProcess {
public:
    RunningProcess(RunningProcess&&) noexcept;
    auto operator=(RunningProcess&&) noexcept -> RunningProcess&;

    // Destructor kills the process if still alive. Use detach() to release
    // ownership if you want the child to outlive this handle.
    ~RunningProcess();

    RunningProcess(const RunningProcess&) = delete;
    auto operator=(const RunningProcess&) -> RunningProcess& = delete;

    auto pid() const -> int;
    auto is_alive() const -> bool;

    auto wait() -> std::expected<Result, SpawnError>;
    auto wait_for(std::chrono::milliseconds timeout) -> std::optional<Result>;

    // Bool contract, shared across terminate/interrupt/kill:
    //   true  — the signal was delivered.
    //   false — the signal was not delivered (process already gone, syscall
    //           failed, or the platform has no mapping for this signal).
    // That is the only meaning of the bool.
    //
    // Mode contract, specific to terminate/interrupt:
    //   Interactive-mode children share the parent's process group; the
    //   terminal owns their signals. Calling terminate() or interrupt() on
    //   an interactive handle is a contract violation and throws ModeError
    //   — the bool is reserved for "was the signal delivered?", not "are
    //   you allowed to call this method?". kill() is unconditional so RAII
    //   teardown works in both modes.

    // Send a termination request. No waiting, no escalation — compose with
    // wait_for()/kill() yourself. Requires CommandConfig::Mode::headless
    // (or Command::headless() on the builder); throws ModeError if the
    // child was spawned interactive.
    auto terminate() -> bool;

    // Send an interrupt request. Requires headless mode (throws ModeError
    // otherwise). On Windows even in headless mode always returns false:
    // CTRL_C_EVENT can only target the whole console and is disabled for
    // processes in a new process group per MSDN.
    auto interrupt() -> bool;

    // Immediate tree kill. Works in both modes — needed for RAII teardown.
    auto kill() -> bool;

    // ── stdin streaming (StdinMode::pipe only) ───────────────────────────
    //
    // Feed bytes to the child's stdin while it runs. Both methods require
    // the spawn to have used CommandConfig::StdinMode::pipe (or the builder
    // method Command::stdin_pipe()). Calling them on any other stdin mode,
    // or after close_stdin() has been called, throws ModeError — these are
    // contract violations, not transient I/O failures.
    //
    // Thread-safe: write_stdin and close_stdin serialize on an internal
    // mutex. Concurrent writes from multiple threads land contiguously
    // (no interleaved bytes within a single call). Writing from inside an
    // on_stdout callback is fully supported and is the headline use case
    // (e.g. JSON-RPC over stdio, where the parser reacts to a response by
    // sending the next request).
    //
    // Partial-write handling: write_stdin loops internally on short writes
    // and EINTR — on success, the entire buffer was delivered to the kernel
    // pipe. Empty string_view is a valid no-op success.
    //
    // Errors are runtime I/O failures only:
    //   broken_pipe    — child closed stdin or has exited.
    //   platform_error — unexpected EIO / I/O failure.
    // POSIX SIGPIPE is masked per-thread inside write_stdin so a broken
    // pipe never crashes the calling process; the user's own SIGPIPE
    // disposition is preserved.
    auto write_stdin(std::string_view bytes) -> std::expected<void, WriteError>;

    // Send EOF to the child by closing the write end. Idempotent at the
    // type level only via ModeError: a second call (or any post-close
    // call to write_stdin) throws ModeError. detach() and the destructor
    // close stdin automatically if you haven't already.
    void close_stdin();

    // Release ownership — child survives. Returns PID for reconnection
    // via ProcessRef. Consumes the RunningProcess (&&-qualified).
    auto detach(this RunningProcess&& self) -> int;

    // Internal — used by spawn() to construct
    struct Impl;
    explicit RunningProcess(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace collab::process
