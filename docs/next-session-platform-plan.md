# Next Session Plan

This document is a handoff for the next session.
The current tree already fixes the following review items:

- item 1: failed launches now count as attempts, and `AUTOSSH_MAXSTART` sees launch failures
- item 2: first-start gate-time behavior is now treated as a fatal startup failure
- item 5: monitor port parsing now validates before narrowing, so overflowed values do not wrap
- item 6: `AUTOSSH_*` parsing now uses explicit validation and reports readable errors
- shared requested-action path for supervisor control (`none` / `stop` / `restart`)
- POSIX signal integration:
  - `SIGUSR1` requests restart
  - `SIGINT` / `SIGTERM` request stop
- POSIX real `-f` mode:
  - parent returns after a successful detach handshake
  - child calls `setsid()`
  - stdin/stdout/stderr are redirected to `/dev/null`
  - detached supervisor continues the normal main loop
- Windows real `-f` background mode:
  - parent relaunches the same executable with hidden `--autosshpp-detached`
  - detached child is created with `DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP`
  - the hidden marker is stripped before normal CLI parsing, help output, and forwarded SSH args
  - detached supervisor continues the normal main loop and writes the pid file
- shared control CLI:
  - `autossh --control restart --pid <pid>`
  - `autossh --control stop --pid <pid>`
- Windows native restart/stop control:
  - supervisor creates per-pid named events
  - control CLI opens and signals those named events
  - event listeners feed the same shared requested-action path used by POSIX signals
  - restart requests replace the current child
  - stop requests shut the supervisor down and remove the pid file

The current committed tree still keeps `CMAKE_CXX_STANDARD` at `26` in
`CMakeLists.txt`. The implementation style in the recent changes uses modern
C++ facilities such as `std::expected`, `std::string_view`, `std::from_chars`,
chrono-based normalization logic, and the existing shared supervisor/control
boundary.

## Remaining Gaps

The main compatibility gap that still needs work is:

1. Child termination semantics.
   The current `Boost.Process v2` `terminate()` path is still more aggressive
   than original autossh, especially on POSIX. The current logic is in
   `AutoSSH::kill_ssh()`. Now that detach and external restart/stop control are
   in place on both POSIX and Windows, the next round should revisit graceful
   shutdown and escalation policy.

Secondary non-functional follow-up:

2. Fresh Windows review-build validation.
   Manual verification and incremental builds passed on Windows, but a clean
   `cmake --fresh ... -B build-review` run still hit a local
   command-line/FetchContent issue in this environment. That is separate from
   the autossh behavior fixes, but it is still worth cleaning up.

## Cross-Platform Direction

The right target is behavioral equivalence, not a byte-for-byte clone of POSIX
mechanics.

Shared supervisor behavior should stay platform-neutral:

- restart policy
- gate-time handling
- backoff
- max-start counting
- lifetime accounting
- monitor probe logic
- pid-file behavior

Platform-specific code should only handle:

- background detach
- external restart/stop requests
- signal/control-channel integration
- child process creation details that are OS-specific

## Current Architecture

The tree now has the intended small platform boundary instead of scattering
platform logic through the supervisor loop.

Current model:

- shared supervisor consumes events:
  - `stop_requested`
  - `restart_requested`
- platform backend produces those events
- shared supervisor decides what to do next

Current split:

- `src/main.cpp`
  - parses config
  - dispatches `--control ... --pid ...` before normal supervisor startup
  - enters normal detach + supervisor flow otherwise
- `src/config.hpp`
  - owns CLI parsing
  - strips the hidden detached relaunch marker before normal argument parsing
  - parses control-mode arguments
- `src/autossh.cpp`
  - owns restart policy, backoff, monitoring, and the main loop
  - consumes the shared requested-action state
  - on Windows, waits on named control events and translates them into requested actions
- `src/platform_control.hpp`
  - declares requested-action types
  - declares platform signal/control helpers
  - declares background-detach entry points
- `src/platform_control.cpp`
  - implements POSIX signal mapping and POSIX detach
  - implements Windows detached relaunch
  - implements control-request sending
  - implements Windows named-event helpers

Avoid making the main loop know whether requests came from:

- POSIX signals
- Windows named events
- a named pipe
- a localhost control socket

It should only know that a restart or stop was requested.

## Background Mode

### POSIX

Implemented in the current tree using:

- fork
- parent/child status pipe so the parent only exits on successful detach
- `setsid()`
- redirect stdin/stdout/stderr to `/dev/null`
- continue with the normal supervisor loop in the detached child

You may optionally do the classic second fork, but a single-fork plus `setsid()`
is probably enough for this project unless stricter daemon semantics become
necessary later.

### Windows

Implemented in the current tree as detached relaunch:

- parse `-f`
- relaunch the same executable with hidden `--autosshpp-detached`
- create the new process with detached/background-friendly flags
- parent exits successfully
- child continues in normal supervisor mode

The hidden detached marker does not appear in user-facing help output.

## Restart Control

### POSIX

Implemented in the current tree:

- `SIGUSR1` requests restart
- `SIGINT` / `SIGTERM` request stop
- restart requests cancel the current wait and skip backoff once
- stop requests cancel the current wait and shut the supervisor down
- `--control restart --pid <pid>` maps to the POSIX restart signal
- `--control stop --pid <pid>` maps to the POSIX stop signal

### Windows

Implemented in the current tree using named events keyed by supervisor pid:

- `--control restart --pid <pid>` signals the restart event
- `--control stop --pid <pid>` signals the stop event
- supervisor listeners translate those events into shared requested actions
- restart requests replace the current child
- stop requests remove the pid file and exit the supervisor

This keeps the supervisor loop platform-neutral while still using native
Windows primitives instead of pretending `SIGUSR1` exists.

## Suggested Implementation Order

1. Revisit `AutoSSH::kill_ssh()` and define the desired graceful shutdown policy.
2. Decide whether restart and stop should share the same escalation path or have
   slightly different behavior.
3. Add targeted tests or manual scripts for restart/stop races and child-exit
   races.
4. Clean up the Windows fresh-review-build path if reproducible in the next
   environment.

## Validation Checklist

After the next round of work, rerun these checks:

- `cmake --fresh ... -B build-review`
- `cmake --build build-review -j4`
- invalid env vars still fail fast:
  - `AUTOSSH_POLL=abc`
  - `AUTOSSH_PORT=70000`
  - `AUTOSSH_MESSAGE` longer than 64 bytes
- first-start gate-time cases:
  - child exits `0` immediately
  - child exits `1` immediately
  - child exits `255` immediately
- `AUTOSSH_MAXSTART=1` with a missing `AUTOSSH_PATH` exits quickly with failure
- repeated launch failures participate in backoff
- POSIX:
  - `-f` returns immediately while the detached supervisor keeps running
  - `SIGUSR1` triggers a restart
  - `--control restart --pid <pid>` also triggers a restart
  - `SIGTERM` or `--control stop --pid <pid>` shuts the supervisor down
- Windows:
  - `-f` returns immediately while the detached relaunched supervisor keeps running
  - the detached instance writes the pid file, not the short-lived parent
  - `--control restart --pid <pid>` actually replaces the child
  - `--control stop --pid <pid>` shuts the supervisor down and removes the pid file
  - the hidden detached marker does not appear in user-facing help output

## Notes For The Next Session

- The current tree is buildable in the existing `build/` directory on Windows.
- Windows manual verification completed in this session:
  - `-f` returned immediately and left a detached supervisor running
  - the detached instance wrote the pid file
  - `--control restart --pid <pid>` replaced the child process
  - `--control stop --pid <pid>` shut the supervisor down and removed the pid file
- The POSIX control-path work had already been manually verified on Linux:
  - `-f` returned immediately and left a detached supervisor running
  - `SIGUSR1` restarted the child process
  - `SIGTERM` shut the detached supervisor down and removed the pid file
- The next session should focus on graceful child shutdown semantics instead of
  more platform-control plumbing.
- Some working trees may still have unrelated local changes in `.gitignore` or
  `CMakeLists.txt`; those were intentionally not part of this functional work.
