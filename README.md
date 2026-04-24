# collab-process 🔀

A C++23 process library. Spawn processes, capture output, manage lifecycles.

## Table of Contents

- [Quick Start](#quick-start)
- [Installation](#installation)
  - [xmake](#xmake)
  - [CMake / vcpkg](#cmake--vcpkg)
- [Two Paths, One Engine](#two-paths-one-engine)
- [API Reference](#api-reference)
  - [CommandConfig](#commandconfig)
  - [IoCallbacks](#iocallbacks)
  - [Result](#result)
  - [SpawnError](#spawnerror)
  - [RunningProcess](#runningprocess)
  - [Signal reachability](#signal-reachability)
  - [ProcessRef](#processref)
  - [Command (fluent builder)](#command-fluent-builder)
  - [Dotenv Integration](#dotenv-integration)
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

## Installation

Packages are hosted on the [BuildWithCollab/Packages](https://github.com/BuildWithCollab/Packages) registry.

### xmake

Add the package registry to your `xmake.lua` (one-time setup):

```lua
add_repositories("BuildWithCollab https://github.com/BuildWithCollab/Packages.git")
```

Then require and use the package:

```lua
add_requires("collab-process")

target("myapp")
    add_packages("collab-process")
```

### CMake / vcpkg

Custom registries for vcpkg are a bit more involved, but still easy to set up. You need two configuration files in your project root.

**`vcpkg-configuration.json`** — tells vcpkg where to find packages. A `baseline` is a git commit hash that determines which package versions are available:

```json
{
    "default-registry": {
        "kind": "git",
        "repository": "https://github.com/microsoft/vcpkg.git",
        "baseline": "<latest-vcpkg-commit-hash>"
    },
    "registries": [
        {
            "kind": "git",
            "repository": "https://github.com/BuildWithCollab/Packages.git",
            "baseline": "<latest-packages-commit-hash>",
            "packages": ["collab-core", "collab-process", "dotenv"]
        }
    ]
}
```

> `collab-core` and `dotenv` must be listed in `packages` — they're transitive dependencies.

To get the latest baselines:

```bash
# BuildWithCollab registry
git ls-remote https://github.com/BuildWithCollab/Packages.git HEAD

# Microsoft vcpkg registry
git ls-remote https://github.com/microsoft/vcpkg.git HEAD
```

> When the registry is updated with new versions, you'll need to update the baseline to see them.

**`vcpkg.json`** — your project manifest:

```json
{
    "name": "my-project",
    "version-string": "0.0.1",
    "dependencies": ["collab-process"]
}
```

> The `name` and `version-string` fields just need to be valid — they can be anything. `name` must be all lowercase letters, numbers, and hyphens.

**`CMakeLists.txt`**:

```cmake
find_package(collab-process CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE collab::collab-process)
```

For more details, see the [Packages registry README](https://github.com/BuildWithCollab/Packages).

### Then

```cpp
#include <collab/process.hpp>
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

    // Signal reachability. nullopt = infer from stream modes (any stream
    // redirected → true, all inherit → false). Set to override the inference.
    std::optional<bool> signalable;

    bool dotenv = false;                  // load .env files into child env
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

Callbacks also fire live for `spawn()` — the child doesn't have to exit first:

```cpp
auto proc = Command("tail").args({"-f", "/var/log/app.log"})
    .stdout_capture()
    .stdout_callback([](std::string_view chunk) {
        process_stream(chunk);   // invoked as each read returns
    })
    .spawn();
// callback fires as tail emits lines; no wait() required
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

    // One-shot signal primitives — no waiting, no escalation, no combined
    // operations. Bool contract, shared across all three:
    //   true  — the signal was delivered.
    //   false — the signal was not delivered (process already gone, syscall
    //           failed, or the platform has no mapping for this signal).
    //
    //   terminate(): SIGTERM / CTRL_BREAK_EVENT
    //   interrupt(): SIGINT  / (always false on Windows)
    //   kill():      SIGKILL / TerminateJobObject (tree kill)
    auto terminate() -> bool;
    auto interrupt() -> bool;
    auto kill() -> bool;

    // Release ownership — child survives destruction.
    // Returns PID for reconnection via ProcessRef.
    auto detach(this RunningProcess&& self) -> int;
};
```

Graceful shutdown is composition, not a single call — the three primitives
give you the parts:

```cpp
auto proc = spawn(config);
if (!proc) return;

proc->terminate();                     // ask nicely
if (!proc->wait_for(5s).has_value())   // didn't exit in time
    proc->kill();                      // escalate
```

`terminate()` and `interrupt()` only deliver when the child was spawned
signalable (see [Signal reachability](#signal-reachability) below); otherwise
they return `false`. `interrupt()` is Unix-only; it always returns `false`
on Windows.

```cpp
// Detach if the child should outlive this handle
int pid = std::move(*proc).detach();
save_to_db(pid);  // reconnect later with ProcessRef
```

### Signal reachability

Whether `terminate()` / `interrupt()` can reach the child, and whether the
user's Ctrl+C does, depends on how the child was spawned. The library
infers this from the stream modes you already configured — redirecting any
stream is taken as a sign you're driving the child from code and want to
drive its signals from code too.

| Configuration | Ctrl+C from user | `terminate()` / `interrupt()` | `kill()` |
|---|---|---|---|
| Any stream redirected (inferred **signalable**) | does not reach child | deliver from code | cascades to descendants |
| All streams inherit (inferred **not signalable**) | reaches child naturally | return `false` | direct child only on Unix; Windows still tree-kills via Job Object |

Override the inference with `.signalable(true)` or `.signalable(false)` (or
set `CommandConfig::signalable` directly) when you need the opposite —
e.g. spawning an all-inherit child that you still want to send
`terminate()` to, or capturing stdout from a child that should still
receive the user's Ctrl+C.

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
| **Behavior** | `timeout(ms)`, `signalable(bool)`, `dotenv()` |
| **Execute** | `run()` → `expected<Result>`, `spawn()` → `expected<RunningProcess>`, `spawn_detached()` → `expected<int>` |

### Dotenv Integration

Load `.env` files into the child's environment. Uses [dotenv](https://github.com/BuildWithCollab/dotenv) — supports `.env`, `.env.yaml`, `.env.json`, hierarchical discovery (walks to root), and `${VAR}` expansion.

```cpp
// Fluent
auto result = Command("myapp")
    .dotenv()
    .stdout_capture()
    .run();

// Struct
CommandConfig config;
config.program = "myapp";
config.dotenv = true;
auto result = run(config);
```

Dotenv vars are loaded from `working_dir` (or cwd if unset) and prepended to `env_add` — explicit `env_add` entries always take precedence over `.env` values.

```cpp
// .env has DATABASE_URL=from_file
// Explicit env_add wins:
auto result = Command("myapp")
    .dotenv()
    .env("DATABASE_URL", "from_config")
    .run();
// child sees DATABASE_URL=from_config
```

When `dotenv` is `false` (the default), no `.env` files are loaded and there is no overhead.

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
| signalable | inferred | Any stream redirected → true; all inherit → false |
| dotenv | false | No .env file loading |

`CommandConfig{}` with just a `program` set behaves like running the command in your terminal. Capture is opt-in.

## What Happens Internally

1. **Load .env files** (if `dotenv == true`) — walk from `working_dir` (or cwd) to root, load all `.env` / `.env.yaml` / `.env.json` files, merge and expand `${VAR}` references. Vars are prepended to `env_add` so explicit entries take precedence. Uses [dotenv](https://github.com/BuildWithCollab/dotenv).
2. **Resolve the program** — walk PATH, find full path
3. **On Windows: MZ check** — read 2 bytes. `MZ` → `CreateProcessW` directly. Not `MZ` → wrap with `cmd /c`
4. **Build env block** — copy parent (or start empty), apply add/remove. Child gets its own block; parent is never touched
5. **Create pipes** — only for modes that need them
6. **On Windows, interactive mode** (all streams inherit, child not signalable) — reset console with `ENABLE_VIRTUAL_TERMINAL_INPUT` for escape sequences (Ctrl+R, PSReadLine)
7. **Spawn** — `CreateProcessW` / `fork`+`execve`. Job object (Windows) or process group (Unix) for tree kill
8. **Concurrent I/O** — stdin writes in a background thread to prevent deadlock with stdout/stderr reading
9. **Return** — `run()` waits + returns `Result`. `spawn()` returns `RunningProcess` immediately

## Building

Requires C++23. Uses [xmake](https://xmake.io).

```bash
xmake -y
xmake test -y
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
xmake run collab-process-tests "[dotenv]"              # just the dotenv integration tests
```
