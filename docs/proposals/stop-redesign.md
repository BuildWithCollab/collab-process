# Proposal: terminate / interrupt / kill + `process_group` config

## Summary

1. Add `config.process_group` + a fluent DSL on `Command`.
2. Add `config.session` + a fluent DSL on `Command`. Replaces `bool detached`.
3. Remove `RunningProcess::stop()`.
4. Add `RunningProcess::terminate()` / `interrupt()` / `kill()` as explicit one-thing primitives.
5. Zero magic defaults — no hidden timers, no combined-operation methods.

## Do not modify

The following existing API remains unchanged and must not be renamed, signature-altered, or removed as part of this work:

- `RunningProcess::detach(this RunningProcess&& self) -> int` — releases RAII ownership of the handle, returns the child's PID. Mirrors `std::thread::detach()`. Its name shares a word with the removed `bool detached` config field by coincidence; the semantics are unrelated (handle lifecycle vs spawn-time platform setup).
- `spawn_detached(CommandConfig, IoCallbacks) -> expected<int, SpawnError>` and `Command::spawn_detached()` — spawn followed by `RunningProcess::detach()`, returns the PID. Named after the method, not the removed config field.

---

## 1. `config.process_group` + fluent DSL

Add to `CommandConfig` (`include/collab/process/command_config.hpp`), in the "Behavior" group alongside `bool detached`:

```cpp
enum class ProcessGroup { inherit, own };
ProcessGroup process_group = ProcessGroup::inherit;
```

Add to `Command` builder (`include/collab/process/command.hpp`):

```cpp
auto process_group(this auto&& self, ProcessGroup group) -> decltype(auto) {
    self.config_.process_group = group;
    return std::forward<decltype(self)>(self);
}

auto own_process_group(this auto&& self) -> decltype(auto) {
    self.config_.process_group = ProcessGroup::own;
    return std::forward<decltype(self)>(self);
}

auto inherit_process_group(this auto&& self) -> decltype(auto) {
    self.config_.process_group = ProcessGroup::inherit;
    return std::forward<decltype(self)>(self);
}
```

All three follow the deducing-`this` / `forward` pattern used by existing builder methods. The enum setter covers all cases; the sugar methods cover the two common cases with names that read naturally in a chain: `Command("git").args({...}).own_process_group().run()`.

## 2. `config.session` + fluent DSL (replaces `bool detached`)

`bool detached` today conflates two concerns:
- Windows `CREATE_NEW_PROCESS_GROUP` — now handled by `process_group`.
- Unix `setsid()` — needs its own home.

Replace the field with a mode enum matching the codebase pattern.

Add to `CommandConfig`, in the "Behavior" group (replacing `bool detached`):

```cpp
enum class Session { inherit, new_session };
Session session = Session::inherit;
```

Add to `Command` builder:

```cpp
auto session(this auto&& self, Session s) -> decltype(auto) {
    self.config_.session = s;
    return std::forward<decltype(self)>(self);
}

auto new_session(this auto&& self) -> decltype(auto) {
    self.config_.session = Session::new_session;
    return std::forward<decltype(self)>(self);
}

auto inherit_session(this auto&& self) -> decltype(auto) {
    self.config_.session = Session::inherit;
    return std::forward<decltype(self)>(self);
}
```

Remove `.detached()` from the builder.

**Platform behavior:**
- Unix: `session == Session::new_session` → child calls `setsid()` (becomes its own session leader, detaches from controlling terminal).
- Windows: `session` is a no-op — no equivalent concept. Set with either value, same result. Documented as a Unix-only effect.

## 3. Remove `stop()`

- Delete `stop()` from `include/collab/process/running_process.hpp`.
- Delete `enum class StopResult` from `include/collab/process/result.hpp`.
- Delete `virtual stop()` from `src/running_process_impl.hpp`.
- Delete `stop()` impls in `src/win32/spawn.cpp` (line 224) and `src/unix/spawn.cpp` (line 155).
- Delete the forwarder in `src/running_process.cpp` (line 28).
- Delete the two `stop()` `TEST_CASE`s in `tests/test_spawn.cpp` (lines 174 and 191).

## 4. Add `terminate()` / `interrupt()` / `kill()`

`RunningProcess` gains:

```cpp
auto terminate() -> bool;   // SIGTERM / CTRL_BREAK_EVENT
auto interrupt() -> bool;   // SIGINT / (Windows: always false)
// kill() already exists; signature unchanged
```

Each returns `true` iff the underlying syscall succeeded. No waiting. No escalation.

### Platform behavior

| Method | Unix | Windows |
|---|---|---|
| `terminate()` | `kill(-pid, SIGTERM)` if `own`, `kill(pid, SIGTERM)` if `inherit` | `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)` — call fails (returns `false`) when `inherit` because no process group exists |
| `interrupt()` | `kill(-pid, SIGINT)` if `own`, `kill(pid, SIGINT)` if `inherit` | Always returns `false`. `CTRL_C_EVENT` can only broadcast to the whole console (would hit the parent), and is disabled for processes in a new process group per MSDN. |
| `kill()` | `kill(-pid, SIGKILL)` if `own`, `kill(pid, SIGKILL)` if `inherit` | `TerminateJobObject` (tree-kill; job object is always created, independent of `process_group`) |

## 5. Zero magic defaults

Nothing has a default grace period. Nothing combines actions. Composition is the caller's job:

```cpp
proc->terminate();
if (!proc->wait_for(5s).has_value())
    proc->kill();
```

---

## Platform implementation

### `src/platform.hpp`

Replace:
```cpp
bool detached = false;
```
with:
```cpp
ProcessGroup process_group = ProcessGroup::inherit;
Session session = Session::inherit;
```

### `src/run.cpp`

Both `run()` (line 65) and `spawn()` (line 117): populate the new fields on `SpawnParams`.

### `src/win32/spawn.cpp`

- Lines 427-428: gate `CREATE_NEW_PROCESS_GROUP` on `params.process_group == ProcessGroup::own` instead of `params.detached`.
- Lines 321-325: in the interactive-mode heuristic, replace `!params.detached` with `params.process_group == ProcessGroup::inherit`. Rationale: the `!detached` clause existed to skip interactive console setup when the child had its own process group (which disrupts signal routing); that concern now lives on `process_group`.
- Replace `stop()` on `Win32ProcessImpl` with:

```cpp
auto terminate() -> bool override {
    if (!is_alive()) return false;
    return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                    static_cast<DWORD>(process_id)) != 0;
}

auto interrupt() -> bool override {
    return false;
}
```

`kill()` on `Win32ProcessImpl` is unchanged.

### `src/unix/spawn.cpp`

- Lines 263-265: gate `setpgid(0, 0)` on `params.process_group == ProcessGroup::own`. Gate `setsid()` on `params.session == Session::new_session`.
- Add `bool has_own_group` field on `UnixProcessImpl`. Set it in `platform_spawn()` immediately after constructing the impl and before `fork()`, from `params.process_group == ProcessGroup::own`.
- Replace `stop()` on `UnixProcessImpl` and modify `kill()`:

```cpp
auto terminate() -> bool override {
    if (!is_alive()) return false;
    pid_t target = (has_own_group ? -child_pid : child_pid);
    return ::kill(target, SIGTERM) == 0;
}

auto interrupt() -> bool override {
    if (!is_alive()) return false;
    pid_t target = (has_own_group ? -child_pid : child_pid);
    return ::kill(target, SIGINT) == 0;
}

auto kill() -> bool override {
    if (!is_alive()) return false;
    pid_t target = (has_own_group ? -child_pid : child_pid);
    ::kill(target, SIGKILL);
    waitpid(child_pid, nullptr, 0);
    return true;
}
```

`kill()` today unconditionally signals `-child_pid`; that fails when the child is not a process-group leader, so it must consult `has_own_group`.

### `src/running_process.cpp` / `src/running_process_impl.hpp`

Replace `virtual stop()` with `virtual terminate()` + `virtual interrupt()` on the `Impl` interface. Remove the `stop()` forwarder and add `terminate()` / `interrupt()` forwarders on `RunningProcess`.

---

## Test plan

### `test_helper` additions (`tests/test_helper.cpp`)

| Mode | Behavior |
|---|---|
| `signal_trap` | Install handlers for SIGTERM/SIGINT (Unix) and CTRL_BREAK/CTRL_C (Windows). **Exit 42 on SIGTERM/CTRL_BREAK, exit 43 on SIGINT/CTRL_C.** Otherwise sleep 30s. |
| `signal_ignore` | Block/ignore SIGTERM/SIGINT/CTRL_BREAK/CTRL_C. Sleep 30s. |
| `spawn_child <seconds>` | Spawn a `sleep <seconds>` child. Print `child-PID\n` to stdout, then `grandchild-PID\n`. Wait on the grandchild. |

Exit-code split lets tests verify the right signal was sent, not just "a signal arrived."

### Test file additions / renames

- `tests/test_signals.cpp` — new file, tag `[signals]`.
- `tests/test_session.cpp` — new file, tag `[session]`.
- `tests/test_command.cpp` — append builder tests.
- `tests/xmake.lua` — register both new files.

### `tests/test_signals.cpp` — 14 cases

**terminate():**

1. `process_group=own` + `signal_trap` → child exits with code 42 (SIGTERM/CTRL_BREAK delivered and handled).
2. `process_group=own` + `signal_ignore` → `terminate()` returns `true`; after 500ms `is_alive()` still returns `true`.
3. `process_group=inherit` on Windows → returns `false`.
4. `process_group=inherit` on Unix + `signal_trap` → child exits with code 42.
5. `terminate()` after `wait()` has reaped the child → returns `false`.

**interrupt():**

6. Windows, any config → returns `false`.
7. Unix, `process_group=own` + `signal_trap` → child exits with code 43 (SIGINT delivered).
8. Unix, `process_group=inherit` + `signal_trap` → child exits with code 43.

**kill():**

9. `kill()` then `wait_for(500ms)` → `wait_for` returns a `Result`, `is_alive()` returns `false`.
10. `process_group=own` + `spawn_child 30` → read grandchild PID from stdout; after `kill()` and 500ms, `ProcessRef(grandchild_pid).is_alive()` returns `false`.
11. Unix, `process_group=inherit` + `spawn_child 30` → after `kill()` and 500ms, grandchild is still alive (documents the tradeoff).
12. Windows, `process_group=inherit` + `spawn_child 30` → grandchild is dead after `kill()` (job-object tree kill works regardless of process group).

**Composition (what `stop()` used to do):**

13. `signal_trap` child + `terminate()` + `wait_for(2s)` → returns a `Result` with `exit_code == 42` within the window. No `kill()` needed.
14. `signal_ignore` child + `terminate()` + `wait_for(500ms)` returns `nullopt` + `kill()` returns `true` + `wait_for(500ms)` returns a `Result` → child is reaped.

### `tests/test_command.cpp` — append builder tests

15. `CommandConfig` default: `process_group == ProcessGroup::inherit`.
16. `Command("x").own_process_group()` sets the field to `own`.
17. `Command("x").inherit_process_group()` sets the field to `inherit`.
18. `Command("x").process_group(ProcessGroup::own)` sets the field to `own`.
19. `Command("x").own_process_group().inherit_process_group()` — later call replaces earlier (verifies the setter replaces, doesn't OR).
20. `CommandConfig` default: `session == Session::inherit`.
21. `Command("x").new_session()` sets the field to `new_session`.
22. `Command("x").inherit_session()` sets the field to `inherit`.
23. `Command("x").session(Session::new_session)` sets the field to `new_session`.
24. `Command("x").new_session().inherit_session()` — later call replaces earlier.

### `tests/test_session.cpp` — new file, tag `[session]`, 3 cases

25. Unix: `session = new_session` → `getsid(child_pid) == child_pid` (child is its own session leader).
26. Unix: `session = inherit` → `getsid(child_pid) == getsid(0)` (child stays in parent's session).
27. Windows: `session = new_session` spawns successfully; observable stdout of `test_helper echo hello` is identical to `session = inherit` (field is a no-op on Windows).

---

## Implementation order

Single PR.

1. Add `ProcessGroup` and `Session` enums + `CommandConfig::process_group` / `session` fields in `include/collab/process/command_config.hpp`, in the "Behavior" group replacing `bool detached`.
2. Replace `bool detached` in `SpawnParams` with `ProcessGroup process_group` + `Session session`.
3. Populate the new fields in both `run()` and `spawn()` in `src/run.cpp` (lines 65, 117).
4. Update `src/win32/spawn.cpp`: `CREATE_NEW_PROCESS_GROUP` gating at line 427, interactive heuristic at line 321.
5. Update `src/unix/spawn.cpp`: `setpgid` gating at line 263, `setsid` gating at line 264. Add `has_own_group` field on `UnixProcessImpl`, set before `fork()`.
6. Extend `tests/test_helper.cpp` with `signal_trap`, `signal_ignore`, `spawn_child` modes.
7. Replace `virtual stop()` with `virtual terminate()` + `virtual interrupt()` on `RunningProcess::Impl`.
8. Implement `terminate()` / `interrupt()` on `Win32ProcessImpl` and `UnixProcessImpl`. Modify `UnixProcessImpl::kill()` to consult `has_own_group`.
9. Update `include/collab/process/running_process.hpp`: remove `stop()`, add `terminate()` / `interrupt()`. Update forwarder in `src/running_process.cpp`.
10. Remove `enum class StopResult` from `include/collab/process/result.hpp`.
11. Add fluent methods on `Command` in `include/collab/process/command.hpp`: `.process_group()`, `.own_process_group()`, `.inherit_process_group()`, `.session()`, `.new_session()`, `.inherit_session()`. Remove `.detached()`.
12. Delete the two `stop()` `TEST_CASE`s in `tests/test_spawn.cpp` (lines 174, 191).
13. Create `tests/test_signals.cpp` with the 14 signal cases.
14. Create `tests/test_session.cpp` with the 3 session cases.
15. Append builder-method cases 15–24 to `tests/test_command.cpp`.
16. Register `test_signals.cpp` and `test_session.cpp` in `tests/xmake.lua`.
