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
#define ARDA_VERSION_MINOR 1
#define ARDA_VERSION_PATCH 2
#define ARDA_VERSION_STRING "1.1.2"

// Configuration constants - adjust these to tune memory usage vs capability
#ifndef ARDA_MAX_TASKS
#define ARDA_MAX_TASKS 16          // Maximum concurrent tasks (1-127). Each task uses ~46-64 bytes depending on platform.
#endif
#ifndef ARDA_MAX_NAME_LEN
#define ARDA_MAX_NAME_LEN 16       // Task name buffer size. Max usable chars = 15 (for null terminator).
#endif
#ifndef ARDA_MAX_CALLBACK_DEPTH
// Default 8: balances safety vs flexibility. Each nesting level uses ~20-50 bytes
// of stack for return addresses and saved registers, plus the callback's local
// variables. On ATmega328 (2KB RAM), 8 levels is safe for typical callbacks.
// Reduce to 4-6 for memory-constrained boards or callbacks with large locals.
#define ARDA_MAX_CALLBACK_DEPTH 8
#endif
#if ARDA_MAX_CALLBACK_DEPTH < 1
#error "ARDA_MAX_CALLBACK_DEPTH must be at least 1"
#endif

// Optional features - define before including Arda.h to enable
// #define ARDA_CASE_INSENSITIVE_NAMES  // Make findTaskByName case-insensitive
// #define ARDA_NO_GLOBAL_INSTANCE      // Don't create global 'OS' instance (create your own)
// #define ARDA_NO_PRIORITY             // Disable priority scheduling (saves code size, uses array-order execution)
// #define ARDA_NO_NAMES                // Disable task names (saves ARDA_MAX_NAME_LEN bytes per task)
// #define ARDA_NO_SHELL                // Disable built-in shell task entirely
// #define ARDA_SHELL_MANUAL_START      // Don't auto-start shell in begin()
// #define ARDA_SHELL_MINIMAL           // Only core commands (p/r/s/t/d/l/h)
// #define ARDA_NO_WATCHDOG             // Disable hardware watchdog (AVR only, auto-enabled by default)
// #define ARDA_YIELD                   // Enable yield() - USE WITH CAUTION (see README)

// Shell is only active when enabled AND global instance exists
#if !defined(ARDA_NO_SHELL) && !defined(ARDA_NO_GLOBAL_INSTANCE)
#define ARDA_SHELL_ACTIVE 1
#define ARDA_SHELL_TASK_ID 0
#ifndef ARDA_SHELL_BUF_SIZE
#define ARDA_SHELL_BUF_SIZE 16    // Command buffer size (min 8 recommended)
#endif
#endif

// Hardware watchdog (AVR only) - opt-in, resets MCU if any task blocks >8 seconds
// Enable with: #define ARDA_WATCHDOG before including Arda.h
#if defined(__AVR__) && defined(ARDA_WATCHDOG)
#include <avr/wdt.h>
#endif

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
#ifdef ARDA_YIELD
    TaskYielded,         // Cannot delete task that has yielded
#endif
    AlreadyBegun,        // begin() was already called
    CallbackDepth,       // Maximum callback nesting depth exceeded
    StateChanged,        // Callback modified task state unexpectedly
    InCallback,          // Cannot call reset() from within a callback
    NotSupported,        // Feature disabled at compile time (e.g., names when ARDA_NO_NAMES)
    InvalidValue         // Parameter value out of valid range (e.g., priority > Highest)
};

// Result codes for startTask() - disambiguates success from partial success
enum class StartResult : uint8_t {
    Success,              // Task started, setup ran (if defined), task is Running
    SetupChangedState,    // Setup ran but modified task state (e.g., stopped itself) - check getError()
    Failed                // Task couldn't be started - check getError() for reason
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

// Debug/trace events for monitoring task lifecycle (9 events).
// Note: "ing" variants (TaskStarting, TaskStopping) bracket user callbacks (setup/teardown).
// Pause/resume have no callbacks, so only "ed" variants exist - no code runs between states.
enum class TraceEvent : uint8_t {
    TaskStarting,   // About to run setup() - fires before callback
    TaskStarted,    // setup() completed, task is now Running
    TaskLoopBegin,  // About to run loop() - fires before callback
    TaskLoopEnd,    // loop() completed
    TaskStopping,   // About to run teardown() - fires before callback (only if teardown exists)
    TaskStopped,    // Task is now Stopped (teardown completed or skipped)
    TaskPaused,     // Task state changed to Paused (no callback, so no "TaskPausing")
    TaskResumed,    // Task state changed to Running (no callback, so no "TaskResuming")
    TaskDeleted     // Task was deleted (already invalidated, ID only)
};
typedef void (*TraceCallback)(int8_t taskId, TraceEvent event);

#ifndef ARDA_NO_PRIORITY
// Named priority levels (5 levels, higher = runs first)
enum class TaskPriority : uint8_t {
    Lowest  = 0,
    Low     = 1,
    Normal  = 2,   // Default
    High    = 3,
    Highest = 4
};
#endif

// Task flags bit positions (packed into single byte for RAM efficiency)
#define ARDA_TASK_STATE_MASK   0x03  // bits 0-1: TaskState (0=Stopped, 1=Running, 2=Paused)
#define ARDA_TASK_RAN_BIT      0x04  // bit 2: ranThisCycle
#ifdef ARDA_YIELD
#define ARDA_TASK_YIELD_BIT    0x08  // bit 3: inYield
#endif
#ifndef ARDA_NO_PRIORITY
#define ARDA_TASK_PRIORITY_MASK  0x70  // bits 4-6: priority (0-4, 3 bits). Bit 7 reserved.
#define ARDA_TASK_PRIORITY_SHIFT 4
#define ARDA_DEFAULT_PRIORITY    2     // TaskPriority::Normal
#endif

#ifdef ARDA_NO_NAMES
// When names are disabled, use state value 3 (unused) as deletion marker.
// This avoids conflict with priority bits (4-7) which would cause false positives.
#define ARDA_TASK_DELETED_STATE  0x03  // state bits = 3: task deleted (only when ARDA_NO_NAMES)
#endif

// Task structure - fields ordered to minimize padding.
// Memory per task (with default ARDA_MAX_NAME_LEN=16):
//   AVR (8-bit):  16 + 3*2 + 4*4 + 1 = ~39 bytes (23 bytes with ARDA_NO_NAMES)
//   ESP8266/32:   16 + 3*4 + 4*4 + 1 = ~45 bytes (29 bytes with ARDA_NO_NAMES)
//   32-bit ARM:   16 + 3*4 + 4*4 + 1 = ~45 bytes (29 bytes with ARDA_NO_NAMES)
//   64-bit:       16 + 3*8 + 4*4 + 1 + padding = ~57 bytes (41 bytes with ARDA_NO_NAMES)
// Total for 16 tasks: ~624 bytes (AVR), ~720 bytes (32-bit), ~912 bytes (64-bit)
struct Task {
#ifndef ARDA_NO_NAMES
    char name[ARDA_MAX_NAME_LEN]; // Copied, safe from dangling pointers; empty = deleted
#endif
    TaskCallback setup;           // One-time initialization callback
    TaskCallback loop;            // Repeated execution callback
    TaskCallback teardown;        // Cleanup callback (called on stop)
    uint32_t interval;            // Run interval in ms (0 = every cycle)
    uint32_t lastRun;             // Actual execution time (for scheduling and getTaskLastRun)
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
    // Packed flags: bits 0-1 = state, bit 2 = ranThisCycle, bit 3 = inYield (if ARDA_YIELD)
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

    // Initialize scheduler and start all registered tasks.
    // Returns number of tasks started, or -1 if already begun (check hasBegun() first).
    // If startFailureCallback is set, it will be called for each task that fails to start.
    // Use createTask(..., autoStart=false) if you need tasks to start in Stopped state.
    int8_t begin();

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
    // Returns -1 if max tasks reached or auto-start fails (check getError()).
    // Use autoStart=false to handle start failures manually.
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
    // name is duplicate, max tasks reached, or auto-start fails (check getError()).
    // If begin() was already called and autoStart is true (default), the new task
    // is automatically started. Use autoStart=false to handle start failures manually.
    int8_t createTask(const char* name, TaskCallback setup, TaskCallback loop,
                      uint32_t intervalMs = 0, TaskCallback teardown = nullptr,
                      bool autoStart = true);
#endif

#ifndef ARDA_NO_PRIORITY
    // Create a task with explicit priority (higher runs first).
    // Priority is last to maintain consistent positional order with the standard overload:
    // (name, setup, loop, interval, teardown, autoStart, priority)
    // See TaskPriority enum for levels (Lowest=0 through Highest=4).
    int8_t createTask(const char* name, TaskCallback setup, TaskCallback loop,
                      uint32_t intervalMs, TaskCallback teardown,
                      bool autoStart, TaskPriority priority);
#endif

    bool deleteTask(int8_t taskId);

    // Stop and delete a task in one operation. Handles already-stopped tasks gracefully.
    // Returns false if: task invalid, teardown restarted task, or task is currently executing.
    // Check getError() for details: InvalidId, StateChanged, TaskExecuting, TaskYielded.
    bool killTask(int8_t taskId);

    // -------------------------------------------------------------------------
    // Task state control
    // -------------------------------------------------------------------------

    // Start a stopped task. By default (runImmediately=false), the task waits until
    // the next run() cycle. For interval-based tasks, it also waits one full interval.
    // Set runImmediately=true to skip the initial interval wait for interval-based tasks.
    // NOTE: Tasks started during a run() cycle cannot run in that same cycle regardless
    // of this flag; they will run on the next cycle. The runImmediately flag primarily
    // affects interval-based timing, not same-cycle execution.
    // NOTE: runCount is reset to 0 on each start. It tracks loop executions since the
    // most recent start, not cumulative runs across the task's lifetime.
    // Returns StartResult enum:
    //   StartResult::Success: Task started, setup ran, task is Running
    //   StartResult::SetupChangedState: Setup ran but changed state (getError() = StateChanged)
    //   StartResult::Failed: Task couldn't be started (check getError())
    StartResult startTask(int8_t taskId, bool runImmediately = false);

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

    // Change task interval. By default, keeps existing timing (next run based on lastRun + new interval).
    // Set resetTiming=true to reset lastRun to now (task waits full new interval from now).
    bool setTaskInterval(int8_t taskId, uint32_t intervalMs, bool resetTiming = false);

    bool setTaskTimeout(int8_t taskId, uint32_t timeoutMs);  // 0 = disabled

#ifndef ARDA_NO_PRIORITY
    // Set task priority (higher = runs first). Returns false with InvalidValue if out of range.
    // Returns false if task is invalid. See TaskPriority enum for valid levels.
    bool setTaskPriority(int8_t taskId, TaskPriority priority);

    // Get task priority. Returns TaskPriority::Lowest for invalid tasks.
    TaskPriority getTaskPriority(int8_t taskId) const;
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
    int8_t stopAllTasks();    // Returns count stopped (includes TeardownSkipped; excludes TeardownChangedState since task restarted)
    int8_t pauseAllTasks();
    int8_t resumeAllTasks();

    // -------------------------------------------------------------------------
    // Task queries
    // -------------------------------------------------------------------------

    int8_t getTaskCount() const;        // Number of active (non-deleted) tasks
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
    uint32_t getTaskLastRun(int8_t taskId) const;   // millis() snapshot when task last ran (0 if never ran or invalid)

    int8_t getCurrentTask() const;          // Returns ID of currently executing task, or -1
    bool isValidTask(int8_t taskId) const;  // Returns true if taskId refers to a non-deleted task
#ifdef ARDA_YIELD
    bool isTaskYielded(int8_t taskId) const;  // Returns true if task is currently inside a yield() call
#endif

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

#ifdef ARDA_YIELD
    void yield();
#endif

    // Returns milliseconds since begin() was called, or 0 if not yet begun.
    uint32_t uptime() const;

    // -------------------------------------------------------------------------
    // Error handling
    // -------------------------------------------------------------------------

    ArdaError getError() const;       // Returns error code from most recent failed operation
    void clearError();
    static const char* errorString(ArdaError error);  // Convert error code to string

    // -------------------------------------------------------------------------
    // Shell (requires: !ARDA_NO_SHELL && !ARDA_NO_GLOBAL_INSTANCE)
    // -------------------------------------------------------------------------

#ifdef ARDA_SHELL_ACTIVE
    void setShellStream(Stream& stream);  // Default: Serial. WARNING: Stream must remain valid.
    bool isShellRunning() const;          // True if shell task exists AND is Running
    static constexpr int8_t getShellTaskId() { return ARDA_SHELL_TASK_ID; }
    void exec(const char* cmd);           // Execute shell command programmatically
#ifndef ARDA_NO_SHELL_ECHO
    void setShellEcho(bool enabled);      // Echo commands back with "> " prefix (default: on)
#endif

#ifdef ARDA_SHELL_MANUAL_START
    bool startShell();
    bool stopShell();
#endif
#endif

private:
    // Scheduler flags bit positions
    static constexpr uint8_t FLAG_BEGUN    = 0x01;  // bit 0: begin() has been called
    static constexpr uint8_t FLAG_IN_RUN   = 0x02;  // bit 1: currently inside run()
    static constexpr uint8_t FLAG_IN_BEGIN = 0x04;  // bit 2: currently inside begin() task-start loop

    Task tasks[ARDA_MAX_TASKS];
    int8_t taskCount;        // Total slots used (including deleted) - for iteration bounds
    int8_t activeCount;      // Active (non-deleted) tasks - O(1) query via getTaskCount()
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
    int8_t initTaskFields_(int8_t id, TaskCallback setup, TaskCallback loop,
                           uint32_t intervalMs, TaskCallback teardown, bool autoStart);  // Common task init

    // Case-insensitive string comparison helper (only used if ARDA_CASE_INSENSITIVE_NAMES defined)
    static bool nameEquals(const char* a, const char* b);

#ifdef ARDA_SHELL_ACTIVE
    Stream* shellStream_;
    char shellBuf_[ARDA_SHELL_BUF_SIZE];
    uint8_t shellBufIdx_;
    bool shellBusy_;                      // Re-entrancy guard for exec()
    bool shellDeleted_;                   // Track if shell task was deleted (for isShellRunning)
    int8_t pendingSelfDelete_;            // Task ID pending self-deletion, or -1
#ifndef ARDA_NO_SHELL_ECHO
    bool shellEcho_;                      // Echo received commands back to stream
#endif
    void initShell_();                    // Initialize shell task in slot 0
    void shellCmd_(uint8_t len);
    void shellList_();
#ifndef ARDA_SHELL_MINIMAL
    void shellInfo_(int8_t id);
    uint32_t shellParseArg2_(uint8_t len);  // Parse second numeric argument from command buffer
#endif
    friend void ardaShellLoop_();         // Static callback needs access
#endif

#ifdef ARDA_INTERNAL_TEST
public:
    // Test accessor for internal task state (e.g., overflow testing)
    Task* getTaskPtr_(int8_t id) { return (id >= 0 && id < taskCount) ? &tasks[id] : nullptr; }
#endif
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
