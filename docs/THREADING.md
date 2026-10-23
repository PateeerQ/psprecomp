# Threading modes

`PSPRECOMP_REAL_THREADS` is **OFF by default**. Without it,
`sceKernelStartThread` runs the guest entry inline, saves/restores the caller's
registers, and returns after that entry has ended. Sleep/delay calls do not
wait in host time; an unsatisfied semaphore/event/pool wait returns a timeout
without scheduling another guest. The callback, sub-interrupt and pool HLE
added alongside the scheduler is available in both modes.

To opt into experimental cooperative scheduling:

```sh
cmake -S . -B build-threads -DPSPRECOMP_REAL_THREADS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-threads --config Release
ctest --test-dir build-threads -C Release --output-on-failure
```

For the default mode use a separate build directory and omit the option (or
set it explicitly to `OFF`). CMake exports the selected `0`/`1` definition to
consumers of the `psprecomp` target. Do not mix runtime libraries and test
binaries from different configurations. A failed build must stop before tests.

## Opt-in execution

Each guest thread has a 16 MiB host stack and its own saved register context.
Only the holder of a shared CPU baton executes guest code. `StartThread`
returns before the new guest entry runs; waits release the baton so a caller
can post work after starting its worker. This is cooperative, not preemptive:
a guest that never reaches a wait can starve every other guest.

Windows uses an original adapter over SRW locks, condition variables and
`_beginthreadex`; its selected code path includes neither pthreads nor
`clock_gettime`. POSIX systems use pthreads, linked through CMake's
`Threads::Threads`. Ended host threads are joined before deletion, restart or
reset, so their handles and synchronization objects can be reclaimed.

The host must call the runtime and generated code from the boot thread, not
from arbitrary concurrent host callbacks. Once `module_start` returns, a host
that wants pending guests to run must keep yielding with
`psp_threadman_yield()`. Wait for all guest threads to end before resetting
ThreadMan, resetting other HLE subsystems or releasing guest memory. ThreadMan
reset fails loudly if a guest host thread is still live; it is not a shutdown
or asynchronous cancellation API. Reset does not itself free guest stack
allocations; delete the threads or reset SysMem only after guests have ended.

## Approximation boundaries

- `SleepThread` returns after one virtual tick even without a wakeup. A queued
  wakeup is consumed without that park; this is not faithful indefinite sleep.
- Semaphore, event, pool and thread-end waits retry, but an unbounded wait is
  capped at 500 iterations and reports a timeout. Their timeout handling remains
  the HLE approximation, not a complete PSP timeout-pointer implementation.
- Virtual time advances when guests park; it is not a wall-clock emulator.
  Individual POSIX condition waits request at most 400 microseconds. Windows
  rounds that request up to milliseconds. Host scheduling, timer resolution and
  CPU handover can add latency; neither bound is an execution-time guarantee.
- A CPU handover prefers already-waiting or newly-started guests, with a 20 ms
  host-time deadline. `psp_threadman_handover_expired()` counts fallback events.
  Priorities and suspend/resume metadata do not implement PSP priority scheduling
  or forced suspension. Termination is cooperative, not an OS thread kill.
- Callback/sub-interrupt delivery happens at explicit HLE checkpoints, not from
  an asynchronous hardware interrupt source. Both CB and non-CB waits currently
  deliver callbacks. Variable pools account capacity but do not reserve a
  separate contiguous region of guest memory.

## Regression coverage and validation scope

The HLE suite tests both selected execution contracts. The synthetic scheduler
suite posts work **after** starting four workers, checks that empty semaphore
waits cannot succeed early in opt-in mode, checks register/stack/exit-status
isolation, and exercises 320 create/start/wait/delete lifecycles and eight
restarts per run. Inline mode instead asserts synchronous completion and the
would-block timeout. Neither configuration silently skips that suite.

CI is configured for Windows/MSVC, Linux and macOS, with both option values and
repeated scheduler tests. Configuring a Windows CI job is not evidence that it
has run: a native MSVC build/test result is still required before release.
No new Lumberjack or retail-game compatibility result is claimed by these
synthetic tests. Real scheduling stays opt-in pending title-level validation.
