# collab-process 🔀

A C++23 process library. Spawn processes, capture output, manage lifecycles.

## Table of Contents

- [Quick Start](#quick-start)
- [Two Paths, One Engine](#two-paths-one-engine)
- [API Reference](#api-reference)
  - [CommandConfig](#commandconfig)
  - [IoCallbacks](#iocallbacks)
  - [Result](#result)
  - [SpawnError](#spawnerror)
  - [StopResult](#stopresult)
  - [RunningProcess](#runningprocess)
  - [ProcessRef](#processref)
  - [Command (fluent builder)](#command-fluent-builder)
  - [Free Functions](#free-functions)
  - [Utilities](#utilities)
- [Defaults](#defaults)
- [What Happens Internally](#what-happens-internally)
- [Building](#building)
- [Testing](#testing)

## Quick Start

```cpp
#include <collab/process.hpp>

using namespace collab::process;

// Capture output from a command
CommandConfig config;
config.program = "git";
config.args = {"status", "--porcelain"};
config.stdout_mode = CommandConfig::OutputMode::capture;

auto result = collab::process::run(config);
if (result && result->ok())
    use(result->stdout_content);
```

Or with the fluent builder:

```cpp
auto result = Command("git")
    .args({"status", "--porcelain"})
    .stdout_capture()
    .run();
```

## Two Paths, One Engine

| Path | For when... | Example |
|------|-------------|---------|
| **`CommandConfig`** (struct) | Everything is determined at runtime — programs from config, conditional flags, reusable templates | `config.program = provider.executable();` |
| **`Command`** (fluent builder) | You know things at write time and want a clean chain | `Command("git").args({...}).run()` |

Both call the same `collab::process::run()` / `spawn()` underneath. `Command` is sugar over `CommandConfig`.

## API Reference

### CommandConfig

Plain data struct. Copyable. Storable. Build it however you want.

```cpp
struct CommandConfig {
    std::string program;                  // command name or path
    std::vector<std::string> args;        // arguments
    std::filesystem::path working_dir;    // empty = inherit parent's

    // Environment — applied on top of parent env (never modifies parent)
    std::vector<std::pair<std::string, std::string>> env_add;
    std::vector<std::string> env_remove;
    bool env_clear = false;               // true = start empty, only env_add

    // Stdout / Stderr
    enum class OutputMode { inherit, capture, discard };
    OutputMode stdout_mode = OutputMode::inherit;
    OutputMode stderr_mode = OutputMode::inherit;
    bool stderr_merge = false;            // merge stderr into stdout stream

    // Stdin — explicit enum, no ambiguity
    enum class StdinMode { inherit, content, file, closed };
    StdinMode stdin_mode = StdinMode::inherit;
    std::string stdin_content;            // read when mode == content
    std::filesystem::path stdin_path;     // read when mode == file

    // Behavior
    std::chrono::milliseconds timeout{0}; // 0 = no timeout
    bool detached = false;                // child survives parent
};
```

**Configs are copyable** — build a template, copy it, tweak per-invocation:

```cpp
CommandConfig base;
base.program = "claude";
base.stdout_mode = CommandConfig::OutputMode::capture;
base.env_remove = {"CLAUDECODE"};

for (auto& prompt : prompts) {
    auto config = base;  // copy
    config.args = {"--print", prompt};
    auto result = run(config);
}
```

### IoCallbacks

Move-only callbacks, passed at execution time. Fire on the I/O thread as data arrives.

```cpp
struct IoCallbacks {
    std::move_only_function<void(std::string_view)> on_stdout;
    std::move_only_function<void(std::string_view)> on_stderr;
};
```

```cpp
IoCallbacks callbacks;
callbacks.on_stdout = [](std::string_view chunk) {
    process_stream(chunk);
};
auto result = run(config, std::move(callbacks));
```

Callbacks live outside `CommandConfig` so the config stays copyable.

### Result

Returned by `run()` and `RunningProcess::wait()`.

```cpp
struct Result {
    std::string stdout_content;           // empty unless stdout_mode == capture
    std::string stderr_content;           // empty unless stderr_mode == capture
    std::optional<int> exit_code;         // nullopt if process never exited
    bool timed_out = false;

    auto ok() const -> bool;             // exit_code == 0 && !timed_out
};
```

### SpawnError

Returned in `std::unexpected` when a process can't be created.

```cpp
struct SpawnError {
    enum Kind {
        command_not_found,
        permission_denied,
        pipe_creation_failed,
        invalid_working_directory,
        platform_error
    };

    Kind kind;
    int native_error = 0;   // GetLastError() on Windows, errno on Unix

    auto what() const -> std::string;
};
```

### StopResult

Returned by `RunningProcess::stop()` — tells you what actually happened.

```cpp
enum class StopResult {
    stopped_gracefully,   // responded to SIGTERM / CTRL_BREAK
    killed,               // had to escalate to SIGKILL / TerminateProcess
    not_running,          // was already dead
    failed                // couldn't stop it
};
```

### RunningProcess

Move-only RAII handle returned by `spawn()`. Owns the job object (Windows) / process group (Unix) for tree operations. **Destructor kills the process** — like `jthread`, cleanup is automatic.

```cpp
class RunningProcess {
    auto pid() const -> int;
    auto is_alive() const -> bool;

    auto wait() -> std::expected<Result, SpawnError>;

    // Poll: returns the Result if done, nullopt if still running.
    // Does NOT kill the process on timeout.
    auto wait_for(std::chrono::milliseconds timeout) -> std::optional<Result>;

    // Graceful shutdown with escalation + tree kill
    auto stop(std::chrono::milliseconds grace = 5s) -> StopResult;

    // Immediate tree kill
    auto kill() -> bool;

    // Release ownership — child survives destruction.
    // Returns PID for reconnection via ProcessRef.
    auto detach(this RunningProcess&& self) -> int;
};
```

```cpp
auto proc = spawn(config);
if (!proc) return;

// ... later ...
auto stop_result = proc->stop(5s);
if (stop_result == StopResult::killed)
    log("had to force kill");
```

```cpp
// Detach if the child should outlive this handle
int pid = std::move(*proc).detach();
save_to_db(pid);  // reconnect later with ProcessRef
```

### ProcessRef

Reconnect to a process by PID — e.g. from a database. Honest about its limitations: no process group, no tree kill, no graceful stop.

```cpp
class ProcessRef {
    explicit ProcessRef(int pid);
    auto pid() const -> int;
    auto is_alive() const -> bool;
    auto kill() -> bool;          // best-effort single-PID kill
};
```

```cpp
auto ref = ProcessRef(pid_from_db);
if (ref.is_alive())
    ref.kill();
```

### Command (fluent builder)

Sugar over `CommandConfig`. Uses C++23 deducing `this` to preserve value category through the chain. `run()` and `spawn()` are `&&`-qualified — the Command is consumed on execute.

```cpp
// Temporary chain — rvalue flows through every method
auto result = Command("claude")
    .args({"--print", prompt})
    .stdin_string(input)
    .stdout_capture()
    .stderr_discard()
    .env_remove("CLAUDECODE")
    .timeout(120s)
    .run();

// Named builder — explicit move at the end
auto cmd = Command("claude");
cmd.stdout_capture();
if (need_input) cmd.stdin_string(input);
auto result = std::move(cmd).run();
```

**Builder methods:**

| Category | Methods |
|----------|---------|
| **Args** | `arg(string)`, `args(initializer_list)`, `args(vector)` |
| **Where** | `working_directory(path)` |
| **Env** | `env(key, value)`, `env_remove(key)`, `env_clear()` |
| **Stdin** | `stdin_string(content)`, `stdin_file(path)`, `stdin_close()`, `stdin_inherit()` |
| **Stdout** | `stdout_capture()`, `stdout_inherit()`, `stdout_discard()`, `stdout_callback(fn)` |
| **Stderr** | `stderr_capture()`, `stderr_inherit()`, `stderr_discard()`, `stderr_merge()`, `stderr_callback(fn)` |
| **Behavior** | `timeout(ms)`, `detached()` |
| **Execute** | `run()` → `expected<Result>`, `spawn()` → `expected<RunningProcess>`, `spawn_detached()` → `expected<int>` |

### Free Functions

The engine. Both `CommandConfig` and `Command` go through these.

```cpp
namespace collab::process {

auto run(CommandConfig config, IoCallbacks callbacks = {})
    -> std::expected<Result, SpawnError>;

auto spawn(CommandConfig config, IoCallbacks callbacks = {})
    -> std::expected<RunningProcess, SpawnError>;

// Fire-and-forget: no ownership, no RAII kill. Returns the PID.
auto spawn_detached(CommandConfig config, IoCallbacks callbacks = {})
    -> std::expected<int, SpawnError>;

}
```

### Utilities

```cpp
// Resolve a command name to its full path via PATH
auto find_executable(std::string_view name) -> std::optional<std::filesystem::path>;

// Check if a file is a PE executable (MZ magic, first 2 bytes)
// Always false on non-Windows.
auto is_pe_executable(const std::filesystem::path& path) -> bool;

// Write to a uniquely-named temp file (no collisions)
auto write_temp_file(std::string_view content, std::string_view prefix = "proc")
    -> std::expected<std::filesystem::path, std::error_code>;
```

## Defaults

| Option | Default | Rationale |
|--------|---------|-----------|
| stdin | inherit | Child reads from terminal |
| stdout | inherit | Child prints to terminal |
| stderr | inherit | Child errors to terminal |
| env | copy parent | Apply additions/removals on top |
| timeout | none | No timeout unless set |
| detached | false | Child in caller's process group |

`CommandConfig{}` with just a `program` set behaves like running the command in your terminal. Capture is opt-in.

## What Happens Internally

1. **Resolve the program** — walk PATH, find full path
2. **On Windows: MZ check** — read 2 bytes. `MZ` → `CreateProcessW` directly. Not `MZ` → wrap with `cmd /c`
3. **Build env block** — copy parent (or start empty), apply add/remove. Child gets its own block; parent is never touched
4. **Create pipes** — only for modes that need them
5. **On Windows, interactive mode** (all streams inherit, not detached) — reset console with `ENABLE_VIRTUAL_TERMINAL_INPUT` for escape sequences (Ctrl+R, PSReadLine)
6. **Spawn** — `CreateProcessW` / `fork`+`execve`. Job object (Windows) or process group (Unix) for tree kill
7. **Concurrent I/O** — stdin writes in a background thread to prevent deadlock with stdout/stderr reading
8. **Return** — `run()` waits + returns `Result`. `spawn()` returns `RunningProcess` immediately

## Building

Requires C++23. Uses [xmake](https://xmake.io).

```bash
xmake -y
xmake test -y
```

### As a dependency

```lua
-- xmake.lua
add_requires("collab-process")

target("myapp")
    add_packages("collab-process")
```

```cpp
#include <collab/process.hpp>
```

## Testing

Tests use [Catch2](https://github.com/catchorg/Catch2) and a `test_helper` binary that provides deterministic, cross-platform process behavior (echo, stdin relay, env printing, sleeping, flooding).

```bash
xmake test -y                                          # run all tests
xmake run collab-process-tests "[run]"                 # just the run tests
xmake run collab-process-tests "[spawn]"               # just the spawn tests
xmake run collab-process-tests "[command]"             # just the fluent builder tests
xmake run collab-process-tests "[utilities]"           # just the utility tests
xmake run collab-process-tests "[callbacks]"           # just the I/O callback tests
xmake run collab-process-tests "[errors]"              # just the error handling tests
xmake run collab-process-tests "[process_ref]"         # just the ProcessRef tests
```
