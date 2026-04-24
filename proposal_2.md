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
      │◀── lifecycle pipe ───┤
      │                      │
      │◀── release pipe  ────┤
```

The supervisor runs a `poll()` loop watching three events:

1. **Lifecycle pipe EOF** — library process died (any cause, including
   `SIGKILL`). Supervisor sends `SIGKILL` to the target, `waitpid`s it,
   `_exit`s. No orphan.
2. **Release pipe readable** — library called `release_for_detach()`.
   Supervisor stops watching, `_exit(0)` without touching the target.
   Target reparents to PID 1 and continues running. This is the `detach()`
   semantic working correctly.
3. **Target exits on its own** — supervisor `waitpid`s it, then `_exit`s
   with the target's exit status encoded (so the library can recover it
   by `waitpid`ing the supervisor).

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

The tradeoff is real — `prctl` is kernel-enforced and cannot be defeated
by OOM-killing our supervisor, while a supervisor is userspace and has
its own failure modes. Robustness of the orphan-prevention primitive
slightly favors `prctl`. API-contract uniformity across platforms
strongly favors supervisor. Uniformity wins: "`detach()` silently doesn't
work on Linux" is a worse lie than "supervisor could theoretically get
OOM-killed on a system already in crisis."

### Public API impact

Zero. Implementation detail.

- `pid()` returns the target's PID (what callers observe and signal).
  The supervisor tracks the target PID and the library reads it from the
  supervisor over a small info pipe at spawn time, before returning.
- `wait()` / `wait_for()` `waitpid` the supervisor (which we are the
  direct parent of). Supervisor's `_exit` status encodes the target's
  status, which the library unpacks.
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

### `spawn_detached()` forces own process group

`spawn_detached()` (`src/run.cpp:142`) is currently `spawn() + detach()`
and inherits the user's `signalable` inference/override. With default
settings (all streams inherit, no explicit `signalable`), the target ends
up in the parent's process group — so terminal Ctrl+C reaches the
"detached" child while the parent is still alive. That contradicts the
explicit fire-and-forget intent of `spawn_detached()`.

Fix: override the flag at the `spawn_detached()` boundary.

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
    int pid = Command(test_helper).args({"sleep","5s"}).spawn_detached().value();
    ::kill(0, SIGINT);                     // parent's pgrp only
    std::this_thread::sleep_for(200ms);
    CHECK(pid_is_alive(pid));
    ::kill(pid, SIGKILL);
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
