# Arda

A cooperative multitasking scheduler for Arduino, built specifically for AVR microcontrollers (e.g., ATmega328, ATmega2560). Designed for memory-constrained environments with only 2KB RAM, where every byte counts.

While Arda compiles on other Arduino-compatible platforms, it is optimized for and tested primarily on AVR.

## Why Arda?

### Task Recovery (AVR) — Hard Abort

In cooperative multitasking, a single misbehaving task can freeze your entire system. If a task's `loop()` never returns (whether due to a bug, an unresponsive sensor, or an unexpected infinite loop), every other task starves. Traditional solutions either reset the entire MCU (losing all state) or just hope your code is perfect.

On AVR with Timer2, Arda is capable of **hard aborts**: forcibly terminating a stuck task without resetting the MCU. No other Arduino scheduler I'm aware of can do this. On a tiny 8-bit MCU with 2KB of RAM, that's basically black magic — a taste of preemptive multitasking where it shouldn't exist.

When a task exceeds its timeout, Timer2 fires and uses `setjmp`/`longjmp` to jump back to the scheduler. The current `loop()` is aborted, an optional recovery callback runs for cleanup, and the scheduler continues. The task remains Running and will retry next cycle (or the recovery callback can stop it). No reset, no lost state, no frozen system. Even if you don't need multitasking, self-recovering sketches are a neat enough trick.

Example:

```cpp
void sensor_setup() { /* init sensor */ }
void sensor_loop()  { /* may hang if sensor stops responding */ }
void sensor_recover() { /* reset sensor, clear state */ }

void setup() {
    int8_t id = OS.createTask("sensor", sensor_setup, sensor_loop, 0);
    OS.setTaskTimeout(id, 50);          // 50 ms max execution time
    OS.setTaskRecover(id, sensor_recover);
    OS.begin();
}

void loop() { OS.run(); }
```

### Built-in Shell

Arda includes a serial shell that lets you **control tasks at runtime** without writing any extra code. Connect via Serial Monitor and type commands to pause, resume, stop, or inspect any task. Useful for debugging, testing, and runtime diagnostics.

```
> l
0 R sh
1 R blinker
2 P sensor

> r 2
OK
```

Arda also supports **priority scheduling**: critical tasks run before less important ones when both are ready.

## Features

- Task recovery with configurable timeouts (hardware abort on AVR)
- Built-in serial shell for runtime control
- Priority scheduling (5 levels)
- Task lifecycle: setup, loop, teardown, recover callbacks
- Dynamic interval adjustment at runtime
- Batch operations (start/stop/pause multiple tasks)
- Detailed error codes (`ArdaError` enum)
- Trace callbacks for debugging
- Optimized for 2KB RAM

## Quick Start

```cpp
#include "Arda.h"

void blinker_setup() { pinMode(LED_BUILTIN, OUTPUT); }
void blinker_loop()  { digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); }

void setup() {
    OS.createTask("blinker", blinker_setup, blinker_loop, 500);
    OS.begin();
}

void loop() {
    OS.run();
}
```

## Cooperative Multitasking

This is **cooperative** multitasking, not preemptive. Key implications:

### You Must Call run() in loop()

**If you forget to call `OS.run()` in your Arduino `loop()` function, no tasks will execute.** The scheduler only runs tasks when `run()` is called.

```cpp
void loop() {
    OS.run();  // REQUIRED - tasks only execute when this is called
    // Other non-task code can go here
}
```

### Tasks Must Return Quickly

Each task's `loop()` function must return promptly so other tasks get CPU time. If a task never returns, the entire scheduler hangs.

**Bad:**
```cpp
TASK_LOOP(blocking) {
    while (waiting_for_something) {
        // Blocks forever - other tasks starve!
    }
}
```

**Good:**
```cpp
TASK_LOOP(nonblocking) {
    if (!waiting_for_something) {
        // Do work
    }
    // Returns immediately, will be called again next cycle
}
```

### Long Operations Need State Machines

Break long operations into steps:

```cpp
static int step = 0;
TASK_LOOP(multistep) {
    switch (step) {
        case 0: doPartOne(); step++; break;
        case 1: doPartTwo(); step++; break;
        case 2: doPartThree(); step = 0; break;
    }
}
```

### Interrupt Safety (ISRs)

**Arda is strictly single-threaded and not safe to call from interrupt service routines.**

Calling any Arda method from an ISR can corrupt scheduler state because operations like `run()`, `startTask()`, and `stopTask()` perform multi-step state changes that are not atomic. An interrupt firing mid-operation will see inconsistent state.

To communicate between ISRs and tasks:

```cpp
// Declare a volatile flag for ISR communication
volatile bool dataReady = false;

// In your ISR - only set the flag, don't call Arda
ISR(TIMER1_COMPA_vect) {
    dataReady = true;
}

// In your task - poll the flag
TASK_LOOP(processor) {
    if (dataReady) {
        dataReady = false;
        processData();
    }
}
```

Key points:
- Use `volatile` for variables shared between ISRs and tasks
- Keep ISRs minimal - set flags, buffer data, nothing else
- Let tasks poll flags and do the actual work
- Never call `OS.run()`, `OS.startTask()`, `OS.yield()`, etc. from an ISR

## Task States

| State | Description |
|-------|-------------|
| `TaskState::Stopped` | Task is not running (can be started or deleted) |
| `TaskState::Running` | Task is active and executing on each scheduler cycle |
| `TaskState::Paused` | Task is temporarily suspended (can be resumed) |
| `TaskState::Invalid` | Returned by getTaskState() for invalid or deleted task IDs |

## API Reference

### Task Management

| Method | Description |
|--------|-------------|
| `createTask(name, setup, loop, interval, teardown, autoStart)` | Register a new task. Returns task ID (-1 on failure). Name must be non-empty, max `ARDA_MAX_NAME_LEN-1` chars (default 15), and is **case-sensitive**. If `begin()` was called and `autoStart` is true (default), task auto-starts immediately; on start failure, task is deleted and -1 is returned (check `getError()`). Set `autoStart=false` to create in STOPPED state and handle start failures manually. When `ARDA_NO_NAMES` is defined, name is ignored; use the nameless `createTask(setup, loop, interval, teardown, autoStart)` overload instead. **Warning:** `interval=0` with high priority will starve all lower-priority tasks, so ensure such tasks return quickly. |
| `createTask(name, setup, loop, interval, teardown, autoStart, priority)` | Create a task with explicit priority. See `TaskPriority` enum for levels (`Lowest` through `Highest`). Not available if `ARDA_NO_PRIORITY` is defined. |
| `createTask(name, setup, loop, interval, teardown, autoStart, priority, timeout, recover)` | Create a task with priority, timeout, and recovery callback. `timeout`: max execution time in ms (0 = disabled). `recover`: called after forced timeout abort (can be nullptr). Requires `ARDA_TASK_RECOVERY` and `ARDA_NO_PRIORITY` must not be defined. |
| `deleteTask(id)` | Delete a stopped task, freeing its slot for reuse. Cannot delete currently executing task. |
| `killTask(id)` | Stop and delete a task in one call. Convenience for `stopTask(id)` then `deleteTask(id)`. Returns false if stop fails or teardown changes state (task remains in whatever state teardown left it). Also fails for invalid IDs or if the task is currently executing. |
| `startTask(id, runImmediately)` | Start a stopped task (runs setup callback). Returns `StartResult` enum - see below. Set `runImmediately=true` to skip the initial interval wait for interval-based tasks; `false` (default) waits one full interval. Note: Tasks started during a run() cycle run on the next cycle regardless of this flag. Resets `runCount` to 0. |
| `pauseTask(id)` | Pause a running task |
| `resumeTask(id)` | Resume a paused task |
| `stopTask(id)` | Stop a task (runs teardown if provided). Returns `StopResult` enum - see below. |
| `setTaskTimeout(id, ms)` | Set max execution time for a task (0 = disabled). Requires `ARDA_TASK_RECOVERY`. On soft platforms: timeout callback fires after task returns. On AVR with Timer2: task is forcibly aborted (timeout callback is skipped; trace events and recover() run instead). |
| `getTaskTimeout(id)` | Get a task's timeout setting (0 if disabled or invalid task). Requires `ARDA_TASK_RECOVERY`. |
| `heartbeat()` | Reset the current task's timeout timer to its configured value. Useful for long-running tasks that want to signal progress without changing their timeout setting. Returns false if called outside task context or task has no timeout configured. **AVR only** (requires hardware timer support). |
| `setTaskRecover(id, cb)` | Set recovery callback for a task (called after forced timeout abort). Requires `ARDA_TASK_RECOVERY` and hardware support (AVR with Timer2); returns false with `NotSupported` on other platforms. |
| `hasTaskRecover(id)` | Check if a task has a recovery callback. Requires `ARDA_TASK_RECOVERY` and hardware support; returns false on other platforms. |
| `setTaskRecoveryEnabled(enabled)` | Enable/disable task recovery globally at runtime. Requires `ARDA_TASK_RECOVERY`. On non-AVR this controls soft timeouts and callbacks. |
| `isTaskRecoveryEnabled()` | Check if task recovery is currently enabled. Requires `ARDA_TASK_RECOVERY`. Hardware availability is reported by `isTaskRecoveryAvailable()`. On non-AVR, this flag only controls soft timeouts and callbacks. |
| `setTaskPriority(id, priority)` | Set task priority (`TaskPriority` enum). Returns false with `InvalidValue` if invalid. Not available if `ARDA_NO_PRIORITY` is defined. |
| `getTaskPriority(id)` | Get task priority. Returns `TaskPriority::Lowest` for invalid tasks. Not available if `ARDA_NO_PRIORITY` is defined. |
| `setTaskInterval(id, ms, resetTiming)` | Change a task's execution interval at runtime. By default (`resetTiming=false`), keeps existing timing (next run based on lastRun + new interval). Set `resetTiming=true` to reset timing so task waits the full new interval from now. |
| `findTaskByName(name)` | Find a task by name (case-sensitive by default), returns ID or -1 if not found. Always returns -1 when `ARDA_NO_NAMES` is defined. |
| `renameTask(id, newName)` | Rename an existing task. Same validation rules as createTask. Returns false with `ArdaError::NotSupported` when `ARDA_NO_NAMES` is defined. |
| `startTasks(ids, count, failedId*)` | Start multiple tasks. Returns count of successful starts. Optional `failedId` receives first failed task ID. |
| `stopTasks(ids, count, failedId*)` | Stop multiple tasks. Returns count stopped. Optional `failedId` receives first failed task ID. |
| `pauseTasks(ids, count, failedId*)` | Pause multiple tasks. Returns count of successful pauses. Optional `failedId` receives first failed task ID. |
| `resumeTasks(ids, count, failedId*)` | Resume multiple tasks. Returns count of successful resumes. Optional `failedId` receives first failed task ID. |
| `startAllTasks()` | Start all stopped tasks. Returns count of successful starts. |
| `stopAllTasks()` | Stop all running/paused tasks. Returns count of tasks stopped. |
| `pauseAllTasks()` | Pause all running tasks. Returns count of successful pauses. |
| `resumeAllTasks()` | Resume all paused tasks. Returns count of successful resumes. |

#### startTask Return Values

`startTask()` returns a `StartResult` enum with three distinct values:

| Value | Description |
|-------|-------------|
| `StartResult::Success` | Task started, setup ran (if defined), task is Running |
| `StartResult::SetupChangedState` | Setup ran but modified task state (e.g., stopped itself) |
| `StartResult::Failed` | Task couldn't be started - check `getError()` for reason |

```cpp
StartResult result = OS.startTask(taskId);
switch (result) {
    case StartResult::Success:
        // Fully successful - task is now Running
        break;
    case StartResult::SetupChangedState:
        // Setup ran but changed state (e.g., called stopTask on itself)
        // Check getError() for StateChanged
        break;
    case StartResult::Failed:
        // Task couldn't be started (wrong state, invalid ID, callback depth)
        break;
}
```

#### stopTask Return Values

`stopTask()` returns a `StopResult` enum with four distinct values:

| Value | Description |
|-------|-------------|
| `StopResult::Success` | Task stopped and teardown ran successfully (or no teardown defined) |
| `StopResult::TeardownSkipped` | Task stopped but teardown was NOT run (callback depth exceeded) |
| `StopResult::TeardownChangedState` | Task stopped, teardown ran but modified task state unexpectedly |
| `StopResult::Failed` | Task couldn't be stopped - check `getError()` for reason |

```cpp
StopResult result = OS.stopTask(taskId);
switch (result) {
    case StopResult::Success:
        // Fully successful
        break;
    case StopResult::TeardownSkipped:
        // Task is stopped, but teardown did NOT run - may need manual cleanup
        // Check getError() for CallbackDepth
        break;
    case StopResult::TeardownChangedState:
        // Teardown ran but restarted the task or changed its state
        // Check getError() for StateChanged
        break;
    case StopResult::Failed:
        // Task couldn't be stopped (wrong state, invalid ID)
        break;
}
```

> **Design note: Return enums vs getError()**
>
> `startTask()` and `stopTask()` return result enums AND set `error_`. This is intentional:
> - **Return enum** = *what happened* (outcome category: success, partial success, failure)
> - **getError()** = *why* (specific reason: `InvalidId`, `CallbackDepth`, `StateChanged`, etc.)
>
> This avoids an explosion of enum values (`FailedInvalidId`, `FailedWrongState`, ...) while preserving both the high-level outcome and detailed diagnostics.

### Scheduler

| Method | Description |
|--------|-------------|
| `begin()` | Initialize scheduler and start all registered tasks. Returns number successfully started, or -1 if already begun. Use `createTask(..., autoStart=false)` for tasks that should start in Stopped state. |
| `run()` | Execute the scheduler (call in loop()). Returns false with `WrongState` error if `begin()` not called. |
| `reset(preserveCallbacks)` | Stop all tasks and reset scheduler to initial state. You must call `begin()` again after reset to restart the scheduler. By default clears user callbacks (timeout, startFailure, trace); set `preserveCallbacks=true` to keep them. Returns true if all stops succeeded, false if any stop failed or teardown was skipped/changed state (check `getError()`). |
| `yield()` | Give other tasks a chance to run. **Requires `ARDA_YIELD`.** **Discouraged.** See [Appendix: yield()](#appendix-yield). |
| `uptime()` | Milliseconds since begin(), or 0 if begin() not yet called |
| `hasBegun()` | Returns true if begin() has been called |
| `setTimeoutCallback(cb)` | Set callback invoked when a task exceeds its timeout. Requires `ARDA_TASK_RECOVERY`. |
| `setStartFailureCallback(cb)` | Set callback invoked for each task that fails to start during `begin()` |
| `setTraceCallback(cb)` | Set callback for debugging/tracing task execution (nullptr to disable) |
| `setShellStream(stream)` | Set Stream for shell I/O (default: Serial). See [Built-in Shell](#built-in-shell). |
| `setShellEcho(bool)` | Enable/disable echoing commands with "> " prefix (default: on) |
| `isShellRunning()` | Returns true if shell task exists and is Running |
| `exec(cmd)` | Execute a shell command programmatically |

> **Tip: `reset(true)` preserves callbacks**
>
> Use `reset(true)` when you want to tear down all tasks but keep your monitoring infrastructure (timeout, trace, startFailure callbacks) intact. For example, if you have a trace callback logging to SD card or a timeout callback that triggers a hardware watchdog, `reset(true)` lets you restart fresh without re-registering them.

### Task Info

| Method | Description |
|--------|-------------|
| `getTaskCount()` | Number of active (non-deleted) tasks |
| `getSlotCount()` | Total task slots used (includes deleted) - for iteration |
| `getMaxTasks()` | Maximum task capacity (compile-time constant) |
| `getTaskName(id)` | Get task name (nullptr if deleted/invalid). Always returns nullptr when `ARDA_NO_NAMES` is defined. |
| `getTaskState(id)` | Get task state (Running/Paused/Stopped/Invalid) |
| `getTaskRunCount(id)` | Execution count (returns 0 if invalid - use `isValidTask()` first) |
| `getTaskInterval(id)` | Interval in ms (returns 0 if invalid - use `isValidTask()` first) |
| `getTaskLastRun(id)` | millis() snapshot when task last ran (returns 0 if invalid or never ran - use `isValidTask()` first) |
| `getCurrentTask()` | ID of currently executing task (-1 if none) |
| `isValidTask(id)` | Returns true if task ID refers to a valid, non-deleted task |
| `isTaskYielded(id)` | Returns true if task is currently inside a yield() call. Only available if `ARDA_YIELD` is defined. |
| `getValidTaskIds(outIds, maxCount)` | Fill array with valid task IDs. Returns total count of valid tasks (may exceed maxCount). Pass nullptr to just get the count. |
| `hasTaskSetup(id)` | Returns true if task has a setup callback configured |
| `hasTaskLoop(id)` | Returns true if task has a loop callback configured |
| `hasTaskTeardown(id)` | Returns true if task has a teardown callback configured |
| `isTaskRecoveryAvailable()` | Static. Returns true if hardware supports task recovery (compile-time constant). |
| `isWatchdogEnabled()` | Static. Returns true if `ARDA_WATCHDOG` is defined (compile-time constant). |

> **Warning: Task Iteration**
> Do NOT use `getTaskCount()` as the loop bound when iterating by index. Deleted tasks leave gaps, so valid tasks may exist at indices beyond `getTaskCount()`. Use `getSlotCount()` with `isValidTask()` instead:
> ```cpp
> for (int8_t i = 0; i < OS.getSlotCount(); i++) {
>     if (!OS.isValidTask(i)) continue;
>     // ... use task i
> }
> ```
> Or use `getValidTaskIds()` to get an array of valid IDs:
> ```cpp
> int8_t ids[ARDA_MAX_TASKS];
> int8_t count = OS.getValidTaskIds(ids, ARDA_MAX_TASKS);
> for (int8_t i = 0; i < count; i++) {
>     // ... use ids[i]
> }
> ```

> **Warning: Iterator Invalidation**
> Do not delete tasks while iterating with `getSlotCount()` or using an array from `getValidTaskIds()`. Deleting a task during iteration may cause you to skip tasks or access invalid slots. If you need to delete tasks based on some condition, collect the IDs first, then delete after the iteration completes.

### Batch Operations

Perform operations on multiple tasks at once:

```cpp
int8_t taskIds[] = {task1, task2, task3};

// Start all tasks
int8_t started = OS.startTasks(taskIds, 3);
if (started < 3) {
    // Some tasks failed to start - check getError() for first failure
}

// Pause all tasks
int8_t paused = OS.pauseTasks(taskIds, 3);

// Resume all tasks
int8_t resumed = OS.resumeTasks(taskIds, 3);

// Stop all tasks
int8_t stopped = OS.stopTasks(taskIds, 3);
```

Batch operations:
- Attempt all operations regardless of individual failures
- Return the count of successful operations
- Set error to the first failure encountered (if any) - first failures are typically more diagnostic

> **Warning: Partial Failures**
> Batch operations may partially succeed. Always compare the return value against the expected count. Use the optional `failedId` parameter to identify which task failed first:
> ```cpp
> int8_t failedTask;
> int8_t started = OS.startTasks(taskIds, 3, &failedTask);
> if (started < 3) {
>     Serial.print("Task failed: "); Serial.println(failedTask);
>     Serial.print("Error: "); Serial.println((int)OS.getError());
> }
> ```

### Task Timeouts

Arda can detect when tasks exceed their expected execution time:

```cpp
// Called when a task exceeds its timeout
void onTimeout(int8_t taskId, uint32_t actualDurationMs) {
    Serial.print(F("Task "));
    Serial.print(OS.getTaskName(taskId));
    Serial.print(F(" exceeded timeout: "));
    Serial.print(actualDurationMs);
    Serial.println(F("ms"));
}

void setup() {
    OS.setTimeoutCallback(onTimeout);

    int8_t id = OS.createTask("worker", worker_setup, worker_loop, 100);
    OS.setTaskTimeout(id, 50);  // Warn if task takes > 50ms

    OS.begin();
}
```

**Important limitations:**
- Timeouts are checked **after** the task returns - Arda cannot interrupt a blocking task
- The callback is informational only; it cannot preempt or kill the task
- Use this for debugging and monitoring, not for hard real-time guarantees
- On cooperative systems, a truly blocking task will still hang the scheduler

On AVR with Timer2, timeouts *can* interrupt blocking tasks. Task recovery uses hardware timer interrupts and `setjmp`/`longjmp` to forcibly abort stuck tasks. On other platforms, timeouts are "soft": checked after `loop()` returns, useful for monitoring but not enforcement.

### Debug/Trace Callback

Monitor task execution for debugging:

```cpp
void onTrace(int8_t taskId, TraceEvent event) {
    const char* eventNames[] = {
        "STARTING", "STARTED", "LOOP_BEGIN", "LOOP_END",
        "STOPPING", "STOPPED", "PAUSED", "RESUMED", "DELETED",
        "ABORTED", "RECOVER_ABORTED"  // ARDA_TASK_RECOVERY only
    };
    uint8_t idx = static_cast<uint8_t>(event);
    if (idx >= sizeof(eventNames)/sizeof(eventNames[0])) return;  // Guard
    Serial.print(OS.getTaskName(taskId));
    Serial.print(F(": "));
    Serial.println(eventNames[idx]);
}

void setup() {
    OS.setTraceCallback(onTrace);
    // ... create tasks ...
    OS.begin();
}
```

Trace events:
- `TraceEvent::TaskStarting` - About to run setup() (fires before callback)
- `TraceEvent::TaskStarted` - setup() completed, task is now Running
- `TraceEvent::TaskLoopBegin` - About to run loop() (fires before callback)
- `TraceEvent::TaskLoopEnd` - loop() completed
- `TraceEvent::TaskStopping` - About to run teardown() (only emitted if teardown exists)
- `TraceEvent::TaskStopped` - Task is now Stopped
- `TraceEvent::TaskPaused` - Task state changed to Paused
- `TraceEvent::TaskResumed` - Task state changed to Running
- `TraceEvent::TaskDeleted` - Task was deleted (already invalidated; callback receives ID only)
- `TraceEvent::TaskAborted` - Task loop() was force-aborted due to timeout (AVR with Timer2 only)
- `TraceEvent::RecoverAborted` - Task recover() was also force-aborted (AVR with Timer2 only)

> **Note:** The "ing" variants (`TaskStarting`, `TaskStopping`, `TaskLoopBegin`) bracket user callbacks - they fire *before* your code runs. Pause/resume have no callbacks to bracket, so only "ed" variants exist.

Set callback to `nullptr` to disable tracing (recommended for production).

When `ARDA_NO_NAMES` is defined, `getTaskName(taskId)` returns nullptr. Use the numeric task ID for identification in trace output instead.

### Start Failure Callback

To get detailed information when tasks fail to start during `begin()`:

```cpp
void onStartFailed(int8_t taskId, ArdaError error) {
    Serial.print(F("Task "));
    Serial.print(taskId);
    Serial.print(F(" failed to start: "));
    Serial.println(Arda::errorString(error));
}

void setup() {
    OS.setStartFailureCallback(onStartFailed);
    // ... create tasks ...
    OS.begin();  // Callback called for each task that fails
}
```

## Built-in Shell

Arda includes a built-in serial shell task (at ID 0) for runtime task management. The shell is enabled by default and provides a command-line interface over Serial.

### Shell Commands

**Core commands (always available, even with `ARDA_SHELL_MINIMAL`):**

| Command | Description |
|---------|-------------|
| `b <id>` | Begin task |
| `s <id>` | Stop task |
| `p <id>` | Pause task |
| `r <id>` | Resume task |
| `k <id>` | Kill task (stop + delete in one command) |
| `d <id>` | Delete task (must be stopped first) |
| `l` | List tasks (format: `ID STATE NAME`, e.g., `0 R sh`). When `ARDA_NO_NAMES` is defined, name is omitted. |
| `h` or `?` | Help (list commands) |
| `o 0\|1` | Set echo off/on (shows current state if no argument). Not available with `ARDA_NO_SHELL_ECHO`. |

**Extended commands (not available with `ARDA_SHELL_MINIMAL`):**

| Command | Description |
|---------|-------------|
| `i <id>` | Task info (interval, runs, priority, timeout) |
| `w <id>` | When: shows time since last run and next due (or `[P]`/`[S]` if paused/stopped) |
| `a <id> <ms>` | Adjust interval (set new interval in milliseconds) |
| `t <id> <ms>` | Set timeout (requires `ARDA_TASK_RECOVERY`) |
| `y <id> <pri>` | Set priority 0-4 (not available with `ARDA_NO_PRIORITY`) |
| `n <id> <name>` | Rename task (not available with `ARDA_NO_NAMES`) |
| `g <id>` | Go: begin task with immediate execution (like `startTask(id, true)`) |
| `e` | Last error code and message |
| `c` | Clear error (resets `getError()` to `ArdaError::Ok`) |
| `m` | Memory info (task count, max tasks, slots used) |
| `u` | Uptime (seconds since begin()) |
| `v` | Arda version |

**State codes in `l` output:** `R` = Running, `P` = Paused, `S` = Stopped

**Buffer size note:** Extended commands like `n <id> <name>` and `a <id> <ms>` need sufficient buffer space. The default `ARDA_SHELL_BUF_SIZE` of 16 is sufficient for most use cases, but increase it to 20+ if using long task names or very large interval values.

### Shell Configuration

```cpp
// Disable shell entirely (saves ~600 bytes flash, ~50 bytes RAM)
#define ARDA_NO_SHELL
#include "Arda.h"
```

```cpp
// Minimal shell - only core commands (b/s/p/r/k/d/l/h/o), saves ~200 bytes
// Removes extended commands: i, w, a, t, y, n, g, e, c, m, u, v
#define ARDA_SHELL_MINIMAL
#include "Arda.h"
```

```cpp
// Manual start - shell doesn't auto-start with begin()
#define ARDA_SHELL_MANUAL_START
#include "Arda.h"

// Later, manually start/stop the shell:
OS.startShell();
OS.stopShell();
```

```cpp
// Custom command buffer size (default: 16, min 8 recommended)
// Increase if you need longer task names in output or programmatic commands
#define ARDA_SHELL_BUF_SIZE 32
#include "Arda.h"
```

### Shell API

```cpp
// Set a different Stream for shell I/O (default: Serial)
// WARNING: Stream must remain valid for shell lifetime
OS.setShellStream(Serial1);

// Check if shell is running
if (OS.isShellRunning()) { ... }

// Execute shell commands programmatically
OS.exec("p 1");     // Pause task 1
OS.exec("l");       // List tasks (output goes to shellStream)
OS.exec("i 2");     // Info about task 2

// Get shell task ID (always 0 when shell enabled)
int8_t shellId = OS.getShellTaskId();

// Disable command echo (on by default)
OS.setShellEcho(false);
```

**Command echo:** By default, the shell echoes received commands back with a `> ` prefix (useful since serial monitors often don't show what you typed). Disable with `OS.setShellEcho(false)`. Define `ARDA_NO_SHELL_ECHO` to remove echo support entirely.

**`exec()` notes:**
- Command must be shorter than `ARDA_SHELL_BUF_SIZE` (default 16)
- Does not echo commands (echo is for serial input only)
- Re-entrant safe: nested calls (e.g., from a callback triggered by a shell command) are silently ignored
- Works even after shell task is deleted (uses class members, not the task)

### Shell Behavior

- **Reserved ID 0**: When shell is enabled, user tasks start at ID 1
- **Self-referential**: Shell appears in its own `l` listing
- **Self-controllable**: Shell can pause/stop itself (`p 0`, `s 0`) - user loses serial control until reboot. This is intentional: your code can still call `OS.exec()` or manipulate tasks directly, and stopping the shell frees CPU cycles.
- **Self-destructible**: Use `k 0` to kill the shell (stop+delete), freeing task slot 0 for reuse. Or `s 0` then `OS.exec("d 0")` from another task. This is useful when you need the extra task slot and no longer need serial control. Note: `d` requires Stopped state; `s 0` stops the shell so it won't read follow-up commands.
- **reset() restores shell**: Calling `reset()` removes all user tasks but reinitializes the shell task (in Stopped state) for the global `OS` instance
- **Requires global OS**: Shell requires the global `OS` instance (incompatible with `ARDA_NO_GLOBAL_INSTANCE`)
- **Global instance only**: Shell is only initialized on the global `OS`. Local `Arda` instances work normally without a shell task (their tasks start at ID 0)

### Shell Resource Impact

| Configuration | Flash (AVR) | RAM (AVR) |
|--------------|-------------|-----------|
| Full shell (default) | ~600 bytes | ~50 bytes |
| `ARDA_SHELL_MINIMAL` | ~400 bytes | ~50 bytes |
| `ARDA_NO_SHELL` | 0 | 0 |

## Priority Scheduling

By default, Arda supports 5-level task priority. Higher-priority tasks run before lower-priority tasks when both are ready in the same scheduling cycle.

### TaskPriority Enum

Use the `TaskPriority` enum:

| Level | Value | Description |
|-------|-------|-------------|
| `TaskPriority::Lowest` | 0 | Background tasks |
| `TaskPriority::Low` | 1 | Below normal priority |
| `TaskPriority::Normal` | 2 | Default priority (assigned to all new tasks) |
| `TaskPriority::High` | 3 | Above normal priority |
| `TaskPriority::Highest` | 4 | Critical tasks |

### Example Usage

```cpp
// Create tasks with explicit priority
int8_t criticalId = OS.createTask("critical", setup, loop, 100, nullptr, true, TaskPriority::Highest);
int8_t normalId = OS.createTask("normal", setup, loop, 100);  // Default (TaskPriority::Normal)
int8_t bgId = OS.createTask("background", setup, loop, 100, nullptr, true, TaskPriority::Lowest);

// Change priority at runtime
OS.setTaskPriority(normalId, TaskPriority::High);

// Query priority
TaskPriority priority = OS.getTaskPriority(criticalId);  // Returns TaskPriority::Highest
```

### Priority Behavior

- **Within a scheduling cycle**: Higher-priority ready tasks run first
- **Interval-based**: Priority only matters when multiple tasks are ready at the same time
- **Tie-breaking**: Tasks with equal priority run in snapshot order (typically creation order)
- **Zero memory overhead**: Priority is packed into unused bits of the existing flags byte

> **Warning: Starvation**
> A high-priority task with `interval=0` (runs every cycle) will starve lower-priority tasks completely. Ensure high-priority tasks either have non-zero intervals or return quickly.

### Disabling Priority

If you don't need priority scheduling and want to save code size (especially on constrained devices like ATmega328), define `ARDA_NO_PRIORITY` before including Arda.h. This:

- Removes the priority-scanning loop (simpler, faster scheduling)
- Restores array-order execution (tasks run in creation/snapshot order)
- Removes `TaskPriority` enum and priority API (`setTaskPriority`, `getTaskPriority`, priority overload of `createTask`)

## Error Codes

Use `getError()` after a failed operation to get detailed error information. Successful operations clear the error state. Use `clearError()` to manually reset the error state to `ArdaError::Ok`. Use `Arda::errorString(error)` to convert an error code to a human-readable string for debugging.

**Note:** Query methods (getters) return sentinel values (nullptr, `TaskState::Invalid`, -1, 0) for invalid tasks without setting error codes. Use `isValidTask(id)` to check validity before calling getters where the return value could be ambiguous (e.g., `getTaskRunCount()` returns 0 for both invalid tasks and tasks that haven't run yet).

```cpp
// Defensive pattern for ambiguous getters
if (OS.isValidTask(taskId)) {
    uint32_t runs = OS.getTaskRunCount(taskId);  // Now 0 means "hasn't run"
} else {
    // Task doesn't exist
}
```

| Error Code | Description |
|------------|-------------|
| `ArdaError::Ok` | No error |
| `ArdaError::NullName` | Task name was null |
| `ArdaError::EmptyName` | Task name was empty string |
| `ArdaError::NameTooLong` | Task name exceeds ARDA_MAX_NAME_LEN-1 |
| `ArdaError::DuplicateName` | Task with this name already exists |
| `ArdaError::MaxTasks` | Maximum task limit reached |
| `ArdaError::InvalidId` | Task ID is out of range or deleted |
| `ArdaError::WrongState` | Task is in wrong state for operation |
| `ArdaError::TaskExecuting` | Cannot delete currently executing task |
| `ArdaError::TaskYielded` | Cannot delete task that has yielded. Only exists if `ARDA_YIELD` is defined. |
| `ArdaError::AlreadyBegun` | begin() was already called |
| `ArdaError::CallbackDepth` | Maximum callback nesting depth exceeded |
| `ArdaError::StateChanged` | Callback modified task state unexpectedly |
| `ArdaError::InCallback` | Operation not allowed from current context (reset() from callback, or run() reentrancy) |
| `ArdaError::NotSupported` | Feature disabled at compile time (e.g., `renameTask` when `ARDA_NO_NAMES` is defined) |
| `ArdaError::InvalidValue` | Parameter value out of valid range (e.g., priority > 4) |
| `ArdaError::TaskAborted` | Task was forcibly aborted due to timeout. Requires `ARDA_TASK_RECOVERY`. |

## Macros (Optional)

Arda provides optional macros to reduce boilerplate. You can also use plain functions and `createTask()` directly; the macros just auto-generate function names to avoid repetition.

**Without macros (recommended for clarity):**
```cpp
void blink_setup() { pinMode(LED_BUILTIN, OUTPUT); }
void blink_loop() { digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); }

void setup() {
    int8_t blinkId = OS.createTask("blink", blink_setup, blink_loop, 500);
    OS.begin();
}
```

**With macros (less repetition, more magic):**
```cpp
TASK_SETUP(blink) { pinMode(LED_BUILTIN, OUTPUT); }
TASK_LOOP(blink) { digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); }

void setup() {
    REGISTER_TASK_ID(blinkId, blink, 500);
    OS.begin();
}
```

### Available Macros

```cpp
TASK_SETUP(name)      // Define setup function: void name_setup()
TASK_LOOP(name)       // Define loop function: void name_loop()
TASK_TEARDOWN(name)   // Define teardown function: void name_teardown()
REGISTER_TASK(name, interval)  // Register task (discards returned ID)
REGISTER_TASK_ID(id, name, interval)  // Register task and capture ID
REGISTER_TASK_WITH_TEARDOWN(name, interval)
REGISTER_TASK_ID_WITH_TEARDOWN(id, name, interval)
```

### Macros for Custom Scheduler Instances

If using `ARDA_NO_GLOBAL_INSTANCE` or multiple schedulers, use the `_ON` variants:

```cpp
#define ARDA_NO_GLOBAL_INSTANCE
#include "Arda.h"

Arda myScheduler;

REGISTER_TASK_ON(myScheduler, blinker, 500);
REGISTER_TASK_ID_ON(taskId, myScheduler, reporter, 1000);
REGISTER_TASK_ON_WITH_TEARDOWN(myScheduler, worker, 100);
REGISTER_TASK_ID_ON_WITH_TEARDOWN(taskId, myScheduler, worker, 100);
```

## Timing Behavior

### Interval Scheduling

Tasks with intervals guarantee a minimum gap between executions. If a task with a 100ms interval runs at t=105, the next run will be at t=205 or later. `getTaskLastRun()` returns the actual execution time.

### Catch-up Prevention

If the scheduler falls behind (e.g., due to a long-running task), it does not run multiple catch-up iterations.

A task with 100ms interval last ran at t=100. If the next `run()` call happens at t=350, the task runs once and `lastRun` is set to t=350. The next execution will be at t=450 or later. If you need strict periodicity, implement your own timing logic using `millis()` inside the task.

### millis() Overflow

Arduino's `millis()` overflows after ~49 days. Arda handles this correctly - interval calculations and `uptime()` continue to work due to unsigned arithmetic properties.

### Run Count Overflow

`getTaskRunCount()` returns a `uint32_t` that increments on each `loop()` execution. This counter resets to 0 on each `startTask()` call, so it tracks runs since the most recent start, not lifetime executions. At 1ms intervals, this overflows after ~49 days. If you need cumulative execution counts across restarts, maintain your own counter.

### Callback Depth Limit

Arda limits nested callback depth to `ARDA_MAX_CALLBACK_DEPTH` (default: 8) to prevent stack overflow. This applies when:
- A task's `setup()` calls `startTask()` on another task
- A task's `teardown()` calls `stopTask()` on another task
- A task's `loop()` triggers other tasks via `yield()`

If the limit is exceeded, `startTask()` fails with `ArdaError::CallbackDepth`, and `stopTask()` returns `StopResult::TeardownSkipped` with `ArdaError::CallbackDepth` (task is stopped but teardown was not run).

**Tuning `ARDA_MAX_CALLBACK_DEPTH`:**
- **Lower (4-6):** For memory-constrained boards (ATmega328) or tasks with large local variables
- **Default (8):** Good balance for most applications
- **Higher (10-16):** If you have complex task chains that legitimately need deep nesting

Each level of nesting consumes stack space for the callback's local variables plus ~20-50 bytes of return address and saved registers.

## Configuration

### Compile-Time Constants

Define these before including `Arda.h`, or edit the header directly:

```cpp
#define ARDA_MAX_TASKS 8           // Maximum number of tasks (default: 16, max: 127)
#define ARDA_MAX_NAME_LEN 12       // Task name buffer size (default: 16, usable: 15 chars)
#define ARDA_MAX_CALLBACK_DEPTH 4  // Max nested callbacks (default: 8)
#include "Arda.h"
```

### Optional Features

```cpp
// Case-insensitive task name matching
#define ARDA_CASE_INSENSITIVE_NAMES
#include "Arda.h"

OS.createTask("MyTask", ...);
OS.findTaskByName("mytask");  // Returns the task ID (matches "MyTask")
```

Case-insensitive matching only handles ASCII letters (A-Z, a-z). Extended ASCII and UTF-8 characters are compared as-is.

```cpp
// Disable global OS instance (create your own scheduler instances)
#define ARDA_NO_GLOBAL_INSTANCE
#include "Arda.h"

Arda scheduler1;
Arda scheduler2;  // Multiple independent schedulers
```

```cpp
// Disable priority scheduling (saves code size, uses array-order execution)
#define ARDA_NO_PRIORITY
#include "Arda.h"

// Priority API (setTaskPriority, getTaskPriority, TaskPriority enum) will not be available
// Tasks will execute in snapshot order (creation order) instead of priority order
```

```cpp
// Use short messages to save flash (errors + help text)
// e.g., "InvalidId" vs "Invalid task ID", "p pause" vs "p <id>        pause task"
#define ARDA_SHORT_MESSAGES
#include "Arda.h"
```

```cpp
// Disable task names entirely (saves ARDA_MAX_NAME_LEN bytes per task)
#define ARDA_NO_NAMES
#include "Arda.h"

// Name-related APIs degrade gracefully:
// - getTaskName(id) returns nullptr
// - findTaskByName(name) returns -1
// - renameTask(id, name) returns false with ArdaError::NotSupported
// - createTask() accepts a simplified signature without name parameter
// - Existing code using createTask(name, ...) still compiles (name is ignored)
```

```cpp
// Enable yield() - DISCOURAGED, see Appendix: yield()
#define ARDA_YIELD
#include "Arda.h"
```

```cpp
// Disable task recovery (soft watchdog) - saves ~200 bytes on AVR
// Task recovery is enabled by default on all platforms
#define ARDA_NO_TASK_RECOVERY
#include "Arda.h"
```

### Combining Configuration Options

For maximum memory savings on extremely constrained devices (e.g., ATtiny), combine multiple options:

```cpp
#define ARDA_MAX_TASKS 4
#define ARDA_NO_NAMES
#define ARDA_NO_PRIORITY
#define ARDA_NO_SHELL
#define ARDA_NO_TASK_RECOVERY
#define ARDA_SHORT_MESSAGES
#include "Arda.h"
```

This configuration saves:
- 16 bytes per task from disabling names (default `ARDA_MAX_NAME_LEN`)
- ~100-200 bytes of code from disabling priority
- ~600 bytes flash + ~50 bytes RAM from disabling shell
- ~200 bytes flash from disabling task recovery
- ~200 bytes flash from short error strings
- Reduced task array size from fewer max tasks

## Memory

Memory usage per task on AVR (with default `ARDA_MAX_NAME_LEN=16`):

| Configuration | Per Task | 16 Tasks | 8 Tasks |
|---------------|----------|----------|---------|
| Default | ~41 bytes | ~656 bytes | ~328 bytes |
| With `ARDA_NO_NAMES` | ~25 bytes | ~400 bytes | ~200 bytes |
| With `ARDA_NO_TASK_RECOVERY` | ~35 bytes | ~560 bytes | ~280 bytes |
| Both disabled | ~19 bytes | ~304 bytes | ~152 bytes |

`ARDA_TASK_RECOVERY` adds 6 bytes/task (recover pointer + timeout field).

### Where the RAM Goes

Per task on AVR (with `ARDA_TASK_RECOVERY`, which is default):
- `name[ARDA_MAX_NAME_LEN]`: 16 bytes (omitted when `ARDA_NO_NAMES` is defined)
- `setup/loop/teardown/recover` pointers: 4 × 2 bytes = 8 bytes
- `interval/lastRun/timeout`: 3 × 4 bytes = 12 bytes
- `runCount/nextFree` union: 4 bytes
- `flags`: 1 byte

**Note:** Priority uses bits 4-6 of the existing flags byte, so it adds **zero memory overhead** per task.

Global scheduler overhead (one-time):
- `tasks[ARDA_MAX_TASKS]` array (dominant cost)
- Small counters/flags (task count, active count, free list head, current task, callback depth)
- Optional callbacks (timeout/start failure/trace pointers)

### ATmega328 (Arduino Uno/Nano)

With only 2KB RAM, memory optimization is critical:
- Reduce `ARDA_MAX_TASKS` and `ARDA_MAX_NAME_LEN` if tight on RAM
- Keep task local variables small (< 50 bytes). If using `ARDA_YIELD`, `yield()` keeps your stack frame live while other tasks run
- If using `ARDA_YIELD`, avoid `yield()` in tasks with large local buffers; use global/static storage instead
- Consider `ARDA_MAX_TASKS 8` and `ARDA_MAX_NAME_LEN 12` for minimal footprint
- Define `ARDA_NO_PRIORITY` to save ~100-200 bytes of code (removes priority-scanning loop)
- Define `ARDA_NO_NAMES` to save 16 bytes per task (256 bytes for 16 tasks)

### Memory-Saving Tips

1. **Use F() for string literals** - Store strings in flash instead of RAM:
   ```cpp
   Serial.println(F("Task started"));  // Good - uses flash
   Serial.println("Task started");     // Bad - uses RAM
   ```

2. **Use smallest integer types** - `int8_t` for task IDs, `uint8_t` for small counters

3. **Reduce ARDA_MAX_TASKS** - If you only need 4 tasks, set it to 4 to save ~400 bytes

4. **Reduce ARDA_MAX_NAME_LEN** - If task names are short, reduce from 16 to save RAM per task

5. **Define ARDA_NO_NAMES** - If you don't need task names (lookup by ID only), save 16 bytes per task:
   ```cpp
   #define ARDA_NO_NAMES
   #include "Arda.h"

   // Use nameless createTask:
   int8_t id = OS.createTask(setup_fn, loop_fn, 100);
   ```

6. **Define ARDA_NO_PRIORITY** - If you don't need priority scheduling, save ~100-200 bytes of code

7. **Avoid Arduino String class** - Use `char[]` arrays instead to prevent heap fragmentation

## Dynamic Task Loading

On memory-constrained devices, you may have more logical tasks than available slots. Arda supports manually rotating tasks through slots:

```cpp
// Unload task from slot
OS.stopTask(id);      // Run teardown, transition to Stopped
OS.deleteTask(id);    // Free the slot

// Load different task into freed slot
int8_t newId = OS.createTask("other", other_setup, other_loop, 100);
```

**Your responsibilities:**
- Save any application state before `deleteTask()` (Arda doesn't preserve it)
- Restore state in the new task's `setup()` or via globals/statics
- Track which logical tasks are currently loaded

**Notes:**
- `killTask(id)` combines stop+delete in one call
- `setup()` runs on every `startTask()`, so use a flag if you need conditional initialization
- Task IDs may be reused - don't cache IDs across delete/create cycles

**Shell and slot 0:** By default, the shell occupies slot 0. If you need that extra slot:

```cpp
OS.killTask(0);  // Delete shell, freeing slot 0
int8_t id = OS.createTask("myTask", ...);  // id == 0 (reuses slot)
```

Once the shell is deleted:
- `startShell()`/`stopShell()` return false (won't accidentally affect your task in slot 0)
- `isShellRunning()` returns false
- `exec()` still works (uses class members, not the task)
- `reset()` restores the shell (clears slot 0 like all other slots)

Alternatively, define `ARDA_NO_SHELL` to have slot 0 available from the start.

---

## Appendix: yield()

> **`yield()` is disabled by default and discouraged.** State machines (see [Long Operations Need State Machines](#long-operations-need-state-machines)) are almost always better. `yield()` exists as an escape hatch for rare cases where you cannot refactor blocking code.

### Enabling yield()

```cpp
#define ARDA_YIELD
#include "Arda.h"
```

When `ARDA_YIELD` is defined:
- `yield()` becomes available
- `isTaskYielded(id)` becomes available
- `ArdaError::TaskYielded` error code exists

### Basic Usage

```cpp
TASK_LOOP(longTask) {
    for (int i = 0; i < 100; i++) {
        doSomeWork(i);
        OS.yield();  // Let other ready tasks execute
    }
}
```

### Why yield() is Dangerous

`yield()` runs other tasks inside your stack frame. Unlike preemptive multitasking, Arda does not give each task its own stack. When you call `yield()`, other tasks execute as nested function calls. Your local variables remain on the stack the entire time. This can silently exhaust stack memory on constrained MCUs:

```cpp
// DANGEROUS on ATmega328 (2KB RAM):
TASK_LOOP(badTask) {
    uint8_t buffer[512];      // 512 bytes on stack
    processData(buffer);
    OS.yield();               // Other tasks run WITH buffer still on stack
}                             // If another task also has large locals → stack overflow
```

**Safe patterns:**
- Keep task local variables small (< 50 bytes)
- Use global/static storage for large buffers
- Avoid `yield()` in tasks with large stack frames

### yield() Semantics

- `yield()` only runs tasks whose interval has elapsed
- Each task runs at most once per scheduler cycle (prevents infinite loops if task A yields to task B which yields back to A)
- `yield()` then returns control to your task to continue execution
- Tasks cannot be deleted while they have an active `yield()` call
- `ARDA_MAX_CALLBACK_DEPTH` (default 8) limits nesting depth, but does not account for stack frame size

### yield() in setup/teardown

Calling `yield()` from `setup()` or `teardown()` callbacks is supported but may produce surprising behavior: other tasks will execute in the middle of your task's initialization or cleanup. Generally avoid this unless you have a specific reason.

## Appendix: Task Recovery (Soft Watchdog, AVR)

On supported AVR platforms, Arda provides a "soft watchdog" that can forcibly abort stuck tasks without resetting the entire MCU. This feature is **enabled by default** on AVR boards with Timer2 support (Uno, Mega, Nano, Pro Mini, etc.).

### How It Works

- Uses Timer2 and `setjmp`/`longjmp` to abort tasks that exceed their timeout
- When a task's `loop()` exceeds its timeout, execution jumps back to the scheduler
- The optional `recover` callback runs to clean up partial state
- The task remains in Running state - it will automatically retry `loop()` on the next scheduler cycle
- To permanently stop a misbehaving task, call `stopTask()` from within `recover()`
- Other tasks continue running normally - no MCU reset required

### Basic Usage

```cpp
#include "Arda.h"

void sensor_recover() {
    // Called after forced abort - clean up partial state
    resetSensorState();
}

void setup() {
    // Create task with timeout and recovery in one call:
    // name, setup, loop, interval, teardown, autoStart, priority, timeout, recover
    int8_t id = OS.createTask("sensor", sensor_setup, sensor_loop,
                              100, nullptr, true, TaskPriority::Normal,
                              50, sensor_recover);  // 50ms timeout
    OS.begin();
}
```

You can also set timeout and recover separately after task creation:

```cpp
void setup() {
    int8_t id = OS.createTask("sensor", sensor_setup, sensor_loop, 100);
    OS.setTaskTimeout(id, 50);              // Abort if loop() takes > 50ms
    OS.setTaskRecover(id, sensor_recover);  // Optional cleanup callback
    OS.begin();
}
```

### Stopping Misbehaving Tasks

The recovery callback can take action beyond just cleanup - for example, stopping a task that keeps timing out:

```cpp
int8_t sensorTaskId = -1;
int abortCount = 0;

void sensor_recover() {
    abortCount++;
    resetSensorState();

    // After 3 aborts, assume hardware is broken - stop the task
    if (abortCount >= 3) {
        OS.stopTask(sensorTaskId);
        logError("Sensor task disabled after repeated timeouts");
    }
}

void setup() {
    sensorTaskId = OS.createTask("sensor", sensor_setup, sensor_loop,
                                 100, nullptr, true, TaskPriority::Normal,
                                 50, sensor_recover);  // 50ms timeout
    OS.begin();
}
```

This pattern is useful for tasks that interact with external hardware that may become unresponsive.

### Adjusting Timeout Mid-Loop

A task can adjust its own timeout during execution. This is useful for known-slow operations that shouldn't trigger recovery:

```cpp
void someTask_loop() {
    doQuickWork();  // Normal timeout applies

    // Extend timeout for a known-slow operation
    uint32_t oldTimeout = OS.getTaskTimeout(myTaskId);
    OS.setTaskTimeout(myTaskId, 500);  // Allow 500ms for this part

    performSlowOperation();  // Won't be aborted

    OS.setTaskTimeout(myTaskId, oldTimeout);  // Restore original

    doMoreQuickWork();  // Original timeout applies again
}
```

Setting timeout to 0 disables recovery for the remainder of the current loop() call.

### Runtime Enable/Disable

Task recovery can also be disabled globally for known-slow operations:

```cpp
void someTask_loop() {
    // Temporarily disable for a known-slow operation
    bool wasEnabled = OS.isTaskRecoveryEnabled();
    OS.setTaskRecoveryEnabled(false);

    performSlowOperation();  // Won't trigger recovery

    OS.setTaskRecoveryEnabled(wasEnabled);
}
```

Or disabled during initialization:

```cpp
void setup() {
    OS.setTaskRecoveryEnabled(false);  // Disable during init
    // ... create tasks, configure hardware ...
    OS.begin();
    OS.setTaskRecoveryEnabled(true);   // Re-enable after system is stable
}
```

### Disabling at Compile Time

To disable task recovery entirely (saves ~200 bytes of code):

```cpp
#define ARDA_NO_TASK_RECOVERY
#include "Arda.h"
```

### Platform Support

Task recovery is enabled by default on all platforms. The timeout API (`setTaskTimeout`, `getTaskTimeout`, timeout callbacks) works everywhere. Hardware abort (forcibly stopping stuck tasks via Timer2) is only available on AVR with Timer2 (Uno, Mega, Nano, Pro Mini, etc.). Use `isTaskRecoveryAvailable()` to check if hardware abort is supported.

### Mutual Exclusion (Hardware Abort Only)

The hardware abort feature (AVR with Timer2) cannot be used with:
- `ARDA_YIELD` - yield corrupts the jump context used by setjmp/longjmp
- `ARDA_NO_GLOBAL_INSTANCE` - Timer2 ISR requires the global `OS` instance

These restrictions do not apply to the soft timeout API (`setTaskTimeout`, `getTaskTimeout`, timeout callbacks), which works on all platforms regardless of these defines.

`ARDA_WATCHDOG` can be used alongside task recovery as a backup. Task recovery aborts individual stuck tasks; if that fails, the watchdog resets the MCU.

### PWM Conflict (Arduino Uno)

On ATmega328P boards (Uno, Nano, Pro Mini), Timer2 also controls PWM output on pins 3 and 11. When hardware abort is active, `analogWrite()` on these pins will not work correctly. PWM on pins 5, 6, 9, and 10 is unaffected.

To restore PWM on pins 3 and 11 at runtime, call `OS.setTaskRecoveryEnabled(false)`. This disables the Timer2 interrupt, allowing normal PWM operation on those pins (at the cost of losing hardware abort protection).

## Appendix: Hardware Watchdog (AVR)

On AVR platforms (Uno, Mega, Nano, Pro Mini, etc.), Arda can optionally enable the hardware watchdog timer with an 8-second timeout. If any task blocks for more than 8 seconds without returning, the MCU resets automatically, recovering the system.

### Enabling the Watchdog

```cpp
#define ARDA_WATCHDOG
#include "Arda.h"
```

### How It Works

- Watchdog is enabled when `begin()` is called
- Timer is reset at the start of each `run()` cycle (so an idle scheduler with no runnable tasks won't trigger a reset)
- Timer is also reset before each task's `loop()` executes and during `yield()` calls
- If a task blocks (infinite loop, deadlock, etc.) for >8 seconds, the watchdog expires and resets the MCU
- After reset, `setup()` runs again and the scheduler restarts cleanly

### Best Practices

For long operations, break them into chunks that return quickly, allowing the scheduler to feed the watchdog between task executions:

```cpp
// State machine approach (recommended, no ARDA_YIELD needed):
static int workIndex = 0;
void longTask_loop() {
    for (int i = 0; i < 100; i++) {
        doWork(workIndex++);
    }
    if (workIndex >= 10000) workIndex = 0;
    // Returns quickly - watchdog fed between run() cycles
}
```

If refactoring is not possible and you must use `yield()` (see [Appendix: yield()](#appendix-yield)):

```cpp
#define ARDA_YIELD
#include "Arda.h"

void longTask_loop() {
    for (int i = 0; i < 10000; i++) {
        doWork();
        if (i % 100 == 0) OS.yield();  // Feed watchdog, let other tasks run
    }
}
```

### Platform Support

Hardware watchdog is AVR-only. On non-AVR platforms, this feature is not available.

## License

Arda is released under the MIT License. See [LICENSE](LICENSE) for details.
