# 🔧 Design Changes — Applied ✅

All changes below have been implemented and tested (82 tests, 213 assertions).

---

## 1. `wait_for()` returns `std::optional<Result>` ✅

`nullopt` means "still running." `Result` means "done." Clean poll semantics — the caller holds a `RunningProcess` and can handle errors through `stop()` or `kill()`.

## 2. Stdin uses enum + single payload ✅

`StdinMode` enum (`inherit`, `content`, `file`, `closed`) mirrors the existing `OutputMode` pattern. The struct stays flat and copyable.

## 3. `exit_code` is `std::optional<int>` ✅

No sentinel values. `nullopt` means "no exit code yet." The friction of `.value()` forces callers to think about it.

## 4. Destructor kills + `detach()` opt-in ✅

RAII cleanup. `detach()` is `&&`-qualified — you must `std::move` to release ownership. Combined with `CommandConfig::detached` for fire-and-forget spawns.

---

## Bugs Fixed

- **`stdin_path`**: pipe was created but file was never read — child blocked forever on empty pipe
- **`wait_for()` timeout**: `read_pipes()` was called before the timed wait, blocking forever on long-running processes
- **`stdin_closed` on Windows**: `nullptr` handle didn't actually close stdin — fixed with NUL read handle
- **Detached stdin thread**: use-after-free when multiple tests ran — stdin writer thread now stored in impl and joined properly
- **Flood/deadlock**: `wait_for_and_kill` (used by `run()` with timeout) now reads pipes concurrently with the process wait
