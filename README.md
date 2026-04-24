# collab-process 🔀

A C++23 process library. Spawn processes, capture output, manage lifecycles.

## Table of Contents

- [Highlights](#highlights)
- [Quick Start](#quick-start)
- [Installation](#installation)
  - [xmake](#xmake)
  - [CMake / vcpkg](#cmake--vcpkg)
- [Concepts](#concepts)
  - [Modes: Interactive vs Headless](#modes-interactive-vs-headless)
  - [Lifecycle Guarantees](#lifecycle-guarantees)
- [Recipes](#recipes)
  - [Capture and stream output](#capture-and-stream-output)
  - [Handle spawn errors](#handle-spawn-errors)
  - [Send input to a command](#send-input-to-a-command)
  - [Run with a timeout](#run-with-a-timeout)
  - [Set environment and working directory](#set-environment-and-working-directory)
  - [Load environment from .env files](#load-environment-from-env-files)
  - [Manage a long-running process](#manage-a-long-running-process)
  - [Gracefully stop a server](#gracefully-stop-a-server)
  - [Fire-and-forget a daemon](#fire-and-forget-a-daemon)
  - [Reusable command templates](#reusable-command-templates)
  - [Resolve a command's path](#resolve-a-commands-path)
  - [Reconnect to a process by PID](#reconnect-to-a-process-by-pid)
- [Defaults](#defaults)
- [Platform Notes](#platform-notes)
- [API Reference](#api-reference)
- [Building](#building)
- [Testing](#testing)

## Highlights

- 🪶 **C++23 ergonomics** — `std::expected`, deducing `this`, move-only callbacks
- 🎛️ **Fluent builder + plain struct** — choose by call site, same engine underneath
- 🛡️ **Cross-platform lifecycle parity** — non-detached children die with the parent on Windows, Linux, and macOS. No orphans.
- 📡 **Live streaming** — get stdout/stderr via callbacks as the child writes, no polling
- 🎭 **Explicit signal ownership** — `interactive` (terminal drives Ctrl+C) vs `headless` (your code drives `terminate()` / `kill()`)
- 🧬 **Dotenv built in** — `.env`, `.env.yaml`, `.env.json`, hierarchical discovery, `${VAR}` expansion

## Quick Start

```cpp
#include <collab/process.hpp>

using namespace collab::process;

auto result = Command("git")
    .args({"log", "--oneline", "-5"})
    .stdout_capture()
    .run();

if (result && result->ok())
    std::cout << result->stdout_content;
```

Or as a plain struct, when things get decided at runtime:

```cpp
CommandConfig config;
config.program = "git";
config.args = {"log", "--oneline", "-5"};
config.stdout_mode = CommandConfig::OutputMode::capture;

auto result = run(config);
```

Both go through the same `run()` / `spawn()` engine. Pick whichever fits the call site.

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

## Concepts

Two ideas show up across the recipes — worth a minute up front.

### Modes: Interactive vs Headless

A child process can safely receive `SIGINT` / `SIGTERM` from exactly one place — that's the **mode**. Process-group membership is one bit, set at spawn time.

| Mode | Ctrl+C from terminal | `terminate()` / `interrupt()` | `kill()` |
|---|---|---|---|
| **`interactive`** (default) | Reaches the child (shared process group) | Throws `ModeError` | Works (single-process on Unix, Job Object tree-kill on Windows) |
| **`headless`** (opt-in) | Does not reach the child (own process group) | Delivers via `killpg` / `CTRL_BREAK_EVENT` | Works (tree kill in both) |

`kill()` is unconditional in both modes so the destructor can always tear down. `terminate()` and `interrupt()` are deliberately strict — calling them on an interactive handle throws `ModeError` (a `std::logic_error`), because there's no sane meaning to "the terminal owns signals AND I'm about to send one."

`spawn_detached()` always forces `headless` regardless of the caller's config — a detached child must not share the dying parent's process group.

> Note: `interrupt()` on Windows always returns `false` even in headless mode. `CTRL_C_EVENT` cannot target a process group and is disabled for processes in a new process group per MSDN.

### Lifecycle Guarantees

A non-detached child's lifetime is bounded by its parent's lifetime — on **every platform**, including when the parent dies abruptly.

| Parent ends by... | Non-detached child | Detached child |
|---|---|---|
| Scope exit (`~RunningProcess()`) | Killed (RAII) | (already released) |
| Normal exit / return from `main` | Killed | Survives |
| Crash, `SIGKILL`, OOM kill | Killed | Survives |

This parity matters and it's enforced differently on each platform: on Windows by the Job Object's kill-on-close flag; on Linux and macOS by an internal supervisor process that watches the library's lifetime and cleans up the target if the library dies. The mechanism is invisible — `pid()`, `wait()`, and the signal methods all act on the target you spawned.

Use `detach()` when you explicitly want a child to outlive its parent (daemons, fire-and-forget jobs handed to an external tracker, observe-then-release patterns). Otherwise, trust that exiting — for any reason — cleans up.

## Recipes

Each recipe is self-contained. `using namespace collab::process;` is assumed. Every recipe shown with the fluent `Command` builder works the same with `CommandConfig` — see [Reusable command templates](#reusable-command-templates).

### Capture and stream output

Block on the child and read the captured output afterwards:

```cpp
auto result = Command("git")
    .args({"log", "--oneline", "-5"})
    .stdout_capture()
    .run();

if (result && result->ok())
    std::cout << result->stdout_content;
```

For long-running processes, attach a callback and handle output as it arrives — no polling, no waiting for exit:

```cpp
auto proc = Command("tail").args({"-f", "/var/log/app.log"})
    .stdout_capture()
    .stdout_callback([](std::string_view chunk) {
        process_chunk(chunk);      // fires on the I/O thread as data arrives
    })
    .spawn();
```

Stderr has the same surface: `stderr_capture()`, `stderr_discard()`, `stderr_callback(...)`. Or merge stderr into stdout:

```cpp
auto result = Command("npm").args({"install"})
    .stdout_capture()
    .stderr_merge()                // stderr lines appear in stdout_content
    .run();
```

Callbacks live outside `CommandConfig` (so the config stays copyable). Pass them as a separate `IoCallbacks` to the free functions:

```cpp
IoCallbacks callbacks;
callbacks.on_stdout = [](std::string_view chunk) { process_chunk(chunk); };

auto result = run(config, std::move(callbacks));
```

What you get back — `Result` is plain data:

```cpp
struct Result {
    std::string stdout_content;        // empty unless stdout was captured
    std::string stderr_content;        // empty unless stderr was captured
    std::optional<int> exit_code;      // nullopt if the child never exited
    bool timed_out = false;

    auto ok() const -> bool;           // exit_code == 0 && !timed_out
};
```

### Handle spawn errors

`run()` and `spawn()` return `std::expected<T, SpawnError>`. A non-engaged result means the spawn itself failed (program not found, bad working dir, etc.) — distinct from "the child ran and exited non-zero":

```cpp
auto result = Command("does-not-exist").run();
if (!result) {
    auto& err = result.error();
    std::cerr << err.what() << "\n";       // human-readable message
    if (err.kind == SpawnError::command_not_found)
        return install_or_bail();
    return;
}
// result.value() — child started; check ok() / exit_code / timed_out
```

`SpawnError::Kind` covers: `command_not_found`, `permission_denied`, `pipe_creation_failed`, `invalid_working_directory`, `platform_error`. The `native_error` field carries the raw `errno` (Unix) or `GetLastError()` (Windows) for diagnostics.

The only other failure path you'll hit is `ModeError`, thrown by `terminate()` / `interrupt()` if you call them on an `interactive` handle — see [Modes](#modes-interactive-vs-headless).

### Send input to a command

```cpp
auto result = Command("python").args({"-"})
    .stdin_string("print('hello from stdin')\n")
    .stdout_capture()
    .run();
```

Other stdin sources:

```cpp
.stdin_file("/path/to/input.txt")  // pipe a file
.stdin_close()                      // close stdin immediately (EOF)
.stdin_inherit()                    // child reads from parent's terminal (default)
```

### Run with a timeout

```cpp
using namespace std::chrono_literals;

auto result = Command("slow-tool").timeout(30s).run();
if (result && result->timed_out)
    std::cerr << "took too long, killed\n";
```

`run()` kills the child if it exceeds the timeout. The returned `Result` has `timed_out == true` so you can distinguish a kill-by-timeout from a normal exit.

### Set environment and working directory

```cpp
auto result = Command("myapp")
    .working_directory("/path/to/project")
    .env("DATABASE_URL", "postgres://localhost/dev")
    .env_remove("SOME_INHERITED_VAR")
    .stdout_capture()
    .run();
```

By default the child gets a copy of the parent's environment with your additions/removals applied — the parent's environment is never modified. To start with an empty environment instead:

```cpp
.env_clear()                       // child starts with no env
.env("PATH", "/usr/bin")           // add only what you need
```

### Load environment from `.env` files

```cpp
auto result = Command("myapp").dotenv().stdout_capture().run();
```

`.dotenv()` walks from `working_dir` (or cwd) up to the filesystem root, loads all `.env` / `.env.yaml` / `.env.json` files it finds, merges them, and expands `${VAR}` references. Explicit `.env(key, value)` calls always win over file values:

```cpp
// .env has DATABASE_URL=from_file
// Explicit env wins:
auto result = Command("myapp")
    .dotenv()
    .env("DATABASE_URL", "from_config")
    .run();
// child sees DATABASE_URL=from_config
```

Backed by [dotenv](https://github.com/BuildWithCollab/dotenv).

### Manage a long-running process

`spawn()` returns a `RunningProcess` handle immediately instead of blocking:

```cpp
auto proc = Command("worker").stdout_capture().spawn();
if (!proc) return;

// do other work...

if (some_condition)
    proc->kill();

auto result = proc->wait();        // blocks until exit
```

`RunningProcess` is move-only and **kills the child in its destructor** — like `jthread`, cleanup is automatic on scope exit. Use `is_alive()` to poll or `wait_for(timeout)` to wait without killing.

The fluent `Command` builder uses C++23 deducing `this` and its `run()` / `spawn()` are `&&`-qualified — the builder is consumed on execute. For multi-statement construction, `std::move()` it explicitly:

```cpp
auto cmd = Command("worker");
cmd.stdout_capture();
if (need_input) cmd.stdin_string(input);
auto proc = std::move(cmd).spawn();
```

### Gracefully stop a server

Servers want a polite signal first, escalation if they ignore it. Compose it from primitives:

```cpp
using namespace std::chrono_literals;

auto proc = Command("server").headless().spawn();
if (!proc) return;

proc->terminate();                     // SIGTERM (Unix) / CTRL_BREAK_EVENT (Windows)
if (!proc->wait_for(5s).has_value())   // didn't exit in 5s?
    proc->kill();                      // escalate (always works)
```

`terminate()` and `interrupt()` require `headless` mode — see [Modes](#modes-interactive-vs-headless). `kill()` works in both modes so the destructor can always tear down.

### Fire-and-forget a daemon

```cpp
int pid = Command("background-worker").spawn_detached().value();
// parent can exit; child keeps running
save_to_db(pid);
```

`spawn_detached()` releases ownership immediately and returns the PID. The child outlives this process.

For "spawn, observe briefly, then detach" (common when a daemon prints a readiness banner before backgrounding):

```cpp
auto proc = Command("daemon").stdout_capture().spawn();
wait_for_ready_banner(*proc);
int pid = std::move(*proc).detach();
```

`detach()` consumes the `RunningProcess` and returns the PID — the child stops being owned by this process.

### Reusable command templates

`CommandConfig` is plain data — copyable, storable, tweakable per call. This is the main reason both `Command` and `CommandConfig` exist:

```cpp
CommandConfig base;
base.program = "claude";
base.stdout_mode = CommandConfig::OutputMode::capture;
base.env_remove = {"CLAUDECODE"};

for (auto& prompt : prompts) {
    auto config = base;            // copy
    config.args = {"--print", prompt};
    auto result = run(config);
}
```

### Resolve a command's path

```cpp
if (auto path = find_executable("git"))
    std::cout << "git is at " << *path << "\n";
else
    std::cerr << "git not on PATH\n";
```

`find_executable()` walks `PATH` and returns the absolute path, or `std::nullopt` if not found. Useful for "is this tool installed?" checks before spawning.

### Reconnect to a process by PID

```cpp
auto ref = ProcessRef(pid_from_db);
if (ref.is_alive())
    ref.kill();
```

`ProcessRef` is a best-effort PID-based handle — no process group, no tree kill, no graceful stop. Use it when all you have is a PID (e.g., from a database or external supervisor).

## Defaults

| Option | Default | Rationale |
|--------|---------|-----------|
| stdin | inherit | Child reads from terminal |
| stdout | inherit | Child prints to terminal |
| stderr | inherit | Child errors to terminal |
| env | copy parent | Apply additions/removals on top |
| timeout | none | No timeout unless set |
| mode | `Mode::interactive` | Terminal drives signals |
| dotenv | false | No `.env` loading |

`CommandConfig{}` with just a `program` set behaves like running the command in your terminal. Capture is opt-in.

## Platform Notes

### Windows

- **Non-PE programs are wrapped with `cmd /c`.** `.bat`, `.cmd`, `.ps1`, and any other non-PE targets are handled transparently — you can spawn them by name the same way you'd spawn an `.exe`. The MZ magic check used to decide is exposed as `is_pe_executable` if you need it yourself.
- **Console VT input is enabled for interactive children.** When all streams inherit and the mode is `Mode::interactive`, the console is reset with `ENABLE_VIRTUAL_TERMINAL_INPUT` so terminal tools that rely on escape sequences (PSReadLine `Ctrl+R`, etc.) behave correctly.

## API Reference

| Type | Purpose |
|---|---|
| `Command` | Fluent process builder |
| `CommandConfig` | Plain-data process config (copyable, storable) |
| `IoCallbacks` | Move-only stdout/stderr stream callbacks |
| `Result` | Captured output + exit code from `run()` / `wait()` |
| `RunningProcess` | RAII handle from `spawn()` — pid, wait, kill, detach |
| `ProcessRef` | PID-based reconnect handle (best effort) |
| `SpawnError` | Spawn-time failure kind + native errno |
| `ModeError` | Thrown by `terminate()` / `interrupt()` on interactive handles |

Free functions: `run()`, `spawn()`, `spawn_detached()`, `find_executable()`, `is_pe_executable()`.

Everything lives in `<collab/process.hpp>` under the `collab::process` namespace. Read the headers in `include/collab/process/` for full method signatures and field-by-field docs.

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
