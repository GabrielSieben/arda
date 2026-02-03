# Arda

A cooperative multitasking scheduler for Arduino that allows multiple tasks to run "simultaneously".
Designed with AVR-class boards (e.g., ATmega328, 2KB RAM) in mind, with a focus on memory efficiency.

## Features

- Cooperative multitasking with configurable intervals
- Task lifecycle management (start, pause, resume, stop)
- Dynamic interval adjustment at runtime
- Task lookup by name
- Lightweight - minimal memory footprint
- Works on all Arduino boards

## Quick Start

```cpp
#include "Arda.h"

// Define a task using macros
TASK_SETUP(blinker) {
    pinMode(13, OUTPUT);
}

TASK_LOOP(blinker) {
    digitalWrite(13, !digitalRead(13));
}

void setup() {
    REGISTER_TASK_ID(blinkerId, blinker, 500);  // Run every 500ms
    if (blinkerId == -1) {
        // Handle error - task creation failed
        while (1) { }
    }
    OS.begin();
}

void loop() {
    OS.run();
}
```

## API Reference

### Task Management

| Method | Description |
|--------|-------------|
| `createTask(name, setup, loop, interval, teardown, autoStart)` | Register a new task. Returns task ID (-1 on failure). Name must be non-empty, max 7 chars, and is **case-sensitive**. If `begin()` was called and `autoStart` is true (default), task auto-starts. Set `autoStart=false` to create in STOPPED state. Note: `begin(false)` only affects tasks existing at `begin()` time; tasks created after use their own `autoStart` parameter. When `ARDA_NO_NAMES` is defined, name is ignored; use the nameless `createTask(setup, loop, interval, teardown, autoStart)` overload instead. |
| `deleteTask(id)` | Delete a stopped task, freeing its slot for reuse. Cannot delete currently executing task. |
| `startTask(id, runImmediately)` | Start a stopped task (runs setup callback). Set `runImmediately=true` to run on next cycle instead of waiting one interval. Note: `runImmediately` has no effect for zero-interval tasks. Resets `runCount` to 0. |
| `pauseTask(id)` | Pause a running task |
| `resumeTask(id)` | Resume a paused task |
| `stopTask(id)` | Stop a task (runs teardown if provided). Returns `StopResult` enum - see below. |
| `setTaskTimeout(id, ms)` | Set max execution time for a task (0 = disabled). If exceeded, timeout callback is called. |
| `getTaskTimeout(id)` | Get a task's timeout setting (0 if disabled or invalid task) |
| `setTaskPriority(id, priority)` | Set task priority (0-15, higher = runs first). Values > 15 are clamped. Not available if `ARDA_NO_PRIORITY` is defined. |
| `getTaskPriority(id)` | Get task priority (0-15). Returns 0 for invalid tasks. Not available if `ARDA_NO_PRIORITY` is defined. |
| `setTaskInterval(id, ms)` | Change a task's execution interval at runtime |
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

### Scheduler

| Method | Description |
|--------|-------------|
| `begin()` | Initialize and start all registered tasks. Returns number successfully started, or -1 if already begun. |
| `run()` | Execute the scheduler (call in loop()). Returns false with `WrongState` error if `begin()` not called. |
| `reset()` | Stop all tasks and reset scheduler to initial state. Returns true if all teardowns ran, false if any were skipped. |
| `yield()` | Give other tasks a chance to run (see Cooperative Multitasking below) |
| `uptime()` | Milliseconds since begin(), or raw millis() if begin() not yet called |
| `hasBegun()` | Returns true if begin() has been called |
| `setTimeoutCallback(cb)` | Set callback invoked when a task exceeds its timeout |
| `setStartFailureCallback(cb)` | Set callback invoked for each task that fails to start during `begin()` |
| `setTraceCallback(cb)` | Set callback for debugging/tracing task execution (nullptr to disable) |

### Task Info

| Method | Description |
|--------|-------------|
| `getTaskCount()` | Number of active (non-deleted) tasks |
| `getActiveTaskCount()` | Alias for getTaskCount() |
| `getSlotCount()` | Total task slots used (includes deleted) - for iteration |
| `getMaxTasks()` | Maximum task capacity (compile-time constant) |
| `getTaskName(id)` | Get task name (nullptr if deleted/invalid). Always returns nullptr when `ARDA_NO_NAMES` is defined. |
| `getTaskState(id)` | Get task state (Running/Paused/Stopped/Invalid) |
| `getTaskRunCount(id)` | Execution count (returns 0 if invalid - use `isValidTask()` first) |
| `getTaskInterval(id)` | Interval in ms (returns 0 if invalid - use `isValidTask()` first) |
| `getCurrentTask()` | ID of currently executing task (-1 if none) |
| `isValidTask(id)` | Returns true if task ID refers to a valid, non-deleted task |
| `isTaskYielded(id)` | Returns true if task is currently inside a yield() call |
| `getValidTaskIds(outIds, maxCount)` | Fill array with valid task IDs. Returns total count of valid tasks (may exceed maxCount). Pass nullptr to just get the count. |
| `hasTaskSetup(id)` | Returns true if task has a setup callback configured |
| `hasTaskLoop(id)` | Returns true if task has a loop callback configured |
| `hasTaskTeardown(id)` | Returns true if task has a teardown callback configured |

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

## Task States

| State | Description |
|-------|-------------|
| `TaskState::Stopped` | Task is not running (can be started or deleted) |
| `TaskState::Running` | Task is active and executing on each scheduler cycle |
| `TaskState::Paused` | Task is temporarily suspended (can be resumed) |
| `TaskState::Invalid` | Returned by getTaskState() for invalid or deleted task IDs |

## Error Codes

Use `getError()` after a failed operation to get detailed error information. Successful operations clear the error state. Use `Arda::errorString(error)` to convert an error code to a human-readable string for debugging.

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
| `ArdaError::TaskYielded` | Cannot delete task that has yielded |
| `ArdaError::AlreadyBegun` | begin() was already called |
| `ArdaError::CallbackDepth` | Maximum callback nesting depth exceeded |
| `ArdaError::StateChanged` | Callback modified task state unexpectedly |
| `ArdaError::InCallback` | Cannot call reset() from within a callback |
| `ArdaError::NotSupported` | Feature disabled at compile time (e.g., `renameTask` when `ARDA_NO_NAMES` is defined) |

## Macros

### Basic Macros

```cpp
TASK_SETUP(name)      // Define setup function: void name_setup()
TASK_LOOP(name)       // Define loop function: void name_loop()
TASK_TEARDOWN(name)   // Define teardown function: void name_teardown()
REGISTER_TASK(name, interval)  // Register task (discards returned ID)
REGISTER_TASK_WITH_TEARDOWN(name, interval)
```

### Macros with ID Capture

If you need to control tasks later, use these variants to capture the task ID:

```cpp
REGISTER_TASK_ID(myTaskId, blinker, 500);  // int8_t myTaskId = ...
OS.pauseTask(myTaskId);  // Now you can control it
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
```

## Configuration

### Compile-Time Constants

Define these before including `Arda.h`, or edit the header directly:

```cpp
#define ARDA_MAX_TASKS 8           // Maximum number of tasks (default: 16, max: 127)
#define ARDA_MAX_NAME_LEN 12       // Task name buffer size (default: 8, usable: 7 chars)
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

**Note:** Case-insensitive matching only handles ASCII letters (A-Z, a-z). Extended ASCII and UTF-8 characters are compared as-is.

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

### Combining Configuration Options

For maximum memory savings on extremely constrained devices (e.g., ATtiny), combine multiple options:

```cpp
#define ARDA_MAX_TASKS 4
#define ARDA_NO_NAMES
#define ARDA_NO_PRIORITY
#include "Arda.h"
```

This configuration saves:
- 8 bytes per task from disabling names (default `ARDA_MAX_NAME_LEN`)
- ~100-200 bytes of code from disabling priority
- Reduced task array size from fewer max tasks

## Priority Scheduling

By default, Arda supports 16-level task priority (0-15). Higher-priority tasks run before lower-priority tasks when both are ready in the same scheduling cycle.

### TaskPriority Enum

For readability, use the `TaskPriority` enum:

| Level | Value | Description |
|-------|-------|-------------|
| `TaskPriority::Lowest` | 0 | Background tasks |
| `TaskPriority::Low` | 4 | Below normal priority |
| `TaskPriority::Normal` | 8 | Default priority (assigned to all new tasks) |
| `TaskPriority::High` | 12 | Above normal priority |
| `TaskPriority::Highest` | 15 | Critical tasks |

You can also use raw values 0-15 directly.

### Example Usage

```cpp
// Create tasks with explicit priority
int8_t criticalId = OS.createTask("critical", setup, loop, 100, 15);  // Highest
int8_t normalId = OS.createTask("normal", setup, loop, 100);          // Default (8)
int8_t bgId = OS.createTask("background", setup, loop, 100, 0);       // Lowest

// Change priority at runtime
OS.setTaskPriority(normalId, static_cast<uint8_t>(TaskPriority::High));

// Query priority
uint8_t priority = OS.getTaskPriority(criticalId);  // Returns 15
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

### Using yield()

Call `yield()` during long operations to let other tasks run:

```cpp
TASK_LOOP(longTask) {
    for (int i = 0; i < 100; i++) {
        doSomeWork(i);
        OS.yield();  // Let other ready tasks execute
    }
}
```

**Important `yield()` semantics:**

- `yield()` runs other ready tasks **immediately in the calling context** - your task's stack frame remains on the stack while other tasks execute. This means:
  - Stack usage grows with each nested yield
  - Local variables in your task are preserved across yield
  - Other tasks run as function calls, not as separate threads
- `yield()` only runs tasks whose interval has elapsed
- Each task runs at most once per scheduler cycle (prevents infinite loops if task A yields to task B which yields back to A)
- `yield()` then returns control to your task to continue execution
- Tasks cannot be deleted while they have an active `yield()` call

**Stack exhaustion warning:**

While Arda limits callback nesting depth (`ARDA_MAX_CALLBACK_DEPTH`, default 8), this doesn't account for stack frame size. Tasks with large local variables (arrays, structs) that call `yield()` can exhaust stack space on memory-constrained boards like ATmega328 (2KB RAM). Keep task local variables small, or use global/static storage for large buffers.

**yield() in setup/teardown:**

Calling `yield()` from `setup()` or `teardown()` callbacks is supported but may produce surprising behavior - other tasks will execute in the middle of your task's initialization or cleanup. Generally avoid this unless you have a specific reason.

## Timing Behavior

### Interval Scheduling

Tasks with intervals use cadence-based timing. If a task is scheduled for 100ms and actually runs at 105ms, the next run is scheduled for 200ms (not 205ms). This prevents timing drift.

### Catch-up Prevention

If the scheduler falls significantly behind (e.g., due to a long-running task), it resets timing rather than running multiple catch-up iterations.

**Example:** A task with 100ms interval last ran at t=100. If the next `run()` call happens at t=350 (missed 2 intervals), the task runs once and `lastRun` is reset to t=350. The next execution will be at t=450, not t=200/t=300/t=400.

**Implication:** If you need strict periodicity or need to know how many intervals were missed, implement your own timing logic using `millis()` inside the task.

### millis() Overflow

Arduino's `millis()` overflows after ~49 days. Arda handles this correctly - interval calculations and `uptime()` continue to work due to unsigned arithmetic properties.

### Run Count Overflow

`getTaskRunCount()` returns a `uint32_t` that increments on each `loop()` execution. **Note:** This counter resets to 0 on each `startTask()` call, so it tracks runs since the most recent start, not lifetime executions. At 1ms intervals, this overflows after ~49 days. If you need cumulative execution counts across restarts, maintain your own counter.

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

### Debug/Trace Callback

Monitor task execution for debugging:

```cpp
void onTrace(int8_t taskId, TraceEvent event) {
    const char* eventNames[] = {
        "STARTING", "STARTED", "LOOP_BEGIN", "LOOP_END",
        "STOPPING", "STOPPED", "PAUSED", "RESUMED", "DELETED"
    };
    Serial.print(OS.getTaskName(taskId));
    Serial.print(F(": "));
    Serial.println(eventNames[static_cast<uint8_t>(event)]);
}

void setup() {
    OS.setTraceCallback(onTrace);
    // ... create tasks ...
    OS.begin();
}
```

Trace events:
- `TraceEvent::TaskStarting` - Task setup() about to run
- `TraceEvent::TaskStarted` - Task is now Running
- `TraceEvent::TaskLoopBegin` - Task loop() about to run
- `TraceEvent::TaskLoopEnd` - Task loop() completed
- `TraceEvent::TaskStopping` - Task teardown() about to run (only emitted if teardown exists)
- `TraceEvent::TaskStopped` - Task is now Stopped
- `TraceEvent::TaskPaused` - Task was paused
- `TraceEvent::TaskResumed` - Task was resumed
- `TraceEvent::TaskDeleted` - Task was deleted (fired before cleanup, so task info still available)

Set callback to `nullptr` to disable tracing (recommended for production).

**Note:** When `ARDA_NO_NAMES` is defined, `getTaskName(taskId)` returns nullptr. Use the numeric task ID for identification in trace output instead.

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

## Memory

Memory usage per task (with default `ARDA_MAX_NAME_LEN=8`):

| Platform | With Names | With `ARDA_NO_NAMES` | 16 Tasks (default) | 16 Tasks (no names) |
|----------|------------|----------------------|--------------------|--------------------|
| **AVR (ATmega328)** | ~38 bytes | ~30 bytes | ~608 bytes | ~480 bytes |
| **ESP8266/ESP32** | ~44 bytes | ~36 bytes | ~704 bytes | ~576 bytes |
| **ARM Cortex-M** | ~44 bytes | ~36 bytes | ~704 bytes | ~576 bytes |
| **64-bit (testing)** | ~56 bytes | ~48 bytes | ~896 bytes | ~768 bytes |

Defining `ARDA_NO_NAMES` saves `ARDA_MAX_NAME_LEN` bytes per task (default 8 bytes).

### Where the RAM Goes

Per task (approximate; padding varies by architecture):
- `name[ARDA_MAX_NAME_LEN]`: `ARDA_MAX_NAME_LEN` bytes (omitted when `ARDA_NO_NAMES` is defined)
- `setup/loop/teardown` pointers: 3 * pointer size (2 bytes on AVR, 4 bytes on 32-bit)
- `interval/lastRun/timeout`: 3 * 4 bytes
- `runCount/nextFree` union: 4 bytes
- `flags`: 1 byte (bits 0-1: state, bit 2: ranThisCycle, bit 3: inYield, bits 4-7: priority). When `ARDA_NO_NAMES` is defined, state value 3 indicates a deleted slot.

**Note:** Priority uses bits 4-7 of the existing flags byte, so it adds **zero memory overhead** per task.

Global scheduler overhead (one-time):
- `tasks[ARDA_MAX_TASKS]` array (dominant cost)
- Small counters/flags (task count, active count, free list head, current task, callback depth)
- Optional callbacks (timeout/start failure/trace pointers)

### Platform-Specific Notes

**ESP8266/ESP32:**
- Much more RAM available, so default settings are fine
- Consider increasing `ARDA_MAX_TASKS` if needed
- Watch for stack overflow with deeply nested yields (larger stack frames)
- WiFi callbacks may interfere - use volatile flags for ISR communication

**ATmega328 (Arduino Uno/Nano):**
- Only 2KB RAM - reduce `ARDA_MAX_TASKS` and `ARDA_MAX_NAME_LEN` if tight
- Keep task local variables small
- Consider `ARDA_MAX_TASKS 8` and `ARDA_MAX_NAME_LEN 12` for minimal footprint
- Define `ARDA_NO_PRIORITY` to save ~100-200 bytes of code (removes priority-scanning loop)
- Define `ARDA_NO_NAMES` to save 8 bytes per task (128 bytes for 16 tasks)

### Memory-Saving Tips

1. **Use F() for string literals** - Store strings in flash instead of RAM:
   ```cpp
   Serial.println(F("Task started"));  // Good - uses flash
   Serial.println("Task started");     // Bad - uses RAM
   ```

2. **Use smallest integer types** - `int8_t` for task IDs, `uint8_t` for small counters

3. **Reduce ARDA_MAX_TASKS** - If you only need 4 tasks, set it to 4 to save ~400 bytes

4. **Reduce ARDA_MAX_NAME_LEN** - If task names are short, reduce from 16 to save RAM per task

5. **Define ARDA_NO_NAMES** - If you don't need task names (lookup by ID only), save 8 bytes per task:
   ```cpp
   #define ARDA_NO_NAMES
   #include "Arda.h"

   // Use nameless createTask:
   int8_t id = OS.createTask(setup_fn, loop_fn, 100);
   ```

6. **Define ARDA_NO_PRIORITY** - If you don't need priority scheduling, save ~100-200 bytes of code

7. **Avoid Arduino String class** - Use `char[]` arrays instead to prevent heap fragmentation

## License

Arda is released under the MIT License. See [LICENSE](LICENSE) for details.
