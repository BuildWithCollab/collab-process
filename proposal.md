# Proposal — Headless vs Interactive

## The two modes

A child can safely receive `SIGINT` / `SIGTERM` from exactly one place. That place is the mode.

- **interactive** — terminal drives signals. Child shares the parent's process group. Ctrl+C reaches parent and child together. Code is not allowed to send `SIGINT` / `SIGTERM`.
- **headless** — code drives signals. Child is in its own process group. Terminal Ctrl+C does not reach it. `terminate()` / `interrupt()` work via `killpg`.

You can't have both. Process-group membership is one bit.

## Public API removed

Every surface below comes out. No deprecation, no shim — the inference behavior they enabled is the bug.

- `CommandConfig::signalable` — the `std::optional<bool>` field in `include/collab/process/command_config.hpp:47`.
- `Command::signalable(bool)` — the fluent builder method in `include/collab/process/command.hpp:161`.
- `terminate()` / `interrupt()` returning `false` for "not signalable" — these now throw `ModeError` instead (see below). Callers relying on the `false` return to branch must switch to catching, or (better) check the mode they asked for.
- README: the `Signal reachability` section (lines 329-345), the `signalable` field entry in the `CommandConfig` block (line 185), the `signalable(bool)` entry in the builder-methods table (line 398), and the `signalable` row in the Defaults table (line 477) — all replaced by `Mode` documentation.
- Doc comments referencing "signalable" on `RunningProcess::terminate()` (`include/collab/process/running_process.hpp:32-33`) — rewritten against `Mode`.

## Public API added

```cpp
// include/collab/process/command_config.hpp
struct CommandConfig {
    enum class Mode { interactive, headless };
    Mode mode = Mode::interactive;   // default
    // ... rest unchanged
};
```

```cpp
// include/collab/process/command.hpp
auto& interactive(this auto&& self);   // sets Mode::interactive
auto& headless(this auto&& self);      // sets Mode::headless
```

```cpp
// new exception type, exposed in a public header
namespace collab::process {

class ModeError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

}
```

## Interactive (default)

```cpp
auto result = Command("git")
    .args({"status", "--porcelain"})
    .stdout_capture()
    .run();
```

Captures output. Child shares the parent's process group. Ctrl+C takes both. `terminate()` / `interrupt()` **throw** — calling them on an interactive handle is a contract violation, not a runtime failure to signal. `kill()` still works (signals the child's pid directly) so `~RunningProcess()` can tear down. No orphan on abrupt parent death.

```cpp
CommandConfig config;
config.program = "git";
config.args = {"status", "--porcelain"};
config.stdout_mode = CommandConfig::OutputMode::capture;
// config.mode defaults to Mode::interactive
auto result = run(config);
```

## Headless (opt-in)

```cpp
auto proc = Command("server")
    .stdout_capture()
    .headless()
    .spawn();

proc->terminate();                     // delivers SIGTERM via killpg
if (!proc->wait_for(5s).has_value())
    proc->kill();                      // escalates via killpg
```

Child is in its own process group. Code owns lifecycle. Terminal Ctrl+C no longer reaches the child.

```cpp
CommandConfig config;
config.program = "server";
config.stdout_mode = CommandConfig::OutputMode::capture;
config.mode = CommandConfig::Mode::headless;
auto proc = spawn(config);
```

## The change

`src/run.cpp` — `resolve_signalable` becomes a direct read:

```cpp
static bool resolve_signalable(const CommandConfig& config) {
    return config.mode == CommandConfig::Mode::headless;
}
```

No inference. Stream modes no longer decide process-group membership.

`src/unix/spawn.cpp` — `params.signalable` still gates `setpgid(0, 0)` and `has_own_group`. `terminate()` and `interrupt()` throw `ModeError` when `!has_own_group` (the field name stays internal; renaming it is optional cleanup):

```cpp
auto terminate() -> bool override {
    if (!has_own_group)
        throw ModeError("collab::process: terminate() requires headless mode");
    if (!is_alive()) return false;
    return ::kill(-child_pid, SIGTERM) == 0;
}

auto interrupt() -> bool override {
    if (!has_own_group)
        throw ModeError("collab::process: interrupt() requires headless mode");
    if (!is_alive()) return false;
    return ::kill(-child_pid, SIGINT) == 0;
}
```

`kill()` stays as-is — RAII teardown needs it in both modes. Same throwing change mirrored on Windows.

`src/win32/spawn.cpp` — mirror the Unix change. The internal `SpawnParams` flag (set from `config.mode == Mode::headless` by the shared layer) continues to gate `CREATE_NEW_PROCESS_GROUP` and the interactive console-reset branch. `terminate()` / `interrupt()` throw `ModeError` in the interactive case; `kill()` stays unconditional so the Job Object tears down in both modes.

`src/platform.hpp` — the internal `SpawnParams::signalable` flag can stay as-is or be renamed (e.g. `SpawnParams::headless`) as non-public cleanup. Behavior is unchanged either way.

## Tests

These are integration tests — real child processes, real signals, real process groups. Pseudocode only; Catch2 bodies slot in at implementation time. Most land in `tests/test_signals.cpp`; builder-shape tests in `tests/test_command.cpp`.

### Builder / config defaults

```
TEST "CommandConfig::mode defaults to interactive"
    CommandConfig cfg;
    CHECK(cfg.mode == Mode::interactive);

TEST "Command() builder defaults to interactive"
    auto cmd = Command("x");
    CHECK(cmd.config().mode == Mode::interactive);

TEST ".interactive() sets Mode::interactive"
TEST ".headless()    sets Mode::headless"
TEST "later .interactive() / .headless() call replaces earlier"
```

### Stream redirection does NOT change the mode

This is the regression guard for the bug this proposal fixes.

```
TEST ".stdout_capture() alone leaves mode == interactive"
    auto cmd = Command("x").stdout_capture();
    CHECK(cmd.config().mode == Mode::interactive);

TEST "every non-inherit stream mode, on its own, leaves mode == interactive"
    // stdout_capture, stdout_discard, stderr_capture, stderr_discard,
    // stderr_merge, stdin_string, stdin_file, stdin_close
    // Each in isolation: mode stays interactive.
```

### Interactive: code-driven SIGINT/SIGTERM throw

```
TEST "interactive + terminate() throws ModeError"
    auto proc = Command(test_helper).args({"sleep", "10s"})
        .stdout_capture()               // intentional — prove stream mode doesn't matter
        .spawn().value();
    CHECK_THROWS_AS(proc.terminate(), ModeError);
    CHECK(proc.is_alive());             // process was not signalled
    proc.kill();                        // cleanup

TEST "interactive + interrupt() throws ModeError"
    // same shape, interrupt()

TEST "ModeError is a std::logic_error"
    try { /* trigger */ }
    catch (const std::logic_error& e) { SUCCEED(); }

TEST "ModeError::what() names the method and required mode"
    // "terminate() requires headless mode" etc.
```

### Interactive: kill() and RAII still work

```
TEST "interactive + kill() terminates the child"
    auto proc = Command(test_helper).args({"sleep", "10s"}).spawn().value();
    CHECK(proc.kill());
    CHECK_FALSE(proc.is_alive());

TEST "interactive RAII kills on scope exit"
    int pid;
    { auto proc = Command(test_helper).args({"sleep","10s"}).spawn().value();
      pid = proc.pid(); }
    // allow reaper a beat, then:
    CHECK_FALSE(pid_is_alive(pid));
```

### Interactive: terminal signal reaches child (shared process group)

Proves the orphan leak is closed for the default path. The test sends a
group-scoped signal to the test process's own group, which on Unix is how
terminal Ctrl+C works.

```
TEST "Unix: interactive child shares parent's process group"
    auto proc = Command(test_helper).args({"sleep","10s"}).spawn().value();
    CHECK(getpgid(proc.pid()) == getpgid(0));   // same group as test process

TEST "Unix: SIGINT to the parent's group reaches an interactive child"
    auto proc = Command(test_helper).args({"trap_sigint_then_exit"}).spawn().value();
    ::kill(0, SIGINT);                          // signal the whole group
    auto result = proc.wait().value();
    CHECK(result.exit_code == expected_sigint_exit);
```

### Headless: code-driven signals deliver

```
TEST "headless + terminate() delivers SIGTERM"
    auto proc = Command(test_helper).args({"trap_sigterm_then_exit"})
        .headless().spawn().value();
    CHECK(proc.terminate());
    auto result = proc.wait().value();
    CHECK(result.exit_code == expected_sigterm_exit);

TEST "headless + interrupt() delivers SIGINT"     // Unix; Windows skip

TEST "headless + kill() delivers SIGKILL via killpg (tree kill)"
    // spawn helper that forks a grandchild; kill() on the parent
    // must take the grandchild too.
```

### Headless: child is isolated from parent's terminal group

```
TEST "Unix: headless child is in its own process group (pgid == pid)"
    auto proc = Command(test_helper).args({"sleep","10s"}).headless().spawn().value();
    CHECK(getpgid(proc.pid()) == proc.pid());

TEST "Unix: SIGINT to the parent's group does NOT reach a headless child"
    auto proc = Command(test_helper).args({"sleep","5s"}).headless().spawn().value();
    ::kill(0, SIGINT);                          // parent's group only
    // Child must still be alive — it's in its own group.
    std::this_thread::sleep_for(200ms);
    CHECK(proc.is_alive());
    proc.kill();
```

### Windows

Same shape as the Unix tests above, with platform-specific mechanisms:

```
TEST "Windows: headless child gets CREATE_NEW_PROCESS_GROUP"
    // spawned with the flag; terminate() uses GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT).

TEST "Windows: interactive + terminate() throws ModeError"
TEST "Windows: headless + terminate() delivers CTRL_BREAK_EVENT"
TEST "Windows: interactive + kill() terminates via Job Object"
TEST "Windows: Job Object kill-on-close closes the child on abrupt parent exit"
    // spawn a child-that-spawns-our-target test binary, SIGKILL the middle,
    // assert the grandchild is gone — this proves Windows is immune to the
    // orphan class regardless of mode, which is an invariant worth pinning.
```

### Removed-surface compile check

Not a runtime test — a build-time smoke that the deletions really happened.

```
STATIC_CHECK !has_member(CommandConfig, signalable)
STATIC_CHECK !has_method(Command, signalable)
```

Catch2 `STATIC_REQUIRE` with `requires` expressions does this cleanly in one
file under `tests/`.
