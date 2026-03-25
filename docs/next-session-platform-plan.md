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

The current buildable snapshot also keeps `CMAKE_CXX_STANDARD` at `26` in
`CMakeLists.txt`. The implementation style in the recent changes uses modern
C++ facilities such as `std::expected`, `std::string_view`, `std::from_chars`,
and chrono-based normalization logic.

## Remaining Gaps

The main compatibility gaps that still need work are:

1. Windows real `-f` background mode.
   POSIX detach is now implemented. Windows still needs a detached relaunch
   path with an internal marker such as `--autosshpp-detached`.

2. Windows restart control.
   The shared requested-action path now exists, and POSIX `SIGUSR1` is wired
   into it. Windows still needs a native transport that can request restart
   and later possibly stop.

3. Child termination semantics.
   The current `Boost.Process v2` `terminate()` path is more aggressive than
   original autossh on POSIX. This is not blocked on the other work, but it
   should be revisited once the restart/detach model is stabilized.

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

- `src/autossh.cpp`
  - owns restart policy, backoff, monitoring, and the main loop
  - consumes the shared requested-action state
- `src/platform_control.hpp`
  - declares the requested-action enum
  - maps platform signals to requested actions
  - owns background-detach entry points
- `src/platform_control.cpp`
  - implements POSIX signal mapping and POSIX detach
  - currently returns a "not implemented yet" error for Windows `-f`

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

There is no `daemon()` equivalent. The next step is still a detached relaunch
model:

- parse `-f`
- relaunch the same executable with an internal marker like
  `--autosshpp-detached`
- create the new process with detached/background-friendly flags
- parent exits successfully
- child continues in normal supervisor mode

This keeps the public contract similar to POSIX without pretending the OS model
is the same.

The internal detached marker should not leak into user-facing help.

Current status:

- Windows `-f` is not implemented yet
- the current code reports an explicit runtime error instead of silently
  pretending to daemonize

## Restart Control

### POSIX

Implemented in the current tree:

- `SIGUSR1` requests restart
- `SIGINT` / `SIGTERM` request stop
- restart requests cancel the current wait and skip backoff once
- stop requests cancel the current wait and shut the supervisor down

### Windows

Do not try to emulate `SIGUSR1` literally. Windows still needs a restart
control channel. Viable choices:

- named pipe
- localhost control socket
- named event plus a small helper CLI

The cleanest user-facing model is probably a tiny control command later, for
example:

```text
autossh --control restart --pid <pid>
```

Under POSIX, that helper can still send `SIGUSR1` for compatibility if desired.
Under Windows, it can speak to the native control channel.

## Suggested Implementation Order

1. Add hidden internal detached-mode parsing for Windows, for example
   `--autosshpp-detached`.
2. Preserve or reconstruct the original command line for Windows relaunch.
3. Implement Windows detached relaunch in `src/platform_control.cpp`.
4. Verify that `-f` returns immediately while the relaunched supervisor keeps
   running and still writes the pid file from the detached instance.
5. Implement a Windows restart control transport on top of the existing
   requested-action path.
6. Add a small control CLI shape if needed, for example
   `--control restart --pid <pid>`.
7. Revisit child termination behavior and graceful shutdown policy.

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
- POSIX only:
  - `-f` returns immediately while the detached supervisor keeps running
  - `SIGUSR1` triggers a restart
- Windows only:
  - `-f` returns immediately while the detached relaunched supervisor keeps
    running
  - the detached instance writes the pid file, not the short-lived parent
  - restarting through the Windows control path actually replaces the child
  - the hidden detached marker does not appear in user-facing help output

## Notes For The Next Session

- The current tree is buildable.
- The new config parser behavior was verified after a clean `cmake --fresh`
  rebuild.
- The POSIX control-path work was manually verified on Linux:
  - `-f` returned immediately and left a detached supervisor running
  - `SIGUSR1` restarted the child process
  - `SIGTERM` shut the detached supervisor down and removed the pid file
- The next session is expected to run on Windows 11, so focus should move to
  detached relaunch and Windows-native restart control.
- `.gitignore` still has local changes that were intentionally not included in
  the commit because they are unrelated to the current functional fixes.
