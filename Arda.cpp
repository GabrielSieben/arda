/*
 * Arda - A cooperative multitasking scheduler for Arduino
 * Implementation
 */

#include "Arda.h"

// Global instance (unless disabled)
#ifndef ARDA_NO_GLOBAL_INSTANCE
Arda OS;
#endif

// Forward declarations for static helpers (defined at bottom with other internals)
static inline bool isDeleted(const Task& task);
static inline void markDeleted(Task& task);
static inline void clearDeleted(Task& task);
static inline TaskState extractState(const Task& task);
static inline void updateState(Task& task, TaskState state);
static inline bool checkRanThisCycle(const Task& task);
static inline void updateRanThisCycle(Task& task, bool val);
static inline bool checkInYield(const Task& task);
static inline void updateInYield(Task& task, bool val);
#ifndef ARDA_NO_PRIORITY
static inline uint8_t extractPriority(const Task& task);
static inline void updatePriority(Task& task, uint8_t priority);
#endif

// =============================================================================
// Constructor
// =============================================================================

Arda::Arda() {
    taskCount = 0;
    activeCount = 0;
    startTime = 0;
    currentTask = -1;
    freeListHead = -1;  // Empty free list (no deleted slots yet)
    callbackDepth = 0;
    flags_ = 0;         // begun=false, inRun=false
    error_ = ArdaError::Ok;
    timeoutCallback = nullptr;
    startFailureCallback = nullptr;
    traceCallback = nullptr;

    for (int8_t i = 0; i < ARDA_MAX_TASKS; i++) {
        tasks[i].setup = nullptr;
        tasks[i].loop = nullptr;
        tasks[i].teardown = nullptr;
        tasks[i].interval = 0;
        tasks[i].lastRun = 0;
        tasks[i].runCount = 0;    // Union: runCount/nextFree share memory; init for unused state
        tasks[i].timeout = 0;
#ifdef ARDA_NO_NAMES
        // Mark as deleted using state value 3
  #ifndef ARDA_NO_PRIORITY
        tasks[i].flags = (ARDA_DEFAULT_PRIORITY << ARDA_TASK_PRIORITY_SHIFT) | ARDA_TASK_DELETED_STATE;
  #else
        tasks[i].flags = ARDA_TASK_DELETED_STATE;
  #endif
#else
        tasks[i].name[0] = '\0';  // Empty string = deleted/unused
  #ifndef ARDA_NO_PRIORITY
        tasks[i].flags = ARDA_DEFAULT_PRIORITY << ARDA_TASK_PRIORITY_SHIFT;
  #else
        tasks[i].flags = 0;       // state=Stopped, ranThisCycle=false, inYield=false
  #endif
#endif
    }
}

// =============================================================================
// Scheduler control
// =============================================================================

int8_t Arda::begin(bool autoStartTasks) {
    // Guard against multiple begin() calls - use reset() first if restarting
    if (flags_ & FLAG_BEGUN) {
        error_ = ArdaError::AlreadyBegun;
        return -1;
    }

    flags_ |= FLAG_BEGUN;
    startTime = millis();

    // If autoStartTasks is false, just initialize without starting tasks
    if (!autoStartTasks) {
        error_ = ArdaError::Ok;
        return 0;
    }

    int8_t started = 0;

    // Snapshot valid task IDs to protect against callbacks modifying the task array.
    // Without this, a setup() callback could create/delete tasks and corrupt iteration.
    int8_t snapshot[ARDA_MAX_TASKS];
    int8_t snapshotCount = 0;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i])) {
            snapshot[snapshotCount++] = i;
        }
    }

    // Start all registered tasks from the snapshot
    for (int8_t j = 0; j < snapshotCount; j++) {
        int8_t i = snapshot[j];
        if (!isValidTask(i)) continue;  // Task was deleted during iteration
        if (startTask(i)) {
            started++;
        } else {
            // Task failed to start - notify via callback if registered
            if (startFailureCallback != nullptr) {
                callbackDepth++;
                startFailureCallback(i, error_);
                callbackDepth--;
            }
        }
    }

    // Only clear error if all tasks started successfully
    if (started == snapshotCount) {
        error_ = ArdaError::Ok;
    }
    // Otherwise, error_ retains the error from the last failed startTask
    return started;
}

bool Arda::run() {
    if (!(flags_ & FLAG_BEGUN)) {
        error_ = ArdaError::WrongState;
        return false;
    }
    runInternal(-1);  // Run all tasks
    return true;
}

void Arda::runInternal(int8_t skipTask) {
    // Reentrancy guard - prevent recursive calls
    if (flags_ & FLAG_IN_RUN) return;
    flags_ |= FLAG_IN_RUN;

    // Snapshot valid task IDs to protect against callbacks modifying the task array.
    // Without this, a callback could delete/create tasks and corrupt iteration.
    int8_t snapshot[ARDA_MAX_TASKS];
    int8_t snapshotCount = 0;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i])) {
            snapshot[snapshotCount++] = i;
        }
    }

    // Reset ranThisCycle flags at start of each run() cycle
    // (but not when called from yield, which uses skipTask >= 0)
    if (skipTask < 0) {
        for (int8_t j = 0; j < snapshotCount; j++) {
            int8_t i = snapshot[j];
            if (isValidTask(i)) {
                updateRanThisCycle(tasks[i], false);
            }
        }
    }

#ifndef ARDA_NO_PRIORITY
    // Priority-based scheduling: repeatedly find and run highest-priority ready task
    // Snapshot millis() once at cycle start for consistent readiness evaluation
    uint32_t now = millis();
    bool taskRan = true;
    while (taskRan) {
        taskRan = false;
        int8_t bestTask = -1;
        uint8_t bestPriority = 0;

        // Find highest-priority ready task
        for (int8_t j = 0; j < snapshotCount; j++) {
            int8_t i = snapshot[j];
            if (i == skipTask) continue;
            if (!isValidTask(i)) continue;
            if (extractState(tasks[i]) != TaskState::Running) continue;
            if (tasks[i].loop == nullptr) continue;
            if (checkRanThisCycle(tasks[i])) continue;
            if (tasks[i].interval != 0 && (now - tasks[i].lastRun < tasks[i].interval)) continue;

            uint8_t priority = extractPriority(tasks[i]);
            if (bestTask < 0 || priority > bestPriority) {
                bestTask = i;
                bestPriority = priority;
            }
        }

        if (bestTask >= 0) {
            int8_t i = bestTask;
            // Guard against excessive callback nesting (prevents stack overflow)
            if (callbackDepth >= ARDA_MAX_CALLBACK_DEPTH) {
                // Can't run any more tasks this cycle due to depth limit
                break;
            }
            taskRan = true;
            int8_t prevTask = currentTask;
            currentTask = i;
            updateRanThisCycle(tasks[i], true);
            callbackDepth++;

            emitTrace(i, TraceEvent::TaskLoopBegin);
            uint32_t execStart = millis();
            tasks[i].loop();
            uint32_t execDuration = millis() - execStart;
            emitTrace(i, TraceEvent::TaskLoopEnd);

            callbackDepth--;
            currentTask = prevTask;

            if (tasks[i].timeout > 0 && execDuration > tasks[i].timeout && timeoutCallback != nullptr) {
                callbackDepth++;
                timeoutCallback(i, execDuration);
                callbackDepth--;
            }

            if (extractState(tasks[i]) == TaskState::Running) {
                if (tasks[i].interval > 0) {
                    tasks[i].lastRun += tasks[i].interval;
                    if (now - tasks[i].lastRun >= tasks[i].interval) {
                        tasks[i].lastRun = now;
                    }
                } else {
                    tasks[i].lastRun = now;
                }
                tasks[i].runCount++;
            }
        }
    }
#else
    // Original array-order scheduling (when ARDA_NO_PRIORITY is defined)
    uint32_t now = millis();
    for (int8_t j = 0; j < snapshotCount; j++) {
        int8_t i = snapshot[j];
        if (i == skipTask) continue;  // Skip specified task (for yield)
        if (!isValidTask(i)) continue;  // Task was deleted during iteration
        if (extractState(tasks[i]) != TaskState::Running) continue;
        if (tasks[i].loop == nullptr) continue;
        if (checkRanThisCycle(tasks[i])) continue;  // Already ran this cycle (prevents double execution from yield)

        // Check if it's time to run this task
        if (tasks[i].interval == 0 || (now - tasks[i].lastRun >= tasks[i].interval)) {
            // Guard against excessive callback nesting (prevents stack overflow)
            if (callbackDepth >= ARDA_MAX_CALLBACK_DEPTH) {
                continue;  // Skip this task for now, will try again next cycle
            }
            int8_t prevTask = currentTask;
            currentTask = i;
            updateRanThisCycle(tasks[i], true);  // Mark as run before executing (in case of yield)
            callbackDepth++;

            // Track execution time for timeout detection
            // NOTE: Unsigned arithmetic correctly handles millis() overflow here.
            // If millis() wraps during execution (e.g., 4294967290 -> 94), the
            // subtraction still yields the correct duration due to unsigned wraparound.
            emitTrace(i, TraceEvent::TaskLoopBegin);
            uint32_t execStart = millis();
            tasks[i].loop();
            uint32_t execDuration = millis() - execStart;
            emitTrace(i, TraceEvent::TaskLoopEnd);

            callbackDepth--;
            currentTask = prevTask;

            // Check for timeout (only if timeout is configured and callback is set)
            if (tasks[i].timeout > 0 && execDuration > tasks[i].timeout && timeoutCallback != nullptr) {
                callbackDepth++;
                timeoutCallback(i, execDuration);
                callbackDepth--;
            }

            // Only update metadata if task is still running (it may have stopped itself)
            if (extractState(tasks[i]) == TaskState::Running) {
                // For interval-based tasks, advance by interval to maintain cadence
                // For interval=0 tasks, use current time (they run every cycle anyway)
                if (tasks[i].interval > 0) {
                    tasks[i].lastRun += tasks[i].interval;
                    // If significantly behind (missed 2+ intervals), reset to now
                    // to prevent rapid catch-up runs. If only ~1 interval behind,
                    // we maintain cadence (original schedule) at the cost of a
                    // potentially short gap until the next scheduled time.
                    // Example: interval=100, scheduled at 200,300,400. If the 200
                    // run is delayed to 290, the 300 run still fires at 300 (10ms gap)
                    // to preserve long-term timing accuracy.
                    if (now - tasks[i].lastRun >= tasks[i].interval) {
                        tasks[i].lastRun = now;
                    }
                } else {
                    tasks[i].lastRun = now;
                }
                tasks[i].runCount++;
            }
        }
    }
#endif

    flags_ &= ~FLAG_IN_RUN;
}

bool Arda::reset(bool preserveCallbacks) {
    // Cannot reset while inside a callback - would corrupt scheduler state
    // since the calling task's stack frame still references cleared data
    if (callbackDepth > 0) {
        error_ = ArdaError::InCallback;
        return false;
    }

    bool allTeardownsRan = true;

    // Snapshot valid task IDs to protect against teardown callbacks modifying the task array.
    int8_t snapshot[ARDA_MAX_TASKS];
    int8_t snapshotCount = 0;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i])) {
            snapshot[snapshotCount++] = i;
        }
    }

    // Stop all running/paused tasks from the snapshot (calls teardown)
    for (int8_t j = 0; j < snapshotCount; j++) {
        int8_t i = snapshot[j];
        if (!isValidTask(i)) continue;  // Task was deleted during iteration
        if (extractState(tasks[i]) == TaskState::Stopped) continue;
        StopResult result = stopTask(i);
        if (result == StopResult::TeardownSkipped || result == StopResult::TeardownChangedState) {
            allTeardownsRan = false;
        }
        // Continue stopping other tasks regardless of result
    }

    // Clear all task slots
    for (int8_t i = 0; i < ARDA_MAX_TASKS; i++) {
        tasks[i].setup = nullptr;
        tasks[i].loop = nullptr;
        tasks[i].teardown = nullptr;
        tasks[i].interval = 0;
        tasks[i].lastRun = 0;
        tasks[i].runCount = 0;    // Union member; nextFree shares this memory
        tasks[i].timeout = 0;
#ifdef ARDA_NO_NAMES
        // Mark as deleted using state value 3
  #ifndef ARDA_NO_PRIORITY
        tasks[i].flags = (ARDA_DEFAULT_PRIORITY << ARDA_TASK_PRIORITY_SHIFT) | ARDA_TASK_DELETED_STATE;
  #else
        tasks[i].flags = ARDA_TASK_DELETED_STATE;
  #endif
#else
        tasks[i].name[0] = '\0';
  #ifndef ARDA_NO_PRIORITY
        tasks[i].flags = ARDA_DEFAULT_PRIORITY << ARDA_TASK_PRIORITY_SHIFT;
  #else
        tasks[i].flags = 0;       // state=Stopped, ranThisCycle=false, inYield=false
  #endif
#endif
    }

    taskCount = 0;
    activeCount = 0;
    startTime = 0;
    currentTask = -1;
    freeListHead = -1;  // Empty free list (no deleted slots)
    flags_ = 0;         // begun=false, inRun=false
    callbackDepth = 0;

    // Optionally clear user callbacks for a complete clean slate
    if (!preserveCallbacks) {
        timeoutCallback = nullptr;
        startFailureCallback = nullptr;
        traceCallback = nullptr;
    }

    // Set final error state based on whether all teardowns ran
    if (allTeardownsRan) {
        error_ = ArdaError::Ok;
    } else {
        error_ = ArdaError::CallbackDepth;
    }

    return allTeardownsRan;
}

bool Arda::hasBegun() const {
    return (flags_ & FLAG_BEGUN) != 0;
}

// =============================================================================
// Task creation and deletion
// =============================================================================

#ifdef ARDA_NO_NAMES

// Nameless createTask - primary implementation when ARDA_NO_NAMES is defined
int8_t Arda::createTask(TaskCallback setup, TaskCallback loop,
                        uint32_t intervalMs, TaskCallback teardown, bool autoStart) {
    // Allocate a slot (O(1) from free list or new slot)
    int8_t id = allocateSlot();
    if (id == -1) {
        error_ = ArdaError::MaxTasks;
        return -1;
    }

    // Clear the deleted flag and initialize task
    clearDeleted(tasks[id]);
    tasks[id].setup = setup;
    tasks[id].loop = loop;
    tasks[id].teardown = teardown;
    tasks[id].interval = intervalMs;
    tasks[id].lastRun = 0;
    tasks[id].runCount = 0;    // Union member; nextFree shares this memory
    tasks[id].timeout = 0;
#ifndef ARDA_NO_PRIORITY
    tasks[id].flags = ARDA_DEFAULT_PRIORITY << ARDA_TASK_PRIORITY_SHIFT;
#else
    tasks[id].flags = 0;       // state=Stopped, ranThisCycle=false, inYield=false
#endif

    activeCount++;

    // Auto-start if begin() was already called and autoStart is enabled
    if ((flags_ & FLAG_BEGUN) && autoStart) {
        if (!startTask(id)) {
            // Auto-start failed - undo task creation for unambiguous error semantics.
            ArdaError startError = error_;  // Preserve the start failure reason
            markDeleted(tasks[id]);         // Mark as deleted
            freeSlot(id);                   // Return slot to free list
            activeCount--;
            error_ = startError;            // Restore the start failure error
            return -1;
        }
    }

    error_ = ArdaError::Ok;
    return id;
}

// Backward compatibility overload - accepts name parameter but ignores it
int8_t Arda::createTask(const char* name, TaskCallback setup, TaskCallback loop,
                        uint32_t intervalMs, TaskCallback teardown, bool autoStart) {
    (void)name;  // Silently ignore name when ARDA_NO_NAMES is defined
    return createTask(setup, loop, intervalMs, teardown, autoStart);
}

#else  // !ARDA_NO_NAMES

int8_t Arda::createTask(const char* name, TaskCallback setup, TaskCallback loop, uint32_t intervalMs, TaskCallback teardown, bool autoStart) {
    // Name is required
    if (name == nullptr) {
        error_ = ArdaError::NullName;
        return -1;
    }

    // Name must be non-empty
    if (name[0] == '\0') {
        error_ = ArdaError::EmptyName;
        return -1;
    }

    // Reject names that are too long (no silent truncation)
    if (strlen(name) >= ARDA_MAX_NAME_LEN) {
        error_ = ArdaError::NameTooLong;
        return -1;
    }

    // Reject duplicate names to avoid ambiguity in findTaskByName()
    if (findTaskByName(name) != -1) {
        error_ = ArdaError::DuplicateName;
        return -1;
    }

    // Allocate a slot (O(1) from free list or new slot)
    int8_t id = allocateSlot();
    if (id == -1) {
        error_ = ArdaError::MaxTasks;
        return -1;
    }

    // Copy name (length already validated above, but use strncpy for defense in depth)
    strncpy(tasks[id].name, name, ARDA_MAX_NAME_LEN - 1);
    tasks[id].name[ARDA_MAX_NAME_LEN - 1] = '\0';

    tasks[id].setup = setup;
    tasks[id].loop = loop;
    tasks[id].teardown = teardown;
    tasks[id].interval = intervalMs;
    tasks[id].lastRun = 0;
    tasks[id].runCount = 0;    // Union member; nextFree shares this memory
    tasks[id].timeout = 0;
#ifndef ARDA_NO_PRIORITY
    tasks[id].flags = ARDA_DEFAULT_PRIORITY << ARDA_TASK_PRIORITY_SHIFT;
#else
    tasks[id].flags = 0;       // state=Stopped, ranThisCycle=false, inYield=false
#endif

    activeCount++;

    // Auto-start if begin() was already called and autoStart is enabled
    if ((flags_ & FLAG_BEGUN) && autoStart) {
        if (!startTask(id)) {
            // Auto-start failed - undo task creation for unambiguous error semantics.
            // Caller should use autoStart=false if they want the task to exist on start failure.
            ArdaError startError = error_;  // Preserve the start failure reason
            markDeleted(tasks[id]);            // Mark as deleted
            freeSlot(id);                      // Return slot to free list
            activeCount--;
            error_ = startError;            // Restore the start failure error
            return -1;
        }
    }

    error_ = ArdaError::Ok;
    return id;
}

#endif  // ARDA_NO_NAMES

#ifndef ARDA_NO_PRIORITY
int8_t Arda::createTask(const char* name, TaskCallback setup, TaskCallback loop,
                        uint32_t intervalMs, uint8_t priority,
                        TaskCallback teardown, bool autoStart) {
    // Create task without auto-start so we can set priority first
    int8_t id = createTask(name, setup, loop, intervalMs, teardown, false);
    if (id >= 0) {
        updatePriority(tasks[id], priority);
        // Now handle auto-start if requested and begin() was already called
        if ((flags_ & FLAG_BEGUN) && autoStart) {
            if (!startTask(id)) {
                // Auto-start failed - undo task creation
                ArdaError startError = error_;
                markDeleted(tasks[id]);
                freeSlot(id);
                activeCount--;
                error_ = startError;
                return -1;
            }
        }
    }
    return id;
}
#endif

bool Arda::deleteTask(int8_t taskId) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }

    // Can't delete a running or paused task - must stop first
    if (extractState(tasks[taskId]) != TaskState::Stopped) {
        error_ = ArdaError::WrongState;
        return false;
    }

    // Can't delete the currently executing task (prevents corruption)
    if (taskId == currentTask) {
        error_ = ArdaError::TaskExecuting;
        return false;
    }

    // Can't delete a task that has yielded and is waiting for yield() to return
    if (checkInYield(tasks[taskId])) {
        error_ = ArdaError::TaskYielded;
        return false;
    }

    // Emit trace event before clearing data (so callback can still query task info)
    emitTrace(taskId, TraceEvent::TaskDeleted);

    // Clear task data and add to free list
    markDeleted(tasks[taskId]);
    tasks[taskId].setup = nullptr;
    tasks[taskId].loop = nullptr;
    tasks[taskId].teardown = nullptr;
    tasks[taskId].lastRun = 0;
    // Note: runCount shares union with nextFree; freeSlot() will set nextFree
    tasks[taskId].timeout = 0;
    // Reset flag bits; markDeleted already set the deleted state
#ifdef ARDA_NO_NAMES
    tasks[taskId].flags = ARDA_TASK_DELETED_STATE;  // state=deleted, clear other bits
#else
    // name[0]='\0' already marks as deleted
    tasks[taskId].flags = 0;  // state=Stopped, ranThisCycle=false, inYield=false
#endif

    // Add slot to free list (O(1) operation) - sets nextFree in union
    freeSlot(taskId);
    activeCount--;

    error_ = ArdaError::Ok;
    return true;
}

// =============================================================================
// Task state control
// =============================================================================

bool Arda::startTask(int8_t taskId, bool runImmediately) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    if (extractState(tasks[taskId]) != TaskState::Stopped) {
        error_ = ArdaError::WrongState;
        return false;
    }

    updateState(tasks[taskId], TaskState::Running);
    if (runImmediately && tasks[taskId].interval > 0) {
        // Set lastRun far enough in the past to trigger on next run() cycle.
        // Note: For zero-interval tasks, this has no effect since they run every cycle anyway.
        tasks[taskId].lastRun = millis() - tasks[taskId].interval;
    } else {
        tasks[taskId].lastRun = millis();  // Wait one full interval before first run
    }
    tasks[taskId].runCount = 0;
    updateRanThisCycle(tasks[taskId], false);
    updateInYield(tasks[taskId], false);  // Clear any stale yield state from previous run

    // Run setup function if provided
    if (tasks[taskId].setup != nullptr) {
        // Guard against excessive callback nesting (prevents stack overflow)
        if (callbackDepth >= ARDA_MAX_CALLBACK_DEPTH) {
            // Revert all state changes for clean failure semantics
            updateState(tasks[taskId], TaskState::Stopped);
            tasks[taskId].runCount = 0;
            tasks[taskId].lastRun = 0;
            updateRanThisCycle(tasks[taskId], false);
            error_ = ArdaError::CallbackDepth;
            return false;
        }
        emitTrace(taskId, TraceEvent::TaskStarting);
        int8_t prevTask = currentTask;
        currentTask = taskId;
        callbackDepth++;
        tasks[taskId].setup();
        callbackDepth--;
        currentTask = prevTask;

        // Check if setup() modified the task state (e.g., called stopTask on itself)
        if (extractState(tasks[taskId]) != TaskState::Running) {
            error_ = ArdaError::StateChanged;
            return false;
        }
    }

    emitTrace(taskId, TraceEvent::TaskStarted);
    error_ = ArdaError::Ok;
    return true;
}

bool Arda::pauseTask(int8_t taskId) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    if (extractState(tasks[taskId]) != TaskState::Running) {
        error_ = ArdaError::WrongState;
        return false;
    }

    updateState(tasks[taskId], TaskState::Paused);
    emitTrace(taskId, TraceEvent::TaskPaused);
    error_ = ArdaError::Ok;
    return true;
}

bool Arda::resumeTask(int8_t taskId) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    if (extractState(tasks[taskId]) != TaskState::Paused) {
        error_ = ArdaError::WrongState;
        return false;
    }

    updateState(tasks[taskId], TaskState::Running);
    emitTrace(taskId, TraceEvent::TaskResumed);
    error_ = ArdaError::Ok;
    return true;
}

StopResult Arda::stopTask(int8_t taskId) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return StopResult::Failed;
    }

    // Only stop if running or paused
    if (extractState(tasks[taskId]) == TaskState::Stopped) {
        error_ = ArdaError::WrongState;
        return StopResult::Failed;
    }

    // Can't stop a task that is mid-yield (teardown would run while stack frame active)
    if (checkInYield(tasks[taskId])) {
        error_ = ArdaError::TaskYielded;
        return StopResult::Failed;
    }

    // Set state BEFORE teardown so teardown can query correct state
    updateState(tasks[taskId], TaskState::Stopped);

    // Call teardown function if provided
    if (tasks[taskId].teardown != nullptr) {
        // Guard against excessive callback nesting (prevents stack overflow)
        // Task state is STOPPED, return TeardownSkipped to indicate teardown was not run.
        if (callbackDepth >= ARDA_MAX_CALLBACK_DEPTH) {
            emitTrace(taskId, TraceEvent::TaskStopped);
            error_ = ArdaError::CallbackDepth;
            return StopResult::TeardownSkipped;  // Task stopped but teardown didn't run
        }
        emitTrace(taskId, TraceEvent::TaskStopping);
        int8_t prevTask = currentTask;
        currentTask = taskId;
        callbackDepth++;
        tasks[taskId].teardown();
        callbackDepth--;
        currentTask = prevTask;

        // Check if teardown() modified the task state (e.g., called startTask on itself)
        if (extractState(tasks[taskId]) != TaskState::Stopped) {
            // Note: TaskStopped not emitted here since task is no longer stopped
            error_ = ArdaError::StateChanged;
            return StopResult::TeardownChangedState;  // Teardown ran but modified state
        }
    }

    emitTrace(taskId, TraceEvent::TaskStopped);
    error_ = ArdaError::Ok;
    return StopResult::Success;
}

// =============================================================================
// Task configuration
// =============================================================================

bool Arda::setTaskInterval(int8_t taskId, uint32_t intervalMs, bool resetTiming) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }

    tasks[taskId].interval = intervalMs;
    if (resetTiming) {
        // Reset lastRun to ensure consistent timing from when interval was changed.
        tasks[taskId].lastRun = millis();
    }
    // If resetTiming=false, keep existing lastRun so next run is based on
    // previous timing + new interval (useful for extending/shortening current wait)
    error_ = ArdaError::Ok;
    return true;
}

bool Arda::setTaskTimeout(int8_t taskId, uint32_t timeoutMs) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    tasks[taskId].timeout = timeoutMs;
    error_ = ArdaError::Ok;
    return true;
}

#ifndef ARDA_NO_PRIORITY
bool Arda::setTaskPriority(int8_t taskId, uint8_t priority) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    updatePriority(tasks[taskId], priority);
    error_ = ArdaError::Ok;
    return true;
}

uint8_t Arda::getTaskPriority(int8_t taskId) const {
    if (!isValidTask(taskId)) return 0;
    return extractPriority(tasks[taskId]);
}
#endif

#ifdef ARDA_NO_NAMES
bool Arda::renameTask(int8_t taskId, const char* newName) {
    (void)taskId;
    (void)newName;
    error_ = ArdaError::NotSupported;
    return false;
}
#else
bool Arda::renameTask(int8_t taskId, const char* newName) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }

    // Validate new name
    if (newName == nullptr) {
        error_ = ArdaError::NullName;
        return false;
    }
    if (newName[0] == '\0') {
        error_ = ArdaError::EmptyName;
        return false;
    }
    if (strlen(newName) >= ARDA_MAX_NAME_LEN) {
        error_ = ArdaError::NameTooLong;
        return false;
    }

    // Check for duplicate (but allow renaming to same name)
    int8_t existing = findTaskByName(newName);
    if (existing != -1 && existing != taskId) {
        error_ = ArdaError::DuplicateName;
        return false;
    }

    // Copy name (length already validated above, but use strncpy for defense in depth)
    strncpy(tasks[taskId].name, newName, ARDA_MAX_NAME_LEN - 1);
    tasks[taskId].name[ARDA_MAX_NAME_LEN - 1] = '\0';
    error_ = ArdaError::Ok;
    return true;
}
#endif

// =============================================================================
// Batch operations
// =============================================================================

int8_t Arda::startTasks(const int8_t* taskIds, int8_t count, int8_t* failedId) {
    if (failedId) *failedId = -1;
    if (taskIds == nullptr || count <= 0) {
        return 0;
    }
    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t i = 0; i < count; i++) {
        if (startTask(taskIds[i])) {
            succeeded++;
        } else if (firstError == ArdaError::Ok) {
            firstError = error_;  // Capture first failure
            if (failedId) *failedId = taskIds[i];
        }
        // Continue attempting remaining tasks even if one fails
    }
    // Restore first error (more diagnostic than last)
    if (firstError != ArdaError::Ok) {
        error_ = firstError;
    }
    return succeeded;
}

int8_t Arda::stopTasks(const int8_t* taskIds, int8_t count, int8_t* failedId) {
    if (failedId) *failedId = -1;
    if (taskIds == nullptr || count <= 0) {
        return 0;
    }
    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t i = 0; i < count; i++) {
        StopResult result = stopTask(taskIds[i]);
        // Count Success, TeardownSkipped, and TeardownChangedState as successful stops
        if (result != StopResult::Failed) {
            succeeded++;
        } else if (firstError == ArdaError::Ok) {
            firstError = error_;  // Capture first failure
            if (failedId) *failedId = taskIds[i];
        }
        // Continue attempting remaining tasks even if one fails
    }
    // Restore first error (more diagnostic than last)
    if (firstError != ArdaError::Ok) {
        error_ = firstError;
    }
    return succeeded;
}

int8_t Arda::pauseTasks(const int8_t* taskIds, int8_t count, int8_t* failedId) {
    if (failedId) *failedId = -1;
    if (taskIds == nullptr || count <= 0) {
        return 0;
    }
    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t i = 0; i < count; i++) {
        if (pauseTask(taskIds[i])) {
            succeeded++;
        } else if (firstError == ArdaError::Ok) {
            firstError = error_;  // Capture first failure
            if (failedId) *failedId = taskIds[i];
        }
    }
    // Restore first error (more diagnostic than last)
    if (firstError != ArdaError::Ok) {
        error_ = firstError;
    }
    return succeeded;
}

int8_t Arda::resumeTasks(const int8_t* taskIds, int8_t count, int8_t* failedId) {
    if (failedId) *failedId = -1;
    if (taskIds == nullptr || count <= 0) {
        return 0;
    }
    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t i = 0; i < count; i++) {
        if (resumeTask(taskIds[i])) {
            succeeded++;
        } else if (firstError == ArdaError::Ok) {
            firstError = error_;  // Capture first failure
            if (failedId) *failedId = taskIds[i];
        }
    }
    // Restore first error (more diagnostic than last)
    if (firstError != ArdaError::Ok) {
        error_ = firstError;
    }
    return succeeded;
}

int8_t Arda::startAllTasks() {
    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i]) && extractState(tasks[i]) == TaskState::Stopped) {
            if (startTask(i)) {
                succeeded++;
            } else if (firstError == ArdaError::Ok) {
                firstError = error_;
            }
        }
    }
    if (firstError != ArdaError::Ok) {
        error_ = firstError;
    } else {
        error_ = ArdaError::Ok;
    }
    return succeeded;
}

int8_t Arda::stopAllTasks() {
    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i]) && extractState(tasks[i]) != TaskState::Stopped) {
            StopResult result = stopTask(i);
            if (result != StopResult::Failed) {
                succeeded++;
            } else if (firstError == ArdaError::Ok) {
                firstError = error_;
            }
        }
    }
    if (firstError != ArdaError::Ok) {
        error_ = firstError;
    } else {
        error_ = ArdaError::Ok;
    }
    return succeeded;
}

int8_t Arda::pauseAllTasks() {
    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i]) && extractState(tasks[i]) == TaskState::Running) {
            if (pauseTask(i)) {
                succeeded++;
            } else if (firstError == ArdaError::Ok) {
                firstError = error_;
            }
        }
    }
    if (firstError != ArdaError::Ok) {
        error_ = firstError;
    } else {
        error_ = ArdaError::Ok;
    }
    return succeeded;
}

int8_t Arda::resumeAllTasks() {
    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i]) && extractState(tasks[i]) == TaskState::Paused) {
            if (resumeTask(i)) {
                succeeded++;
            } else if (firstError == ArdaError::Ok) {
                firstError = error_;
            }
        }
    }
    if (firstError != ArdaError::Ok) {
        error_ = firstError;
    } else {
        error_ = ArdaError::Ok;
    }
    return succeeded;
}

// =============================================================================
// Task queries
// =============================================================================

int8_t Arda::getTaskCount() const {
    return activeCount;
}

int8_t Arda::getActiveTaskCount() const {
    return activeCount;
}

int8_t Arda::getSlotCount() const {
    return taskCount;
}

#ifdef ARDA_NO_NAMES
const char* Arda::getTaskName(int8_t taskId) const {
    (void)taskId;
    return nullptr;
}
#else
const char* Arda::getTaskName(int8_t taskId) const {
    if (!isValidTask(taskId)) return nullptr;
    return tasks[taskId].name;
}
#endif

TaskState Arda::getTaskState(int8_t taskId) const {
    if (!isValidTask(taskId)) return TaskState::Invalid;
    return ::extractState(tasks[taskId]);
}

uint32_t Arda::getTaskRunCount(int8_t taskId) const {
    if (!isValidTask(taskId)) {
        return 0;
    }
    return tasks[taskId].runCount;
}

uint32_t Arda::getTaskInterval(int8_t taskId) const {
    if (!isValidTask(taskId)) {
        return 0;
    }
    return tasks[taskId].interval;
}

uint32_t Arda::getTaskTimeout(int8_t taskId) const {
    if (!isValidTask(taskId)) {
        return 0;
    }
    return tasks[taskId].timeout;
}

int8_t Arda::getCurrentTask() const {
    return currentTask;
}

bool Arda::isValidTask(int8_t taskId) const {
    return taskId >= 0 && taskId < taskCount && !isDeleted(tasks[taskId]);
}

bool Arda::isTaskYielded(int8_t taskId) const {
    if (!isValidTask(taskId)) return false;
    return checkInYield(tasks[taskId]);
}

#ifdef ARDA_NO_NAMES
int8_t Arda::findTaskByName(const char* name) const {
    (void)name;
    return -1;  // No names to search when ARDA_NO_NAMES is defined
}
#else
int8_t Arda::findTaskByName(const char* name) const {
    if (name == nullptr) return -1;

    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i]) && nameEquals(tasks[i].name, name)) {
            return i;
        }
    }
    return -1;
}
#endif

int8_t Arda::getValidTaskIds(int8_t* outIds, int8_t maxCount) const {
    int8_t count = 0;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i])) {
            if (outIds != nullptr && count < maxCount) {
                outIds[count] = i;
            }
            count++;
        }
    }
    return count;
}

bool Arda::hasTaskSetup(int8_t taskId) const {
    if (!isValidTask(taskId)) return false;
    return tasks[taskId].setup != nullptr;
}

bool Arda::hasTaskLoop(int8_t taskId) const {
    if (!isValidTask(taskId)) return false;
    return tasks[taskId].loop != nullptr;
}

bool Arda::hasTaskTeardown(int8_t taskId) const {
    if (!isValidTask(taskId)) return false;
    return tasks[taskId].teardown != nullptr;
}

// =============================================================================
// Callback configuration
// =============================================================================

void Arda::setTimeoutCallback(TimeoutCallback callback) {
    timeoutCallback = callback;
}

void Arda::setStartFailureCallback(StartFailureCallback callback) {
    startFailureCallback = callback;
}

void Arda::setTraceCallback(TraceCallback callback) {
    traceCallback = callback;
}

// =============================================================================
// Utility
// =============================================================================

void Arda::yield() {
    // Give other tasks a chance to run while this task waits.
    // Skips the currently executing task to prevent infinite recursion,
    // but allows other ready tasks to execute.
    // ranThisCycle prevents tasks from running twice in the same cycle.
    if (currentTask >= 0) {
        int8_t yieldingTask = currentTask;
        updateInYield(tasks[yieldingTask], true);  // Mark as yielding (prevents deletion)
        bool wasInRun = (flags_ & FLAG_IN_RUN) != 0;  // Save original state
        flags_ &= ~FLAG_IN_RUN;  // Temporarily allow re-entry
        runInternal(yieldingTask);  // Run others, skip current
        if (wasInRun) flags_ |= FLAG_IN_RUN; else flags_ &= ~FLAG_IN_RUN;  // Restore original state
        updateInYield(tasks[yieldingTask], false);  // No longer yielding
    }
}

uint32_t Arda::uptime() const {
    if (!(flags_ & FLAG_BEGUN)) {
        return millis();  // Return raw millis() if scheduler not started
    }
    return millis() - startTime;
}

// =============================================================================
// Error handling
// =============================================================================

ArdaError Arda::getError() const {
    return error_;
}

void Arda::clearError() {
    error_ = ArdaError::Ok;
}

const char* Arda::errorString(ArdaError error) {
    switch (error) {
        case ArdaError::Ok:            return "OK";
        case ArdaError::NullName:      return "NULL_NAME";
        case ArdaError::EmptyName:     return "EMPTY_NAME";
        case ArdaError::NameTooLong:   return "NAME_TOO_LONG";
        case ArdaError::DuplicateName: return "DUPLICATE_NAME";
        case ArdaError::MaxTasks:      return "MAX_TASKS";
        case ArdaError::InvalidId:     return "INVALID_ID";
        case ArdaError::WrongState:    return "WRONG_STATE";
        case ArdaError::TaskExecuting: return "TASK_EXECUTING";
        case ArdaError::TaskYielded:   return "TASK_YIELDED";
        case ArdaError::AlreadyBegun:  return "ALREADY_BEGUN";
        case ArdaError::CallbackDepth: return "CALLBACK_DEPTH";
        case ArdaError::StateChanged:  return "STATE_CHANGED";
        case ArdaError::InCallback:    return "IN_CALLBACK";
        case ArdaError::NotSupported:  return "NOT_SUPPORTED";
        default:                       return "UNKNOWN";
    }
}

// =============================================================================
// Private helpers
// =============================================================================

int8_t Arda::allocateSlot() {
    // O(1) allocation from free list or new slot
    if (freeListHead != -1) {
        // Reuse a deleted slot from the free list
        int8_t slot = freeListHead;
        freeListHead = tasks[slot].nextFree;  // Advance head to next free slot
        return slot;
    }
    // No free slots, allocate a new one
    if (taskCount >= ARDA_MAX_TASKS) {
        return -1;  // Full
    }
    return taskCount++;
}

void Arda::freeSlot(int8_t slot) {
    // Validate slot is in valid range to prevent free list corruption
    if (slot < 0 || slot >= ARDA_MAX_TASKS) {
        return;
    }
    // O(1) deallocation - prepend to free list
    tasks[slot].nextFree = freeListHead;
    freeListHead = slot;
}

void Arda::emitTrace(int8_t taskId, TraceEvent event) {
    if (traceCallback) {
        callbackDepth++;
        traceCallback(taskId, event);
        callbackDepth--;
    }
}

#ifndef ARDA_NO_NAMES
bool Arda::nameEquals(const char* a, const char* b) {
#ifdef ARDA_CASE_INSENSITIVE_NAMES
    // Case-insensitive comparison (ASCII letters A-Z only; extended ASCII/UTF-8 compared as-is)
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        // Convert ASCII uppercase to lowercase (A-Z -> a-z)
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == *b;  // Both must be at null terminator
#else
    // Case-sensitive comparison (default)
    return strncmp(a, b, ARDA_MAX_NAME_LEN) == 0;
#endif
}
#endif

// =============================================================================
// Static helpers
// =============================================================================

// Helper to check if task slot is deleted
static inline bool isDeleted(const Task& task) {
#ifdef ARDA_NO_NAMES
    // Use state value 3 (unused) as deletion marker
    return (task.flags & ARDA_TASK_STATE_MASK) == ARDA_TASK_DELETED_STATE;
#else
    return task.name[0] == '\0';
#endif
}

// Helper to mark a task slot as deleted
static inline void markDeleted(Task& task) {
#ifdef ARDA_NO_NAMES
    // Set state bits to 3 (deleted), preserve other bits (though they don't matter for deleted tasks)
    task.flags = (task.flags & ~ARDA_TASK_STATE_MASK) | ARDA_TASK_DELETED_STATE;
#else
    task.name[0] = '\0';
#endif
}

// Helper to clear the deleted marker (when allocating a slot)
static inline void clearDeleted(Task& task) {
#ifdef ARDA_NO_NAMES
    // Set state to Stopped (0) - clears the deleted state
    task.flags &= ~ARDA_TASK_STATE_MASK;
#else
    // Name will be set by createTask, no action needed
    (void)task;
#endif
}

// Task flag helpers - access packed state and flags
static inline TaskState extractState(const Task& task) {
    return static_cast<TaskState>(task.flags & ARDA_TASK_STATE_MASK);
}
static inline void updateState(Task& task, TaskState state) {
    task.flags = (task.flags & ~ARDA_TASK_STATE_MASK) | static_cast<uint8_t>(state);
}
static inline bool checkRanThisCycle(const Task& task) {
    return (task.flags & ARDA_TASK_RAN_BIT) != 0;
}
static inline void updateRanThisCycle(Task& task, bool val) {
    if (val) task.flags |= ARDA_TASK_RAN_BIT;
    else task.flags &= ~ARDA_TASK_RAN_BIT;
}
static inline bool checkInYield(const Task& task) {
    return (task.flags & ARDA_TASK_YIELD_BIT) != 0;
}
static inline void updateInYield(Task& task, bool val) {
    if (val) task.flags |= ARDA_TASK_YIELD_BIT;
    else task.flags &= ~ARDA_TASK_YIELD_BIT;
}
#ifndef ARDA_NO_PRIORITY
static inline uint8_t extractPriority(const Task& task) {
    return (task.flags & ARDA_TASK_PRIORITY_MASK) >> ARDA_TASK_PRIORITY_SHIFT;
}
static inline void updatePriority(Task& task, uint8_t priority) {
    if (priority > 15) priority = 15;
    task.flags = (task.flags & ~ARDA_TASK_PRIORITY_MASK) | (priority << ARDA_TASK_PRIORITY_SHIFT);
}
#endif
