# Proposal: signal routing inferred from streams

Remove the `ProcessGroup` and `Session` public-API concepts introduced by
`stop-redesign.md`. Behavior is inferred from the stream modes the developer
already chose. One optional override exists for the rare case where the
inference is wrong.

## What's removed from the public API

- `CommandConfig::ProcessGroup` enum
- `CommandConfig::process_group` field
- `CommandConfig::Session` enum
- `CommandConfig::session` field
- `Command::process_group()`, `.own_process_group()`, `.inherit_process_group()`
- `Command::session()`, `.new_session()`, `.inherit_session()`

The library ceases to expose "process group" or "session" as user-facing
concepts. The words do not appear in any public header or in the README.

## Behavior contract (the star)

Two cases, determined by stream modes.

**Any stream is redirected** — `stdout_mode != inherit`, `stderr_mode != inherit`,
or `stdin_mode != inherit`:

- `proc->terminate()` and `proc->interrupt()` deliver from code.
- `proc->kill()` cascades to descendants on both platforms.
- The user's Ctrl+C in the terminal will not reach the child.

**All streams inherit** (default `.spawn()`):

- Child stays wired to the user's terminal.
- The user's Ctrl+C reaches the child naturally.
- `proc->terminate()` and `proc->interrupt()` return `false`.
- `proc->kill()` kills the direct child only on Unix (Windows Job Object still
  tree-kills).

The developer picks signal behavior by picking stream modes — the same way
they already configure everything else.

## What the implementation does under the hood

The library holds one predicate:

```cpp
bool is_signalable(const CommandConfig& c) {
    if (c.signalable.has_value()) return *c.signalable;
    return c.stdout_mode != OutputMode::inherit
        || c.stderr_mode != OutputMode::inherit
        || c.stdin_mode  != StdinMode::inherit;
}
```

When `is_signalable(config)` is **true**:

- **Windows:** `CREATE_NEW_PROCESS_GROUP` is OR'd into the `CreateProcessW`
  flags. This is what gives `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)` a
  valid group ID to target, and what excludes the child from `CTRL_C_EVENT`
  broadcast on that console.
- **Unix:** the forked child calls `setpgid(0, 0)` before `execve`. The child
  becomes a process-group leader, `kill(-pid, SIG*)` reaches the whole group,
  and terminal SIGINT (Ctrl+C) does not reach the child since it's no longer
  in the terminal's foreground group.

When `is_signalable(config)` is **false**:

- **Windows:** `CREATE_NEW_PROCESS_GROUP` is not set. Child is in the parent's
  process group and receives `CTRL_C_EVENT` with the parent.
- **Unix:** `setpgid(0, 0)` is not called. Child remains in the parent's
  process group and receives terminal SIGINT.

The Windows Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` is
unchanged — applied unconditionally, as before. It handles tree-kill for
`proc->kill()` and orphan prevention when the parent dies. Signal routing
doesn't affect it.

## Signal-method behavior table

| Condition | `terminate()` | `interrupt()` | `kill()` |
|---|---|---|---|
| `is_signalable` true | Win: CTRL_BREAK delivered. Unix: SIGTERM to group | Win: always `false`. Unix: SIGINT to group | Win: Job Object tree kill. Unix: SIGKILL to group (tree kill) |
| `is_signalable` false | Returns `false` | Returns `false` | Win: Job Object tree kill (unchanged). Unix: SIGKILL to the direct child only |

## The override

```cpp
struct CommandConfig {
    // ...
    std::optional<bool> signalable;  // nullopt = infer from stream modes
};

class Command {
    // ...
    auto signalable(bool value = true) -> decltype(auto);
};
```

- `.signalable(true)` with all-inherit streams: child still gets process-group
  setup. `terminate()` works. User's Ctrl+C won't reach it.
- `.signalable(false)` with redirected streams: child does not get
  process-group setup. User's Ctrl+C reaches it. `terminate()` / `interrupt()`
  return `false`. On Unix, `kill()` hits only the direct child.

Default is `std::nullopt`. Stream modes drive the behavior.

## README changes

- Remove the `StopResult`, `ProcessGroup`, `Session` sections entirely.
- `CommandConfig` section: remove the two enums and two fields.
- Builder table: remove the process-group and session rows; no replacement
  rows except a one-line entry for `.signalable()`.
- Defaults table: remove the `process_group` and `session` rows.
- Add a short "Signal reachability" subsection documenting the two-case
  behavior contract above.

## Tests

- Delete every test tagged `[process_group]` or `[session]` and anything that
  references `CommandConfig::ProcessGroup`, `CommandConfig::Session`, or the
  removed builder methods.
- Rewrite `tests/test_signals.cpp`: cases express signal behavior through
  stream-mode choices, not via removed enums. For example, "child with
  `stdout_discard` + `signal_trap` receives SIGTERM via `terminate()`"
  replaces "own process_group + signal_trap".
- Delete `tests/test_session.cpp` entirely.
- Add a handful of cases for the explicit override:
  - `.signalable(true)` on an all-inherit spawn: `terminate()` works.
  - `.signalable(false)` on a `stdout_capture` spawn: `terminate()` returns
    `false`.
- Keep the `signal_trap`, `signal_ignore`, `spawn_child` modes in
  `test_helper.cpp` unchanged — they still model the same scenarios, the
  test harness around them just asks different questions.

## TDD order

1. Delete public-API surface (headers, builder methods, enums).
2. Update tests: delete dead ones, rewrite signal tests in the stream-mode
   vocabulary. These will not compile yet — that's the red.
3. Add `signalable` field + builder method + `is_signalable()` predicate.
4. Wire `is_signalable()` into `platform_spawn()` on both platforms.
5. Update README.
6. Build and run full suite — green.

## Cleanup

- `docs/proposals/stop-redesign.md` is deleted by this change — superseded.
