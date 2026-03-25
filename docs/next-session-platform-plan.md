# Next Session Plan

This document is a handoff for the next session.
The current tree already fixes the following review items:

- item 2: first-start gate-time behavior is now treated as a fatal startup failure
- item 5: monitor port parsing now validates before narrowing, so overflowed values do not wrap
- item 6: `AUTOSSH_*` parsing now uses explicit validation and reports readable errors

The current buildable snapshot also keeps `CMAKE_CXX_STANDARD` at `26` in
`CMakeLists.txt`. The implementation style in the recent changes uses modern
C++ facilities such as `std::expected`, `std::string_view`, `std::from_chars`,
and chrono-based normalization logic.

## Remaining Gaps

The main compatibility gaps that still need work are:

1. Real `-f` background mode.
   The current code still only changes retry policy. It does not detach or
   relaunch into a background process.

2. Cross-platform restart control.
   Original autossh exposes restart-through-signal semantics on POSIX
   (`SIGUSR1`). That exact interface does not exist on Windows, so this should
   become a higher-level "restart request" feature with platform-specific
   transports.

3. Startup failure accounting and `AUTOSSH_MAXSTART`.
   Failed launches should count as attempts. Right now the code increments
   `start_count_` only after a successful `bp::process` construction.

4. Child termination semantics.
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

## Recommended Architecture

Introduce a very small platform boundary instead of scattering `#ifdef`s
through the main loop.

Suggested model:

- shared supervisor consumes events:
  - `stop_requested`
  - `restart_requested`
  - `child_started`
  - `child_exited`
- platform backend produces those events
- shared supervisor decides what to do next

That suggests splitting responsibilities as follows:

- `src/autossh.cpp`
  - owns restart policy and monitoring state machine
- new platform abstraction, for example `src/platform_control.hpp`
  - background detach
  - control request subscription
  - optional process relaunch helpers

Avoid making the main loop know whether requests came from:

- POSIX signals
- Windows named events
- a named pipe
- a localhost control socket

It should only know that a restart or stop was requested.

## Background Mode

### POSIX

Implement real detach behavior using standard POSIX steps:

- fork
- parent exits successfully
- child calls `setsid()`
- redirect stdin/stdout/stderr to `/dev/null` unless logging is explicitly
  redirected elsewhere
- continue with the normal supervisor loop in the detached child

You may optionally do the classic second fork, but a single-fork plus `setsid()`
is probably enough for this project unless you want stricter daemon semantics.

### Windows

There is no `daemon()` equivalent. Use a detached relaunch model instead:

- parse `-f`
- relaunch the same executable with an internal marker like
  `--autosshpp-detached`
- create the new process with detached/background-friendly flags
- parent exits successfully
- child continues in normal supervisor mode

This keeps the public contract similar to POSIX without pretending the OS model
is the same.

The internal detached marker should not leak into user-facing help.

## Restart Control

### POSIX

Keep compatibility with `SIGUSR1`:

- `SIGUSR1` should enqueue or mark `restart_requested`
- `SIGINT` / `SIGTERM` should enqueue or mark `stop_requested`

### Windows

Do not try to emulate `SIGUSR1` literally. Provide a restart control channel.
Viable choices:

- named pipe
- localhost control socket
- named event plus a small helper CLI

The cleanest user-facing model is probably a tiny control command later, for
example:

```text
autosshpp --control restart --pid <pid>
```

Under POSIX, that helper can still send `SIGUSR1` for compatibility if desired.
Under Windows, it can speak to the native control channel.

## Startup Failure Accounting

This should be fixed before the detach/control work because it is self-contained.

Recommended rule:

- increment the attempt counter before the launch attempt
- classify:
  - launch failed before child creation
  - child launched and exited

`AUTOSSH_MAXSTART` should limit launch attempts, not just successfully spawned
children.

Backoff should also see repeated launch failures as fast failures.

## Suggested Implementation Order

1. Fix `start_count_` / max-start semantics.
2. Introduce a shared "requested action" path that can represent:
   - none
   - stop
   - restart
3. Add POSIX `SIGUSR1` on top of that shared action path.
4. Implement POSIX real `-f` detach.
5. Implement Windows detached relaunch.
6. Implement Windows restart control channel.
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
- `AUTOSSH_MAXSTART=1` with a missing `AUTOSSH_PATH`
- POSIX only:
  - `-f` returns immediately while the detached supervisor keeps running
  - `SIGUSR1` triggers a restart

## Notes For The Next Session

- The current tree is buildable.
- The new config parser behavior was verified after a clean `cmake --fresh`
  rebuild.
- `.gitignore` still has local changes that were intentionally not included in
  the commit because they are unrelated to the current functional fixes.
