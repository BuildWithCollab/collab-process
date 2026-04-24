# Mac test run — collab-process supervisor

Target: verify the Unix supervisor (src/unix/spawn.cpp) on macOS. The code
already passed on Linux; Mac is the third platform.

## First-session xmake config — mandatory

```bash
xmake f -m release -c -y
```

No `--qt`, no `-p`, no `-a` on Mac — xmake auto-detects.
The `-y` is mandatory (drop it and xmake silently hangs on an approval prompt).

## Build

```bash
xmake build -a -y
```

Do **not** add `-r` (rebuild). Don't rebuild unless there's a specific reason.

## Full suite

```bash
xmake test -y
```

Expected: **119/119 tests passing** (same as Windows + Linux).

## If it's all green, you're done.

Report: "Mac: 119/119 green, X.Xs." and we're finished.

## If there are failures

Run the failing tag and capture raw output. The most likely failure surfaces:

```bash
# Supervisor lifecycle — the headline feature
xmake run collab-process-tests "[lifecycle]"

# Signal routing (pgrp / terminate / interrupt)
xmake run collab-process-tests "[signals]"

# Detach path + spawn_detached's forced signalable
xmake run collab-process-tests "[spawn_detached]"

# wait_for_and_kill path (used by run() with timeout)
xmake run collab-process-tests "[run]"
```

## Most load-bearing Unix-supervisor tests (report raw output if any fail)

- `lifecycle: Unix child dies when parent process is SIGKILL'd`
  → orphan prevention — the whole point of the supervisor.
- `lifecycle: spawn_detached child survives parent death`
  → supervisor receives release byte, exits without touching target.
- `lifecycle: spawn() + detach() child survives parent death`
  → observe-then-detach (the case `prctl` can't handle).
- `lifecycle: Unix pid() returns target PID, not supervisor`
  → `pid()` plumbing through info-pipe message 1.
- `lifecycle: Unix wait() returns target's exit code, not supervisor's`
  → info-pipe message 2 wire format (tag=2, flag, payload).

## macOS-specific things to watch for

- `pipe2` was added to macOS in 10.10 (Yosemite, 2014). Any Mac you'd develop
  on has it. If the build fails with `pipe2` undefined, surface that —
  that's platform coverage.
- macOS's `kqueue` is richer than Linux `poll`, but we use plain `poll` for
  portability. If a test hangs in the poll loop specifically on Mac, it's
  worth investigating.
- SIP and code signing shouldn't bite here (we're not loading dylibs or
  hitting protected paths), but if a spawn fails with `platform_error` and
  a weird errno, that could be it.

## If a test hangs

Hangs usually mean the supervisor didn't exit — its `poll()` loop is stuck,
or the library's `waitpid(supervisor)` is blocking forever.

Capture:
```bash
ps -ef | grep test_helper  # any leftover targets?
ps -ef | grep collab-process-tests  # test still running?
```

If there are orphaned `test_helper` / supervisor processes after a run, the
supervisor isn't cleaning up. Report raw `ps` output + the test name that
hung.

## Head of current branch

Commit `33ee8ee` on `stop-refactor`. Fire away.
