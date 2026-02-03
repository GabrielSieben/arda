/*
 * Arda - A cooperative multitasking scheduler for Arduino
 *
 * WARNING: NOT INTERRUPT-SAFE. Do not call Arda methods from interrupt handlers
 * (ISRs). If you must interact with Arda from an interrupt, set a volatile flag
 * and check it in your main loop instead.
 */

#pragma once

#include <string.h>
#include <Arduino.h>

// Version info
#define ARDA_VERSION_MAJOR 1
#define ARDA_VERSION_MINOR 0
#define ARDA_VERSION_PATCH 0
#define ARDA_VERSION_STRING "1.0.0"

// Configuration constants - adjust these to tune memory usage vs capability
#ifndef ARDA_MAX_TASKS
#define ARDA_MAX_TASKS 16          // Maximum concurrent tasks (1-127). Each task uses ~46-64 bytes depending on platform.
#endif
#ifndef ARDA_MAX_NAME_LEN
#define ARDA_MAX_NAME_LEN 8        // Task name buffer size. Max usable chars = 7 (for null terminator).
#endif
#ifndef ARDA_MAX_CALLBACK_DEPTH
// Default 8: balances safety vs flexibility. Each nesting level uses ~20-50 bytes
// of stack for return addresses and saved registers, plus the callback's local
// variables. On ATmega328 (2KB RAM), 8 levels is safe for typical callbacks.
// Reduce to 4-6 for memory-constrained boards or callbacks with large locals.
#define ARDA_MAX_CALLBACK_DEPTH 8
#endif

// Optional features - define before including Arda.h to enable
// #define ARDA_CASE_INSENSITIVE_NAMES  // Make findTaskByName case-insensitive
// #define ARDA_NO_GLOBAL_INSTANCE      // Don't create global 'OS' instance (create your own)
// #define ARDA_NO_PRIORITY             // Disable priority scheduling (saves code size, uses array-order execution)
// #define ARDA_NO_NAMES                // Disable task names (saves ARDA_MAX_NAME_LEN bytes per task)

typedef void (*TaskCallback)(void);

// Use uint8_t underlying type to save memory (1 byte instead of 4)
enum class TaskState : uint8_t {
    Stopped,
    Running,
    Paused,
    Invalid  // Returned for invalid/deleted task queries
};

// Error codes for detailed error reporting (check with getError())
// Note: Query methods (getters, findTaskByName) do NOT set error - they return
// sentinel values (nullptr, TaskState::Invalid, -1, 0) to indicate no result.
// Only mutation methods set error on failure.
enum class ArdaError : uint8_t {
    Ok = 0,              // No error
    NullName,            // Task name was null
    EmptyName,           // Task name was empty string
    NameTooLong,         // Task name exceeds ARDA_MAX_NAME_LEN-1
    DuplicateName,       // Task with this name already exists
    MaxTasks,            // Maximum task limit reached
    InvalidId,           // Task ID is out of range or deleted
    WrongState,          // Task is in wrong state for operation
    TaskExecuting,       // Cannot delete currently executing task
    TaskYielded,         // Cannot delete task that has yielded
    AlreadyBegun,        // begin() was already called
    CallbackDepth,       // Maximum callback nesting depth exceeded
    StateChanged,        // Callback modified task state unexpectedly
    InCallback,          // Cannot call reset() from within a callback
    NotSupported         // Feature disabled at compile time (e.g., names when ARDA_NO_NAMES)
};

// Result codes for stopTask() - disambiguates success from partial success
enum class StopResult : uint8_t {
    Success,              // Task stopped and teardown ran successfully (or no teardown defined)
    TeardownSkipped,      // Task stopped but teardown was NOT run (callback depth exceeded)
    TeardownChangedState, // Task stopped, teardown ran but modified task state unexpectedly
    Failed                // Task couldn't be stopped - check getError() for reason
};

// Callback types for event notifications
typedef void (*TimeoutCallback)(int8_t taskId, uint32_t actualDurationMs);
typedef void (*StartFailureCallback)(int8_t taskId, ArdaError error);

// Debug/trace events for monitoring task lifecycle (9 events)
enum class TraceEvent : uint8_t {
    TaskStarting,   // Task setup() about to run
    TaskStarted,    // Task setup() completed, now Running
    TaskLoopBegin,  // Task loop() about to run
    TaskLoopEnd,    // Task loop() completed
    TaskStopping,   // Task teardown() about to run
    TaskStopped,    // Task is now Stopped
    TaskPaused,     // Task was paused
    TaskResumed,    // Task was resumed
    TaskDeleted     // Task was deleted
};
typedef void (*TraceCallback)(int8_t taskId, TraceEvent event);

#ifndef ARDA_NO_PRIORITY
// Named priority levels for readability (can also use raw 0-15 values)
enum class TaskPriority : uint8_t {
    Lowest  = 0,
    Low     = 4,
    Normal  = 8,   // Default
    High    = 12,
    Highest = 15
};
#endif

// Task flags bit positions (packed into single byte for RAM efficiency)
#define ARDA_TASK_STATE_MASK   0x03  // bits 0-1: TaskState (0=Stopped, 1=Running, 2=Paused)
#define ARDA_TASK_RAN_BIT      0x04  // bit 2: ranThisCycle
#define ARDA_TASK_YIELD_BIT    0x08  // bit 3: inYield
#ifndef ARDA_NO_PRIORITY
#define ARDA_TASK_PRIORITY_MASK  0xF0  // bits 4-7: priority (0-15, higher = runs first)
#define ARDA_TASK_PRIORITY_SHIFT 4
#define ARDA_DEFAULT_PRIORITY    8     // Middle priority (TaskPriority::Normal)
#endif

#ifdef ARDA_NO_NAMES
// When names are disabled, use state value 3 (unused) as deletion marker.
// This avoids conflict with priority bits (4-7) which would cause false positives.
#define ARDA_TASK_DELETED_STATE  0x03  // state bits = 3: task deleted (only when ARDA_NO_NAMES)
#endif

// Task structure - fields ordered to minimize padding.
// Memory per task (with default ARDA_MAX_NAME_LEN=8):
//   AVR (8-bit):  8 + 3*2 + 4*4 + 1 = ~31 bytes (23 bytes with ARDA_NO_NAMES)
//   ESP8266/32:   8 + 3*4 + 4*4 + 1 = ~37 bytes (29 bytes with ARDA_NO_NAMES)
//   32-bit ARM:   8 + 3*4 + 4*4 + 1 = ~37 bytes (29 bytes with ARDA_NO_NAMES)
//   64-bit:       8 + 3*8 + 4*4 + 1 + padding = ~49 bytes (41 bytes with ARDA_NO_NAMES)
// Total for 16 tasks: ~496 bytes (AVR), ~592 bytes (32-bit), ~784 bytes (64-bit)
struct Task {
#ifndef ARDA_NO_NAMES
    char name[ARDA_MAX_NAME_LEN]; // Copied, safe from dangling pointers; empty = deleted
#endif
    TaskCallback setup;           // One-time initialization callback
    TaskCallback loop;            // Repeated execution callback
    TaskCallback teardown;        // Cleanup callback (called on stop)
    uint32_t interval;            // Run interval in ms (0 = every cycle)
    uint32_t lastRun;             // Last execution time (for interval scheduling)
    uint32_t timeout;             // Max execution time in ms (0 = disabled)
    // INVARIANT: This union shares memory between active and deleted task states.
    // - runCount: ONLY valid when task is not deleted. Read via getTaskRunCount().
    // - nextFree: ONLY valid when task is deleted. Used internally for free list.
// Deletion is indicated by name[0]=='\0' (normal) or ARDA_TASK_DELETED_STATE (ARDA_NO_NAMES).
    // Accessing the wrong member yields garbage. Always check isValidTask() before using runCount.
    union {
        uint32_t runCount;        // Execution count (overflows after ~49 days at 1ms)
        int8_t nextFree;          // Next free slot index (-1 = end of list); internal use only
    };
    // Packed flags: bits 0-1 = state, bit 2 = ranThisCycle, bit 3 = inYield
    // When ARDA_NO_NAMES: state value 3 = deleted (ARDA_TASK_DELETED_STATE)
    // Bits 4-7: priority (when ARDA_NO_PRIORITY is not defined)
    uint8_t flags;
};

// Validate ARDA_MAX_TASKS range (must fit in int8_t and be at least 1)
#if ARDA_MAX_TASKS < 1
#error "ARDA_MAX_TASKS must be at least 1"
#endif
#if ARDA_MAX_TASKS > 127
#error "ARDA_MAX_TASKS cannot exceed 127 due to int8_t task IDs"
#endif

// Validate ARDA_MAX_NAME_LEN (must allow at least 1 char + null terminator)
#ifndef ARDA_NO_NAMES
#if ARDA_MAX_NAME_LEN < 2
#error "ARDA_MAX_NAME_LEN must be at least 2 (1 char + null terminator)"
#endif
#endif

class Arda {
public:
    // -------------------------------------------------------------------------
    // Constructor
    // -------------------------------------------------------------------------
    Arda();

    // Non-copyable and non-movable: copying/moving would create two schedulers
    // with duplicate task data, which is almost certainly not what you want.
    Arda(const Arda&) = delete;
    Arda& operator=(const Arda&) = delete;
    Arda(Arda&&) = delete;
    Arda& operator=(Arda&&) = delete;

    // -------------------------------------------------------------------------
    // Scheduler control
    // -------------------------------------------------------------------------

    // Returns number of tasks started, or -1 if already begun (check hasBegun() first).
    // Set autoStartTasks=false to initialize scheduler without starting registered tasks.
    // If startFailureCallback is set, it will be called for each task that fails to start.
    int8_t begin(bool autoStartTasks = true);

    // Execute the scheduler. Returns false and sets error if begin() hasn't been called.
    bool run();

    // Stop all tasks and reset scheduler to initial state.
    // By default, clears user callbacks (timeout, startFailure, trace) for a clean slate.
    // Set preserveCallbacks=true to keep callbacks registered across reset.
    // Returns true if all teardowns ran successfully, false if any were skipped.
    // Check getError() for ArdaError::CallbackDepth if false is returned.
    bool reset(bool preserveCallbacks = false);

    bool hasBegun() const;  // Returns true if begin() has been called

    // -------------------------------------------------------------------------
    // Task creation and deletion
    // -------------------------------------------------------------------------

#ifdef ARDA_NO_NAMES
    // Simplified createTask without name parameter (ARDA_NO_NAMES mode).
    // Returns -1 if max tasks reached or auto-start fails.
    int8_t createTask(TaskCallback setup, TaskCallback loop,
                      uint32_t intervalMs = 0, TaskCallback teardown = nullptr,
                      bool autoStart = true);

    // Backward compatibility: accepts name parameter but ignores it.
    // Allows existing code to compile without changes.
    int8_t createTask(const char* name, TaskCallback setup, TaskCallback loop,
                      uint32_t intervalMs = 0, TaskCallback teardown = nullptr,
                      bool autoStart = true);
#else
    // Name is copied, so the original string does not need to remain valid.
    // Name comparison is case-sensitive ("MyTask" != "mytask").
    // Returns -1 if: name is null/empty, name exceeds ARDA_MAX_NAME_LEN-1 chars,
    // name is duplicate, max tasks reached, or auto-start fails.
    // If begin() was already called and autoStart is true (default), the new task
    // is automatically started; on start failure, the task is NOT created (-1 returned).
    // Set autoStart=false to create a task in STOPPED state after begin().
    // NOTE: begin(autoStartTasks=false) only affects tasks existing at begin() time.
    // Tasks created after begin() follow their own autoStart parameter (default true).
    int8_t createTask(const char* name, TaskCallback setup, TaskCallback loop,
                      uint32_t intervalMs = 0, TaskCallback teardown = nullptr,
                      bool autoStart = true);
#endif

#ifndef ARDA_NO_PRIORITY
    // Create a task with explicit priority (0-15, higher runs first).
    // See TaskPriority enum for named levels, or use raw values.
    int8_t createTask(const char* name, TaskCallback setup, TaskCallback loop,
                      uint32_t intervalMs, uint8_t priority,
                      TaskCallback teardown = nullptr, bool autoStart = true);
#endif

    bool deleteTask(int8_t taskId);

    // -------------------------------------------------------------------------
    // Task state control
    // -------------------------------------------------------------------------

    // Start a stopped task. By default, interval-based tasks wait one full interval
    // before their first execution. Set runImmediately=true to execute the first
    // loop() call right away (on the next run() cycle), then resume normal interval timing.
    // Note: runImmediately has no effect for zero-interval tasks (they run every cycle anyway).
    // NOTE: runCount is reset to 0 on each start. It tracks loop executions since the
    // most recent start, not cumulative runs across the task's lifetime.
    bool startTask(int8_t taskId, bool runImmediately = false);

    bool pauseTask(int8_t taskId);
    bool resumeTask(int8_t taskId);

    // Stop a running or paused task. Returns StopResult enum:
    //   StopResult::Success: Task stopped and teardown ran successfully (or no teardown defined)
    //   StopResult::TeardownSkipped: Task stopped but teardown NOT run (check getError() for CallbackDepth)
    //   StopResult::TeardownChangedState: Task stopped, teardown ran but modified state (getError() = StateChanged)
    //   StopResult::Failed: Task couldn't be stopped (check getError() for WrongState, InvalidId)
    StopResult stopTask(int8_t taskId);

    // -------------------------------------------------------------------------
    // Task configuration
    // -------------------------------------------------------------------------

    // Change task interval. By default, resets lastRun to now (task waits full new interval).
    // Set resetTiming=false to keep existing timing (next run based on previous lastRun + new interval).
    bool setTaskInterval(int8_t taskId, uint32_t intervalMs, bool resetTiming = true);

    bool setTaskTimeout(int8_t taskId, uint32_t timeoutMs);  // 0 = disabled

#ifndef ARDA_NO_PRIORITY
    // Set task priority (0-15, higher = runs first). Values > 15 are clamped to 15.
    // Returns false if task is invalid.
    bool setTaskPriority(int8_t taskId, uint8_t priority);

    // Get task priority (0-15). Returns 0 for invalid tasks.
    uint8_t getTaskPriority(int8_t taskId) const;
#endif

    // Rename a task. Returns false if task invalid, name invalid, or name already exists.
    bool renameTask(int8_t taskId, const char* newName);

    // -------------------------------------------------------------------------
    // Batch operations - perform operations on multiple tasks
    // -------------------------------------------------------------------------

    // Returns count of successful operations. On partial failure, error reflects the FIRST failure.
    // Optional failedId parameter receives the ID of the first task that failed (-1 if all succeeded).
    // Operations are attempted in order; failures don't stop subsequent attempts.
    int8_t startTasks(const int8_t* taskIds, int8_t count, int8_t* failedId = nullptr);
    int8_t stopTasks(const int8_t* taskIds, int8_t count, int8_t* failedId = nullptr);
    int8_t pauseTasks(const int8_t* taskIds, int8_t count, int8_t* failedId = nullptr);
    int8_t resumeTasks(const int8_t* taskIds, int8_t count, int8_t* failedId = nullptr);

    // Convenience methods - operate on ALL non-deleted tasks
    // Returns count of successful operations.
    int8_t startAllTasks();
    int8_t stopAllTasks();    // Returns count stopped (includes partial success: TeardownSkipped/TeardownChangedState)
    int8_t pauseAllTasks();
    int8_t resumeAllTasks();

    // -------------------------------------------------------------------------
    // Task queries
    // -------------------------------------------------------------------------

    int8_t getTaskCount() const;        // Number of active (non-deleted) tasks
    int8_t getActiveTaskCount() const;  // Alias for getTaskCount()
    int8_t getSlotCount() const;        // Total slots used (including deleted) - for iteration
    static constexpr int8_t getMaxTasks() { return ARDA_MAX_TASKS; }  // Maximum task capacity

    const char* getTaskName(int8_t taskId) const;  // Returns nullptr if invalid
    TaskState getTaskState(int8_t taskId) const;   // Returns TaskState::Invalid if invalid

    // NOTE: These getters return 0 for invalid tasks, but 0 is also a valid value
    // (runCount=0 means not yet run, interval=0 means every cycle, timeout=0 means disabled).
    // Call isValidTask() first if you need to distinguish invalid from zero.
    uint32_t getTaskRunCount(int8_t taskId) const;  // Runs since last start (reset on each startTask)
    uint32_t getTaskInterval(int8_t taskId) const;
    uint32_t getTaskTimeout(int8_t taskId) const;

    int8_t getCurrentTask() const;          // Returns ID of currently executing task, or -1
    bool isValidTask(int8_t taskId) const;  // Returns true if taskId refers to a non-deleted task
    bool isTaskYielded(int8_t taskId) const;  // Returns true if task is currently inside a yield() call

    // Find task by name. Returns task ID or -1 if not found.
    // Name comparison is case-sensitive by default ("MyTask" != "mytask").
    // Define ARDA_CASE_INSENSITIVE_NAMES before including Arda.h for case-insensitive matching.
    int8_t findTaskByName(const char* name) const;

    // Task iteration helper - fills outIds with valid (non-deleted) task IDs.
    // Returns total count of valid tasks (may exceed maxCount).
    // Only the first min(count, maxCount) IDs are written to outIds.
    // Pass nullptr to just get the count without storing IDs.
    // Example: int8_t ids[16]; int8_t n = os.getValidTaskIds(ids, 16);
    int8_t getValidTaskIds(int8_t* outIds, int8_t maxCount) const;

    // Task callback queries - returns false if task is invalid or callback is null
    bool hasTaskSetup(int8_t taskId) const;
    bool hasTaskLoop(int8_t taskId) const;
    bool hasTaskTeardown(int8_t taskId) const;

    // -------------------------------------------------------------------------
    // Callback configuration
    // -------------------------------------------------------------------------

    // Set callback invoked when a task exceeds its timeout (called after task returns)
    void setTimeoutCallback(TimeoutCallback callback);

    // Set callback invoked for each task that fails to start during begin()
    void setStartFailureCallback(StartFailureCallback callback);

    // Set callback for debugging/tracing task execution (set to nullptr to disable)
    void setTraceCallback(TraceCallback callback);

    // -------------------------------------------------------------------------
    // Utility
    // -------------------------------------------------------------------------

    void yield();

    // Returns milliseconds since begin() was called, or millis() if not yet begun.
    uint32_t uptime() const;

    // -------------------------------------------------------------------------
    // Error handling
    // -------------------------------------------------------------------------

    ArdaError getError() const;       // Returns error code from most recent failed operation
    void clearError();
    static const char* errorString(ArdaError error);  // Convert error code to string

private:
    // Scheduler flags bit positions
    static constexpr uint8_t FLAG_BEGUN  = 0x01;  // bit 0: begin() has been called
    static constexpr uint8_t FLAG_IN_RUN = 0x02;  // bit 1: currently inside run()

    Task tasks[ARDA_MAX_TASKS];
    int8_t taskCount;        // Total slots used (including deleted) - for iteration bounds
    int8_t activeCount;      // Active (non-deleted) tasks - O(1) query via getActiveTaskCount()
    uint32_t startTime;
    int8_t freeListHead;     // Head of free slot linked list (-1 if none) for O(1) allocation
    int8_t currentTask;      // ID of currently executing task (-1 if none)
    uint8_t callbackDepth;   // Current callback nesting depth (guards against stack overflow)
    uint8_t flags_;          // Packed flags: bit 0 = begun, bit 1 = inRun
    ArdaError error_;        // Error code from most recent failed operation

    // User callbacks
    TimeoutCallback timeoutCallback;            // Called when task exceeds timeout
    StartFailureCallback startFailureCallback;  // Called when task fails to start in begin()
    TraceCallback traceCallback;                // Called for debug/trace events

    int8_t allocateSlot();   // Get next free slot from free list, or new slot. Returns -1 if full.
    void freeSlot(int8_t slot);  // Return slot to free list
    void runInternal(int8_t skipTask);  // Internal scheduler loop
    void emitTrace(int8_t taskId, TraceEvent event);  // Invoke trace callback with depth guard

    // Case-insensitive string comparison helper (only used if ARDA_CASE_INSENSITIVE_NAMES defined)
    static bool nameEquals(const char* a, const char* b);
};

// Global instance - define ARDA_NO_GLOBAL_INSTANCE before including Arda.h to disable
#ifndef ARDA_NO_GLOBAL_INSTANCE
extern Arda OS;
#endif

// Macro helpers for defining tasks
#define TASK_SETUP(name) void name##_setup()
#define TASK_LOOP(name) void name##_loop()
#define TASK_TEARDOWN(name) void name##_teardown()

// REGISTER_TASK returns the task ID but is often used without capturing it.
// Use REGISTER_TASK_ID if you need to store the ID for later operations.
// Note: These macros use the global 'OS' instance. If ARDA_NO_GLOBAL_INSTANCE is defined,
// use the _ON variants below that accept a scheduler parameter.
#ifndef ARDA_NO_GLOBAL_INSTANCE
#ifdef ARDA_NO_NAMES
#define REGISTER_TASK(name, interval) OS.createTask(name##_setup, name##_loop, interval)
#define REGISTER_TASK_WITH_TEARDOWN(name, interval) OS.createTask(name##_setup, name##_loop, interval, name##_teardown)
#define REGISTER_TASK_ID(var, name, interval) int8_t var = OS.createTask(name##_setup, name##_loop, interval)
#define REGISTER_TASK_ID_WITH_TEARDOWN(var, name, interval) int8_t var = OS.createTask(name##_setup, name##_loop, interval, name##_teardown)
#else
#define REGISTER_TASK(name, interval) OS.createTask(#name, name##_setup, name##_loop, interval)
#define REGISTER_TASK_WITH_TEARDOWN(name, interval) OS.createTask(#name, name##_setup, name##_loop, interval, name##_teardown)
#define REGISTER_TASK_ID(var, name, interval) int8_t var = OS.createTask(#name, name##_setup, name##_loop, interval)
#define REGISTER_TASK_ID_WITH_TEARDOWN(var, name, interval) int8_t var = OS.createTask(#name, name##_setup, name##_loop, interval, name##_teardown)
#endif
#endif

// Variants that accept a scheduler parameter (work with or without global instance)
#ifdef ARDA_NO_NAMES
#define REGISTER_TASK_ON(scheduler, name, interval) (scheduler).createTask(name##_setup, name##_loop, interval)
#define REGISTER_TASK_ON_WITH_TEARDOWN(scheduler, name, interval) (scheduler).createTask(name##_setup, name##_loop, interval, name##_teardown)
#define REGISTER_TASK_ID_ON(var, scheduler, name, interval) int8_t var = (scheduler).createTask(name##_setup, name##_loop, interval)
#define REGISTER_TASK_ID_ON_WITH_TEARDOWN(var, scheduler, name, interval) int8_t var = (scheduler).createTask(name##_setup, name##_loop, interval, name##_teardown)
#else
#define REGISTER_TASK_ON(scheduler, name, interval) (scheduler).createTask(#name, name##_setup, name##_loop, interval)
#define REGISTER_TASK_ON_WITH_TEARDOWN(scheduler, name, interval) (scheduler).createTask(#name, name##_setup, name##_loop, interval, name##_teardown)
#define REGISTER_TASK_ID_ON(var, scheduler, name, interval) int8_t var = (scheduler).createTask(#name, name##_setup, name##_loop, interval)
#define REGISTER_TASK_ID_ON_WITH_TEARDOWN(var, scheduler, name, interval) int8_t var = (scheduler).createTask(#name, name##_setup, name##_loop, interval, name##_teardown)
#endif
