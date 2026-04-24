# Proposal — Lifecycle Parity Across Platforms

## The bug

On abrupt parent death (`SIGKILL`, crash, OOM kill, power loss before
userspace cleanup runs), Unix children orphan to PID 1. Windows children
die automatically via the Job Object's kill-on-close flag. The lifecycle
contract this library exposes should not depend on the host OS.

Scope is narrow: the bug only affects **non-detached** Unix spawns where
the parent dies abruptly. Graceful parent exit is already handled by
`~RunningProcess()` → `impl_->kill()` on all three platforms. Detached
children are *supposed* to survive parent death — that is the entire
point of `detach()`, and any fix must preserve that contract.

## Current state

| Platform | Abrupt parent death | Mechanism |
|---|---|---|
| Windows | Child dies ✅ | Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (`win32/spawn.cpp:461`) |
| Linux | Child orphans ❌ | None |
| macOS | Child orphans ❌ | None |

Windows' `release_for_detach()` (`win32/spawn.cpp:264`) already clears the
kill-on-close flag so `detach()` works correctly there. Unix's
`release_for_detach()` (`unix/spawn.cpp:192`) only closes reader pipes —
there is no lifecycle mechanism to clear because there is no lifecycle
mechanism to begin with.

## The fix — supervisor process on both Linux and macOS

Unix platform_spawn double-forks. The intermediate process stays alive as a
supervisor; the grandchild `execve`s the target binary.

```
library code ──fork──▶  supervisor  ──fork──▶  target (execve)
      │                      │
      │◀── info pipe ────────┤   (one-shot: target PID or spawn error)
      │                      │
      │◀── lifecycle pipe ───┤   (EOF when library dies)
      │                      │
      │─── release pipe ────▶┤   (one byte when detach() is called)
```

**File descriptor hygiene.** All library-created pipes (stdio, info,
lifecycle, release, and the exec-probe below) are opened with
`O_CLOEXEC` via `pipe2`. The existing `pipe()` calls at
`src/unix/spawn.cpp:220, 226, 233` become `pipe2(..., O_CLOEXEC)`. The
grandchild's `dup2` of its stdio ends into `0`/`1`/`2` clears
`O_CLOEXEC` on the destination fds, so the target inherits
stdin/stdout/stderr and nothing else. No post-fork FD-table walk, no
`closefrom` loop — the flag does the work. This is load-bearing:
without it, the target can inherit the lifecycle pipe's write end and
the supervisor will never see EOF on library death.

**Supervisor process group.** The supervisor calls `setpgid(0, 0)`
*after* the second fork, not before. Ordering matters: the grandchild
is forked while the supervisor still shares the library's pgrp, so the
grandchild inherits the library's pgrp (preserving the "interactive
mode shares the parent's pgrp" contract). The grandchild then does its
own `setpgid(0, 0)` when `has_own_group` is set, exactly as today. Only
after the grandchild is forked does the supervisor move to its own
pgrp, isolating itself from pgrp-scoped signals aimed at the library
or target — an external tree-kill of the library's pgrp does not take
the supervisor with it.

**Detecting `execve` failure.** `execve` happens in the grandchild,
after the supervisor has already returned from its second `fork`. The
grandchild creates a CLOEXEC exec-probe pipe (`pipe2(fd, O_CLOEXEC)`)
before `execve`:

- Successful `execve` → kernel atomically closes the write end →
  supervisor reads EOF from the probe → execve succeeded.
- Failed `execve` → grandchild writes `errno` to the probe, `_exit`s →
  supervisor reads the errno → execve failed.

**Info pipe wire format.** The info pipe carries two framed messages
over the supervisor's lifetime, written by the supervisor and read by
the library.

*Message 1 — spawn result.* Written after the supervisor learns whether
the second fork and grandchild `execve` succeeded:

- tag `0` (ok) — followed by `pid_t` target PID
- tag `1` (spawn error) — followed by `int` errno (from `fork` or from
  the grandchild's exec-probe report)

*Message 2 — target exit.* Written when the target exits (poll event 3):

- tag `2` (exit) — followed by `int` flag (0 = normal exit, 1 = killed
  by signal) and `int` payload (exit code or signal number, per the
  flag)

POSIX guarantees writes ≤ `PIPE_BUF` are atomic, so each message lands
all-or-nothing. The library reads message 1 synchronously before
returning from `spawn()`; message 2 during `wait()` / `wait_for()` /
destructor. On spawn error it returns `std::unexpected(SpawnError{...})`;
callers cannot distinguish fork-failure from execve-failure, which
matches today's `run()`/`spawn()` semantics. On success it stores the
target PID in `RunningProcess`.

The supervisor then enters a `poll()` loop watching three events:

1. **Lifecycle pipe EOF** — library process died (any cause, including
   `SIGKILL`). Supervisor sends `SIGKILL`: `kill(-target_pid, SIGKILL)`
   when the target has its own pgrp (tree-kills descendants), otherwise
   `kill(target_pid, SIGKILL)` (single-target). Supervisor `waitpid`s
   the target, `_exit(0)`. No orphan.
2. **Release pipe readable** — library called `release_for_detach()`.
   Supervisor stops watching, `_exit(0)` without touching the target.
   Target reparents to PID 1 and continues running. This is the `detach()`
   semantic working correctly.
3. **Target exits on its own** — supervisor `waitpid`s the target,
   writes a result message to the info pipe containing the full target
   status (`WIFEXITED` vs `WIFSIGNALED`, plus exit code or signal
   number), closes the pipe, and `_exit(0)`. The library's `waitpid` on
   the supervisor then resolves unambiguously:
   - `WIFEXITED(0)` — supervisor exited clean. Read target status from
     the info pipe and build `Result` with full fidelity.
   - `WIFSIGNALED` or `WIFEXITED(nonzero)` — supervisor died abnormally
     or violated protocol. Return `SpawnError{Kind::platform_error}`;
     target's fate is unknown to the library.

If `poll()` reports release-readable and lifecycle-EOF in the same
epoch (library wrote to the release pipe then died before the supervisor
scheduled), release wins — user intent was detach, not tear-down.

The supervisor never `execve`s. It stays our code, so we can talk to it.

### Why supervisor and not `prctl(PR_SET_PDEATHSIG)` on Linux

`prctl(PR_SET_PDEATHSIG, SIGKILL)` is set inside the child, survives
`execve`, and is kernel-enforced. It would handle abrupt parent death with
less machinery than a supervisor.

It cannot be cleared from outside the child. Once set, only the child
calling `prctl(PR_SET_PDEATHSIG, 0)` on itself can clear it — but the
target binary is now executing its own code (`nginx`, `postgres`,
whatever the user spawned) and will never make that call. There is no
syscall for "parent, clear the child's PDEATHSIG."

That makes `prctl` fundamentally incompatible with `detach()` after
`spawn()`. The child was marked for death at fork time; the mark cannot
be undone. `detach()` would return a PID and look like it worked, but the
"detached" child would die the moment the parent exits — silently
breaking real use cases:

1. Start a daemon, health-check via its stdout, detach on success (so
   the CLI can exit and the daemon keeps running).
2. Spawn a child and hand its PID to an external tracker (database,
   supervisor, job queue), detach once the tracker has acknowledged.
3. Spawn with a captured stdout, read a banner line, then detach — the
   fire-and-forget pattern that needs to observe the child first.

All three need `detach()` to actually detach. A supervisor can be told
things (via the release pipe) after the fact. `prctl` cannot.

`prctl` is kernel-enforced and cannot be defeated by killing a userspace
process. A supervisor can be: if the OOM killer picks it, the supervisor
dies without SIGKILL'ing the target, and if the library subsequently dies
the target orphans. Accepted as a known residual limitation — the
library does not touch `oom_score_adj` (supervisor is not a critical
system service, and lowering the score below `0` without
`CAP_SYS_RESOURCE` is blocked by the kernel anyway).

Pure orphan-prevention robustness favors `prctl`. `detach()` after
`spawn()` is only supported by a supervisor. This proposal picks
supervisor because the `detach()` contract is load-bearing for the three
use cases listed above.

### Public API impact

Zero. Implementation detail.

- `pid()` returns the target's PID (what callers observe and signal).
  The supervisor tracks the target PID and the library reads it from the
  supervisor over a small info pipe at spawn time, before returning.
- `wait()` / `wait_for()` `waitpid` the supervisor (which we are the
  direct parent of). The supervisor's own wait status flags health:
  `WIFEXITED(0)` means the target's true status is on the info pipe
  (full fidelity); anything else means the supervisor died abnormally
  and the library returns `SpawnError{platform_error}`.
- `terminate()` / `interrupt()` / `kill()` signal the target PID
  directly. The supervisor is never signalled by the library; it exits on
  its own when the target does or when the release pipe fires.
- `release_for_detach()` writes one byte to the release pipe and closes
  both pipe fds, then reaps the supervisor with `waitpid(..., 0)`. The
  library returns control to the caller with the target still alive and
  reparented to PID 1.

## Windows — unchanged

Job Object with `KILL_ON_JOB_CLOSE` already gives the guarantee.
`release_for_detach()` already clears the flag so detach works. The
reference behavior the Unix supervisor is matching.

## Other fixes bundled

These came out of the same analysis. Each is small, each is about
restoring honesty in the API.

### Unix `kill()` lies about success

`unix/spawn.cpp:184-190` currently:

```cpp
auto kill() -> bool override {
    if (!is_alive()) return false;
    pid_t target = has_own_group ? -child_pid : child_pid;
    ::kill(target, SIGKILL);                // return value discarded
    waitpid(child_pid, nullptr, 0);
    return true;                             // always true if was alive
}
```

If `::kill()` fails (EPERM, ESRCH between the `is_alive()` check and the
call, etc.) the caller is told "yes, the signal landed" when it did not.
Under the bool contract below, this is a bug. Fix:

```cpp
auto kill() -> bool override {
    if (!is_alive()) return false;
    pid_t target = has_own_group ? -child_pid : child_pid;
    bool ok = ::kill(target, SIGKILL) == 0;
    waitpid(child_pid, nullptr, 0);
    return ok;
}
```

### Pin the bool contract on signal methods

`terminate()`, `interrupt()`, and `kill()` return `bool`. The contract is
documented explicitly:

> **`true` — the signal was delivered.
> `false` — the signal was not delivered** (process already gone, syscall
> failed, platform has no mapping for this signal).

That is the only meaning of the bool. No other conditions are encoded in
it. This is a documentation change plus the `kill()` fix above to make
the implementation match.

### `spawn_detached()` always places the target in its own process group

**Contract.** A detached child must not share the parent's process
group. Otherwise terminal Ctrl+C aimed at the dying parent's pgrp lands
on a child the caller explicitly asked to keep running — the opposite
of the fire-and-forget intent.

**Current bug.** `spawn_detached()` (`src/run.cpp:142`) is
`spawn() + detach()` and inherits the `signalable` inference. With
default settings (all streams inherit, no explicit `signalable`), the
inference returns false → target shares parent's pgrp → terminal Ctrl+C
hits it.

**Fix.** Override the flag at the `spawn_detached()` boundary.

```cpp
auto spawn_detached(CommandConfig config, IoCallbacks callbacks)
    -> std::expected<int, SpawnError> {
    config.signalable = true;  // detached children always get their own pgrp
    auto proc = spawn(std::move(config), std::move(callbacks));
    if (!proc) return std::unexpected(proc.error());
    return std::move(*proc).detach();
}
```

The `spawn()` + `detach()` split pattern is unchanged — the pgrp decision
is fixed at `spawn()` time and the user can opt into `.signalable(true)`
explicitly if they want isolation during the observation window before
`detach()`. In practice the three real `spawn()` + `detach()` use cases
(see above) all involve capturing the child's output, which already trips
the "any stream redirected → signalable" inference, so they land in the
correct pgrp naturally.

## Explicitly out of scope

- The `interactive` / `headless` mode redesign from `proposal.md`. That
  proposal reshapes the signal API and removes the `signalable`
  inference. Orthogonal to lifecycle parity; each platform's
  parent-death mechanism works the same regardless of process-group
  configuration. Decide separately.
- The migration story for removing the `signalable` inference. Lives with
  whichever proposal touches the signal API shape.
- Any change to `ProcessRef`. It is already honest about being a
  best-effort PID-based handle with no group/tree/graceful semantics.

## Tests

Integration tests, real processes, real signals. Pseudocode; Catch2 bodies
at implementation time. Most land in a new `tests/test_lifecycle.cpp`.

### Headline — orphan prevention

```
TEST "Linux: child dies when parent process is SIGKILL'd"
    // Spawn a test harness that spawns a sleep via the library.
    // SIGKILL the harness. Sleep must be gone within 500ms.
    int target_pid = spawn_harness_that_spawns(sleep, 60s);
    ::kill(harness_pid, SIGKILL);
    wait_until(500ms, [&] { return !pid_is_alive(target_pid); });
    CHECK_FALSE(pid_is_alive(target_pid));

TEST "macOS: child dies when parent process is SIGKILL'd"
    // Same shape. Supervisor observes lifecycle pipe EOF and kills target.

TEST "Windows: Job Object kill-on-close still kills child on abrupt parent death"
    // Regression guard. Existing behavior unchanged.
```

### Detach preserves the child

```
TEST "all platforms: spawn_detached() child survives parent death"
    // Fire-and-forget. Parent dies. Child keeps running.
    int pid = spawn_detached_via_harness(long_running_helper);
    ::kill(harness_pid, SIGKILL);
    std::this_thread::sleep_for(500ms);
    CHECK(pid_is_alive(pid));
    ::kill(pid, SIGKILL);  // cleanup

TEST "all platforms: spawn() + detach() child survives parent death"
    // The case that prctl alone cannot handle on Linux.
    int pid = spawn_then_observe_then_detach_via_harness();
    ::kill(harness_pid, SIGKILL);
    std::this_thread::sleep_for(500ms);
    CHECK(pid_is_alive(pid));
    ::kill(pid, SIGKILL);
```

### Graceful exit unchanged

```
TEST "all platforms: graceful parent exit tears down non-detached child"
    // Regression guard — RAII path still works.
    int pid;
    {
        auto proc = spawn_long_running();
        pid = proc.value().pid();
    }  // ~RunningProcess() fires
    wait_until(500ms, [&] { return !pid_is_alive(pid); });
    CHECK_FALSE(pid_is_alive(pid));
```

### Bool contract

```
TEST "Unix: kill() returns false when ::kill() fails"
    // Spawn, wait for exit naturally, then call kill() — child is gone,
    // ::kill() returns ESRCH, method must return false.
    auto proc = spawn_short_lived();
    proc->wait();
    CHECK_FALSE(proc->kill());

TEST "all platforms: terminate()/interrupt()/kill() return false on dead process"
```

### spawn_detached pgrp isolation

```
TEST "Unix: spawn_detached() child is in its own process group"
    // With default (all-inherit) config — proves the override landed.
    int pid = Command(test_helper).args({"sleep","5s"}).spawn_detached().value();
    CHECK(getpgid(pid) == pid);
    ::kill(pid, SIGKILL);

TEST "Unix: SIGINT to parent's process group does NOT reach spawn_detached() child"
    // kill(0, SIGINT) hits the caller's pgrp — which includes the test
    // runner. Isolate via a throwaway forked parent in its own pgrp so
    // only that process takes the hit.
    pid_t throwaway = fork();
    if (throwaway == 0) {
        setpgid(0, 0);                      // own pgrp
        int target = Command(test_helper).args({"sleep","5s"})
            .spawn_detached().value();
        write_pid_to_pipe(target);          // hand target pid back to the test
        ::kill(0, SIGINT);                  // hits this throwaway's pgrp only
        std::this_thread::sleep_for(200ms);
        _exit(0);
    }
    pid_t target_pid = read_pid_from_pipe();
    waitpid(throwaway, nullptr, 0);
    CHECK(pid_is_alive(target_pid));        // detached child survived
    ::kill(target_pid, SIGKILL);            // cleanup
```

### API surface unchanged

```
TEST "Unix: RunningProcess::pid() returns target PID (not supervisor)"
    // Signal sent to pid() must reach target — easiest check is that
    // kill() actually terminates the visible process.
    auto proc = Command(test_helper).args({"sleep","10s"}).spawn().value();
    pid_t reported = proc.pid();
    proc.kill();
    CHECK_FALSE(pid_is_alive(reported));

TEST "Unix: RunningProcess::wait() returns target's exit code (not supervisor's)"
    auto proc = Command(test_helper).args({"exit","42"}).spawn().value();
    auto result = proc.wait().value();
    CHECK(result.exit_code == 42);
```
