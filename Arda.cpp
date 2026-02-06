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
#ifdef ARDA_YIELD
static inline bool checkInYield(const Task& task);
static inline void updateInYield(Task& task, bool val);
#endif
#ifndef ARDA_NO_PRIORITY
static inline uint8_t extractPriority(const Task& task);
static inline void updatePriority(Task& task, uint8_t priority);
#endif

#ifdef ARDA_SHELL_ACTIVE
void ardaShellLoop_();  // Forward decl for constructor (non-static, declared friend in Arda class)
#endif

// =============================================================================
// Task Recovery (Timer2 soft watchdog) - AVR only
// =============================================================================

#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL

// Timer2 overflow calculation constants
// Timer2 with /1024 prescaler overflows every (256 * 1024 / F_CPU) seconds
// ARDA_TICKS_PER_MS = F_CPU / 1000 = ticks per millisecond
// ARDA_MAX_SAFE_TIMEOUT_MS = max timeout before uint32 multiplication overflow
static const uint32_t ARDA_TICKS_PER_MS = (F_CPU / 1000UL);
static const uint32_t ARDA_MAX_SAFE_TIMEOUT_MS = ((UINT32_MAX / ARDA_TICKS_PER_MS) - 1UL);

// Static member definitions
jmp_buf Arda::recoveryJumpBuf_;
volatile int8_t Arda::recoveryTask_ = -1;
volatile bool Arda::recoveryFired_ = false;
volatile uint16_t Arda::recoveryOverflowsRemaining_ = 0;
volatile bool Arda::recoveryInCallback_ = false;
volatile bool Arda::recoveryArmed_ = false;
volatile bool Arda::recoveryEnabled_ = true;      // Enabled by default

// Initialize Timer2 for task recovery (called in begin())
void Arda::initTaskRecovery() {
    TCCR2A = 0;                     // Normal mode
    TCCR2B = 0;                     // Timer stopped
    TIMSK2 = 0;                     // Interrupts disabled until armed
}

// Arm recovery timer (called AFTER setjmp, before task->loop())
void Arda::armRecoveryTimer(int8_t taskId, uint32_t timeoutMs) {
    uint8_t sreg = SREG;
    cli();                          // Atomic setup

    // Don't arm if globally disabled OR already armed
    if (!recoveryEnabled_ || recoveryArmed_) {
        SREG = sreg;
        return;
    }

    recoveryTask_ = taskId;
    recoveryFired_ = false;

    // Timer2 with /1024 prescaler overflows every (256 * 1024 / F_CPU) seconds
    // Overflow period in ms = 262144000 / F_CPU
    // At 16MHz: 16.384ms, at 8MHz: 32.768ms, at 20MHz: 13.1ms
    //
    // Formula: overflows = timeoutMs * (F_CPU / 1000) / 262144
    uint16_t overflows;
    if (timeoutMs > ARDA_MAX_SAFE_TIMEOUT_MS) {
        overflows = UINT16_MAX;  // Saturate to max (~17 min at 16MHz)
    } else {
        // Round to nearest (add half divisor before division)
        overflows = (uint16_t)(((timeoutMs * ARDA_TICKS_PER_MS) + 131072UL) / 262144UL);
    }
    if (overflows == 0) overflows = 1;  // Minimum 1 overflow

    recoveryOverflowsRemaining_ = overflows;
    TCCR2A = 0;                     // Ensure normal mode (not PWM)
    TCCR2B = 0;                     // Stop timer first
    TCNT2 = 0;
    TIFR2 = (1 << TOV2);            // Clear pending overflow flag
    recoveryArmed_ = true;          // Enable ISR logic
    TIMSK2 = (1 << TOIE2);          // Enable overflow interrupt
    TCCR2B = (1 << CS22) | (1 << CS21) | (1 << CS20);  // /1024 prescaler, start

    SREG = sreg;                    // Restore interrupt state
}

// Disarm recovery timer (called after task returns normally)
void Arda::disarmRecoveryTimer() {
    uint8_t sreg = SREG;
    cli();                          // Atomic disarm

    recoveryArmed_ = false;         // Disable ISR logic first
    TCCR2B = 0;                     // Stop timer
    TIMSK2 = 0;                     // Disable interrupt
    TIFR2 = (1 << TOV2);            // Clear any pending flag
    recoveryTask_ = -1;

    SREG = sreg;
}

// Timer2 overflow ISR
ISR(TIMER2_OVF_vect) {
    if (!Arda::recoveryArmed_ || !Arda::recoveryEnabled_) return;  // Guard against spurious interrupts

    if (--Arda::recoveryOverflowsRemaining_ == 0) {
        // Timeout reached - stop timer and abort
        Arda::recoveryArmed_ = false;
        TCCR2B = 0;
        TIMSK2 = 0;
        Arda::recoveryFired_ = true;

        // longjmp restores stack but NOT SREG on AVR - caller must sei()
        longjmp(Arda::recoveryJumpBuf_, 1);
    }
    // Otherwise, keep counting overflows
}

#endif // ARDA_TASK_RECOVERY_IMPL
#endif // ARDA_TASK_RECOVERY

// =============================================================================
// Constructor
// =============================================================================

Arda::Arda() {
#ifdef ARDA_SHELL_ACTIVE
    // Only initialize shell on the global OS instance
    // (shell callback is hard-wired to OS, so local instances shouldn't have it)
    bool isGlobalInstance = (this == &OS);

    if (isGlobalInstance) {
        // Initialize shell task in slot 0
        initShell_();
        // Initialize shell-specific members for global instance
        shellStream_ = &Serial;
    } else {
        // Non-global instance - no shell task
        taskCount = 0;
        activeCount = 0;
        shellStream_ = nullptr;
        shellDeleted_ = true;  // No shell on local instances
    }
    shellBufIdx_ = 0;
    shellBusy_ = false;
    pendingSelfDelete_ = -1;
#ifndef ARDA_NO_SHELL_ECHO
    shellEcho_ = true;  // Echo on by default
#endif

    // Initialize remaining slots
    for (int8_t i = (isGlobalInstance ? 1 : 0); i < ARDA_MAX_TASKS; i++) {
#else
    // No shell - standard initialization
    taskCount = 0;
    activeCount = 0;
    for (int8_t i = 0; i < ARDA_MAX_TASKS; i++) {
#endif
        tasks[i].setup = nullptr;
        tasks[i].loop = nullptr;
        tasks[i].teardown = nullptr;
#ifdef ARDA_TASK_RECOVERY
        tasks[i].recover = nullptr;
#endif
        tasks[i].interval = 0;
        tasks[i].lastRun = 0;
        tasks[i].runCount = 0;
#ifdef ARDA_TASK_RECOVERY
        tasks[i].timeout = 0;
#endif
#ifdef ARDA_NO_NAMES
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
        tasks[i].flags = 0;
  #endif
#endif
    }

    // Initialize other members (common to both paths)
    startTime = 0;
    currentTask = -1;
    freeListHead = -1;
    callbackDepth = 0;
    flags_ = 0;
    error_ = ArdaError::Ok;
#ifdef ARDA_TASK_RECOVERY
    timeoutCallback = nullptr;
#if !ARDA_TASK_RECOVERY_IMPL
    recoveryEnabled_ = true;  // Enabled by default on non-AVR
#endif
#endif
    startFailureCallback = nullptr;
    traceCallback = nullptr;
}

#ifdef ARDA_SHELL_ACTIVE
// Initialize shell task in slot 0 (called from constructor and reset)
void Arda::initShell_() {
    taskCount = 1;
    activeCount = 1;
#ifndef ARDA_NO_NAMES
    strncpy(tasks[0].name, "sh", ARDA_MAX_NAME_LEN - 1);
    tasks[0].name[ARDA_MAX_NAME_LEN - 1] = '\0';
#endif
    tasks[0].setup = nullptr;
    tasks[0].loop = ardaShellLoop_;
    tasks[0].teardown = nullptr;
#ifdef ARDA_TASK_RECOVERY
    tasks[0].recover = nullptr;
#endif
    tasks[0].interval = 0;
    tasks[0].lastRun = 0;
    tasks[0].runCount = 0;
#ifdef ARDA_TASK_RECOVERY
    tasks[0].timeout = 0;
#endif
#ifndef ARDA_NO_PRIORITY
    tasks[0].flags = (ARDA_DEFAULT_PRIORITY << ARDA_TASK_PRIORITY_SHIFT);
#else
    tasks[0].flags = 0;
#endif
#ifndef ARDA_SHELL_MANUAL_START
    // Mark shell for auto-start in begin() (unless manual start mode)
    tasks[0].flags |= ARDA_TASK_RAN_BIT;
#endif
    shellDeleted_ = false;
}
#endif

// =============================================================================
// Scheduler control
// =============================================================================

int8_t Arda::begin() {
    // Guard against multiple begin() calls - use reset() first if restarting
    if (flags_ & FLAG_BEGUN) {
        error_ = ArdaError::AlreadyBegun;
        return -1;
    }

    flags_ |= FLAG_BEGUN;
    startTime = millis();

#ifdef ARDA_WATCHDOG
    wdt_enable(WDTO_8S);
#endif

#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL
    initTaskRecovery();
#endif
#endif

    int8_t started = 0;
    ArdaError firstFailure = ArdaError::Ok;  // Track first failure to avoid masking by later successes

    // Snapshot valid task IDs to protect against callbacks modifying the task array.
    // Without this, a setup() callback could create/delete tasks and corrupt iteration.
    int8_t snapshot[ARDA_MAX_TASKS];
    int8_t snapshotCount = 0;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i])) {
#if defined(ARDA_SHELL_ACTIVE) && defined(ARDA_SHELL_MANUAL_START)
            // Skip shell task when manual start is enabled
            if (i == ARDA_SHELL_TASK_ID) continue;
#endif
            snapshot[snapshotCount++] = i;
        }
    }

    // Start all registered tasks from the snapshot
    // Set FLAG_IN_BEGIN so yield() doesn't clear autoStart bits for tasks not yet processed
    flags_ |= FLAG_IN_BEGIN;
    for (int8_t j = 0; j < snapshotCount; j++) {
        int8_t i = snapshot[j];
        if (!isValidTask(i)) continue;  // Task was deleted during iteration

        // Skip tasks that are already Running or Paused (user started them before begin())
        TaskState state = extractState(tasks[i]);
        if (state == TaskState::Running || state == TaskState::Paused) {
            tasks[i].flags &= ~ARDA_TASK_RAN_BIT;  // Clear stale autoStart bit
            started++;  // Count as already started
            continue;
        }

        // Skip tasks created with autoStart=false (bit 2 not set)
        // Bit 2 is reused: before begin() = shouldAutoStart, after = ranThisCycle
        if (!(tasks[i].flags & ARDA_TASK_RAN_BIT)) {
            continue;
        }
        tasks[i].flags &= ~ARDA_TASK_RAN_BIT;  // Clear the bit (will be ranThisCycle now)

        StartResult result = startTask(i);
        if (result == StartResult::Success) {
            started++;
        } else {
            // Capture error before callback (callback may mutate scheduler state)
            ArdaError startError = error_;
            if (firstFailure == ArdaError::Ok) {
                firstFailure = startError;  // Remember first failure
            }
            // Task failed to start - notify via callback if registered
            if (startFailureCallback != nullptr && callbackDepth < ARDA_MAX_CALLBACK_DEPTH) {
                callbackDepth++;
                startFailureCallback(i, startError);
                callbackDepth--;
            }
        }
    }
    flags_ &= ~FLAG_IN_BEGIN;

    // Report first failure error, or Ok if all succeeded
    if (firstFailure != ArdaError::Ok) {
        error_ = firstFailure;
    } else {
        error_ = ArdaError::Ok;
    }
    return started;
}

bool Arda::run() {
    if (!(flags_ & FLAG_BEGUN)) {
        error_ = ArdaError::WrongState;
        return false;
    }
    if (flags_ & FLAG_IN_RUN) {
        error_ = ArdaError::InCallback;  // Already in a run cycle (reentrancy)
        return false;
    }
    runInternal(-1);  // Run all tasks
    // Preserve any error set during task execution (e.g., by task code calling
    // scheduler APIs). Only set Ok if no error was set during this cycle.
    // Note: run() itself succeeded - errors here are from task callbacks.
    return true;
}

void Arda::runInternal(int8_t skipTask) {
    // Reentrancy guard - prevent recursive calls
    if (flags_ & FLAG_IN_RUN) return;
    flags_ |= FLAG_IN_RUN;

#ifdef ARDA_WATCHDOG
    // Reset watchdog at start of each run() cycle - ensures idle scheduler
    // (no runnable tasks) doesn't trigger reset. Individual task loops that
    // block >8s will still trigger the watchdog.
    wdt_reset();
#endif

    // Snapshot valid task IDs to protect against callbacks modifying the task array.
    // Without this, a callback could delete/create tasks and corrupt iteration.
    int8_t snapshot[ARDA_MAX_TASKS];
    int8_t snapshotCount = 0;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i])) {
            snapshot[snapshotCount++] = i;
        }
    }

    // Reset ranThisCycle flags at start of each run() cycle (skipTask < 0).
    // When called from yield() (skipTask >= 0), preserve ranThisCycle flags to
    // prevent tasks from running multiple times in the same top-level cycle.
    // Note: yield() handles clearing stale flags from previous cycles when called
    // outside run() - see yield() implementation.
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
            if (tasks[i].interval != 0 && (millis() - tasks[i].lastRun < tasks[i].interval)) continue;

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

            emitTrace(i, TraceEvent::TaskLoopBegin);
            uint32_t execStart = millis();

            // ARDA_WATCHDOG and ARDA_TASK_RECOVERY can be enabled together
#ifdef ARDA_WATCHDOG
            wdt_reset();
#endif

#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL
            bool wasAborted = false;
            uint32_t cachedTimeout = tasks[i].timeout;  // Cache before potential invalidation
            bool recEnabled = recoveryEnabled_;  // Cache volatile read once per task

            // Mark as run BEFORE any execution - must happen for ALL tasks (even timeout=0)
            updateRanThisCycle(tasks[i], true);

            // Check global enable AND per-task timeout
            if (recEnabled && cachedTimeout > 0) {
                // Save state that may be corrupted by longjmp
                uint8_t savedCallbackDepth = callbackDepth;

                if (setjmp(recoveryJumpBuf_) == 0) {
                    // Normal path - arm timer AFTER setjmp establishes jump point
                    recoveryInCallback_ = false;
                    armRecoveryTimer(i, cachedTimeout);
                    callbackDepth++;  // Track callback depth for loop()
                    tasks[i].loop();
                    callbackDepth--;
                    disarmRecoveryTimer();
                } else {
                    // longjmp landed here - INTERRUPTS ARE DISABLED!
                    disarmRecoveryTimer();
                    sei();  // Re-enable interrupts (longjmp doesn't restore SREG on AVR)
                    callbackDepth = savedCallbackDepth;  // Restore corrupted depth

                    // Re-validate task - could have been invalidated before timeout fired
                    if (!isValidTask(i)) {
                        recoveryInCallback_ = false;
                        wasAborted = true;
                    } else if (!recoveryInCallback_) {
                        // Task itself timed out - try recover()
                        wasAborted = true;
                        error_ = ArdaError::TaskAborted;
                        emitTrace(i, TraceEvent::TaskAborted);

#ifdef ARDA_SHELL_ACTIVE
                        // Clear partial shell command buffer if shell task was aborted
                        if (i == ARDA_SHELL_TASK_ID) {
                            shellBufIdx_ = 0;
                        }
#endif

                        if (tasks[i].recover) {
                            // Arm timer again for recover() - use current timeout
                            // (task may have adjusted it mid-loop via setTaskTimeout)
                            if (setjmp(recoveryJumpBuf_) == 0) {
                                recoveryInCallback_ = true;
                                armRecoveryTimer(i, tasks[i].timeout);
                                callbackDepth++;  // Track callback depth for recover()
                                tasks[i].recover();
                                callbackDepth--;
                                disarmRecoveryTimer();
                                recoveryInCallback_ = false;
                            } else {
                                // recover() also timed out - longjmp landed here
                                disarmRecoveryTimer();
                                sei();
                                callbackDepth--;  // Undo recover() increment
                                recoveryInCallback_ = false;
                                emitTrace(i, TraceEvent::RecoverAborted);
                            }
                        }
                    }
                }
            } else {
                // No timeout set - run without recovery protection
                // (updateRanThisCycle already called above for ALL tasks)
                callbackDepth++;
                tasks[i].loop();
                callbackDepth--;
            }
#else
            // Hardware doesn't support recovery - run task normally
            updateRanThisCycle(tasks[i], true);
            callbackDepth++;
            tasks[i].loop();
            callbackDepth--;
#endif
#else
            // ARDA_TASK_RECOVERY not enabled - original code
            updateRanThisCycle(tasks[i], true);
            callbackDepth++;
            tasks[i].loop();
            callbackDepth--;
#endif

            uint32_t execDuration = millis() - execStart;
            emitTrace(i, TraceEvent::TaskLoopEnd);

            currentTask = prevTask;

            // Update run statistics (happens for both normal and aborted tasks)
#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL
            if (isValidTask(i)) {
#endif
#endif
                if (++tasks[i].runCount == 0) tasks[i].runCount = 1;  // Skip 0 on overflow
                tasks[i].lastRun = execStart;
#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL
            }
#endif
#endif

#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL
            // Skip soft timeout callback if task was hard-aborted (already notified via trace)
            // Use current timeout if task is still valid (may have been adjusted mid-loop)
            if (wasAborted) {
                execDuration = isValidTask(i) ? tasks[i].timeout : cachedTimeout;
            }
            // Only fire timeout callback if recovery enabled (re-read in case task disabled it mid-loop), NOT aborted
            // Use current tasks[i].timeout (not cachedTimeout) in case task adjusted it mid-loop
            uint32_t finalTimeout = tasks[i].timeout;
            if (recoveryEnabled_ && !wasAborted && finalTimeout > 0 && execDuration > finalTimeout
                && timeoutCallback && callbackDepth < ARDA_MAX_CALLBACK_DEPTH) {
                callbackDepth++;
                timeoutCallback(i, execDuration);
                callbackDepth--;
            }
#else
            // Non-AVR: software timeout callback only (no hardware abort)
            // Check recoveryEnabled_ to honor setTaskRecoveryEnabled()
            if (recoveryEnabled_ && tasks[i].timeout > 0 && execDuration > tasks[i].timeout
                && timeoutCallback != nullptr && callbackDepth < ARDA_MAX_CALLBACK_DEPTH) {
                callbackDepth++;
                timeoutCallback(i, execDuration);
                callbackDepth--;
            }
#endif
#endif

#ifdef ARDA_SHELL_ACTIVE
            // Handle deferred self-deletion (e.g., shell killing itself with 'k 0')
            if (pendingSelfDelete_ == i) {
                pendingSelfDelete_ = -1;
                if (deleteTask(i)) {  // Safe now since currentTask has been restored
                    if (i == ARDA_SHELL_TASK_ID) shellDeleted_ = true;
                }
                // If deletion fails (e.g., teardown restarted task), shellDeleted_ stays false
            }
#endif
        }
    }
#else
    // Original array-order scheduling (when ARDA_NO_PRIORITY is defined)
    for (int8_t j = 0; j < snapshotCount; j++) {
        int8_t i = snapshot[j];
        if (i == skipTask) continue;  // Skip specified task (for yield)
        if (!isValidTask(i)) continue;  // Task was deleted during iteration
        if (extractState(tasks[i]) != TaskState::Running) continue;
        if (tasks[i].loop == nullptr) continue;
        if (checkRanThisCycle(tasks[i])) continue;  // Already ran this cycle (prevents double execution from yield)

        // Check if it's time to run this task
        if (tasks[i].interval == 0 || (millis() - tasks[i].lastRun >= tasks[i].interval)) {
            // Guard against excessive callback nesting (prevents stack overflow)
            if (callbackDepth >= ARDA_MAX_CALLBACK_DEPTH) {
                continue;  // Skip this task for now, will try again next cycle
            }
            int8_t prevTask = currentTask;
            currentTask = i;

            emitTrace(i, TraceEvent::TaskLoopBegin);
            uint32_t execStart = millis();

            // ARDA_WATCHDOG and ARDA_TASK_RECOVERY can be enabled together
#ifdef ARDA_WATCHDOG
            wdt_reset();
#endif

#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL
            bool wasAborted = false;
            uint32_t cachedTimeout = tasks[i].timeout;  // Cache before potential invalidation
            bool recEnabled = recoveryEnabled_;  // Cache volatile read once per task

            // Mark as run BEFORE any execution - must happen for ALL tasks (even timeout=0)
            updateRanThisCycle(tasks[i], true);

            // Check global enable AND per-task timeout
            if (recEnabled && cachedTimeout > 0) {
                // Save state that may be corrupted by longjmp
                uint8_t savedCallbackDepth = callbackDepth;

                if (setjmp(recoveryJumpBuf_) == 0) {
                    // Normal path - arm timer AFTER setjmp establishes jump point
                    recoveryInCallback_ = false;
                    armRecoveryTimer(i, cachedTimeout);
                    callbackDepth++;  // Track callback depth for loop()
                    tasks[i].loop();
                    callbackDepth--;
                    disarmRecoveryTimer();
                } else {
                    // longjmp landed here - INTERRUPTS ARE DISABLED!
                    disarmRecoveryTimer();
                    sei();  // Re-enable interrupts (longjmp doesn't restore SREG on AVR)
                    callbackDepth = savedCallbackDepth;  // Restore corrupted depth

                    // Re-validate task - could have been invalidated before timeout fired
                    if (!isValidTask(i)) {
                        recoveryInCallback_ = false;
                        wasAborted = true;
                    } else if (!recoveryInCallback_) {
                        // Task itself timed out - try recover()
                        wasAborted = true;
                        error_ = ArdaError::TaskAborted;
                        emitTrace(i, TraceEvent::TaskAborted);

#ifdef ARDA_SHELL_ACTIVE
                        // Clear partial shell command buffer if shell task was aborted
                        if (i == ARDA_SHELL_TASK_ID) {
                            shellBufIdx_ = 0;
                        }
#endif

                        if (tasks[i].recover) {
                            // Arm timer again for recover() - use current timeout
                            // (task may have adjusted it mid-loop via setTaskTimeout)
                            if (setjmp(recoveryJumpBuf_) == 0) {
                                recoveryInCallback_ = true;
                                armRecoveryTimer(i, tasks[i].timeout);
                                callbackDepth++;  // Track callback depth for recover()
                                tasks[i].recover();
                                callbackDepth--;
                                disarmRecoveryTimer();
                                recoveryInCallback_ = false;
                            } else {
                                // recover() also timed out - longjmp landed here
                                disarmRecoveryTimer();
                                sei();
                                callbackDepth--;  // Undo recover() increment
                                recoveryInCallback_ = false;
                                emitTrace(i, TraceEvent::RecoverAborted);
                            }
                        }
                    }
                }
            } else {
                // No timeout set - run without recovery protection
                // (updateRanThisCycle already called above for ALL tasks)
                callbackDepth++;
                tasks[i].loop();
                callbackDepth--;
            }
#else
            // Hardware doesn't support recovery - run task normally
            updateRanThisCycle(tasks[i], true);
            callbackDepth++;
            tasks[i].loop();
            callbackDepth--;
#endif
#else
            // ARDA_TASK_RECOVERY not enabled - original code
            updateRanThisCycle(tasks[i], true);
            callbackDepth++;
            tasks[i].loop();
            callbackDepth--;
#endif

            uint32_t execDuration = millis() - execStart;
            emitTrace(i, TraceEvent::TaskLoopEnd);

            currentTask = prevTask;

            // Update run statistics (happens for both normal and aborted tasks)
#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL
            if (isValidTask(i)) {
#endif
#endif
                if (++tasks[i].runCount == 0) tasks[i].runCount = 1;  // Skip 0 on overflow
                tasks[i].lastRun = execStart;
#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL
            }
#endif
#endif

#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL
            // Skip soft timeout callback if task was hard-aborted (already notified via trace)
            // Use current timeout if task is still valid (may have been adjusted mid-loop)
            if (wasAborted) {
                execDuration = isValidTask(i) ? tasks[i].timeout : cachedTimeout;
            }
            // Only fire timeout callback if recovery enabled (re-read in case task disabled it mid-loop), NOT aborted
            // Use current tasks[i].timeout (not cachedTimeout) in case task adjusted it mid-loop
            uint32_t finalTimeout = tasks[i].timeout;
            if (recoveryEnabled_ && !wasAborted && finalTimeout > 0 && execDuration > finalTimeout
                && timeoutCallback && callbackDepth < ARDA_MAX_CALLBACK_DEPTH) {
                callbackDepth++;
                timeoutCallback(i, execDuration);
                callbackDepth--;
            }
#else
            // Non-AVR: software timeout callback only (no hardware abort)
            // Check recoveryEnabled_ to honor setTaskRecoveryEnabled()
            if (recoveryEnabled_ && tasks[i].timeout > 0 && execDuration > tasks[i].timeout
                && timeoutCallback != nullptr && callbackDepth < ARDA_MAX_CALLBACK_DEPTH) {
                callbackDepth++;
                timeoutCallback(i, execDuration);
                callbackDepth--;
            }
#endif
#endif

#ifdef ARDA_SHELL_ACTIVE
            // Handle deferred self-deletion (e.g., shell killing itself with 'k 0')
            if (pendingSelfDelete_ == i) {
                pendingSelfDelete_ = -1;
                if (deleteTask(i)) {  // Safe now since currentTask has been restored
                    if (i == ARDA_SHELL_TASK_ID) shellDeleted_ = true;
                }
                // If deletion fails (e.g., teardown restarted task), shellDeleted_ stays false
            }
#endif
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

#ifdef ARDA_WATCHDOG
    wdt_disable();
#endif

#ifdef ARDA_TASK_RECOVERY
#if ARDA_TASK_RECOVERY_IMPL
    // Disable Timer2 before clearing state - ISR could longjmp to invalid target
    TCCR2B = 0;                     // Stop timer
    TIMSK2 = 0;                     // Disable interrupt
    TIFR2 = (1 << TOV2);            // Clear pending flag
    recoveryArmed_ = false;
    recoveryEnabled_ = true;        // Restore default enabled state
#else
    recoveryEnabled_ = true;        // Restore default enabled state (non-AVR)
#endif
#endif

    bool allTeardownsRan = true;
    ArdaError teardownError = ArdaError::Ok;

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
        // Track any stop failure: TeardownSkipped, TeardownChangedState, or Failed
        if (result != StopResult::Success) {
            allTeardownsRan = false;
            if (teardownError == ArdaError::Ok) {
                teardownError = error_;  // Capture first error
            }
        }
        // Continue stopping other tasks regardless of result
    }

    // Clear all task slots
    for (int8_t i = 0; i < ARDA_MAX_TASKS; i++) {
        tasks[i].setup = nullptr;
        tasks[i].loop = nullptr;
        tasks[i].teardown = nullptr;
#ifdef ARDA_TASK_RECOVERY
        tasks[i].recover = nullptr;
#endif
        tasks[i].interval = 0;
        tasks[i].lastRun = 0;
        tasks[i].runCount = 0;    // Union member; nextFree shares this memory
#ifdef ARDA_TASK_RECOVERY
        tasks[i].timeout = 0;
#endif
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

#ifdef ARDA_SHELL_ACTIVE
    pendingSelfDelete_ = -1;
    shellBufIdx_ = 0;   // Clear partial command buffer
    shellBusy_ = false; // Clear busy flag
    // Reinitialize shell for global OS instance (restores shell to initial state)
    if (this == &OS) {
        initShell_();
    } else {
        shellDeleted_ = true;
    }
#endif

    // Optionally clear user callbacks for a complete clean slate
    if (!preserveCallbacks) {
#ifdef ARDA_TASK_RECOVERY
        timeoutCallback = nullptr;
#endif
        startFailureCallback = nullptr;
        traceCallback = nullptr;
    }

    // Set final error state based on whether all teardowns ran
    if (allTeardownsRan) {
        error_ = ArdaError::Ok;
    } else {
        error_ = teardownError;  // Report actual error, not always CallbackDepth
    }

    return allTeardownsRan;
}

bool Arda::hasBegun() const {
    return (flags_ & FLAG_BEGUN) != 0;
}

// =============================================================================
// Task creation and deletion
// =============================================================================

// Common task field initialization (called after slot allocation and name setup)
int8_t Arda::initTaskFields_(int8_t id, TaskCallback setup, TaskCallback loop,
                              uint32_t intervalMs, TaskCallback teardown, bool autoStart) {
    tasks[id].setup = setup;
    tasks[id].loop = loop;
    tasks[id].teardown = teardown;
#ifdef ARDA_TASK_RECOVERY
    tasks[id].recover = nullptr;
#endif
    tasks[id].interval = intervalMs;
    tasks[id].lastRun = 0;
    tasks[id].runCount = 0;    // Union member; nextFree shares this memory
#ifdef ARDA_TASK_RECOVERY
    tasks[id].timeout = 0;
#endif
#ifndef ARDA_NO_PRIORITY
    tasks[id].flags = ARDA_DEFAULT_PRIORITY << ARDA_TASK_PRIORITY_SHIFT;
#else
    tasks[id].flags = 0;       // state=Stopped, ranThisCycle=false, inYield=false
#endif
    // Before begin(): use bit 2 to remember autoStart setting
    // After begin(): this bit becomes ranThisCycle (reset each cycle anyway)
    if (!(flags_ & FLAG_BEGUN) && autoStart) {
        tasks[id].flags |= ARDA_TASK_RAN_BIT;  // Reuse as "shouldAutoStart"
    }

    activeCount++;

    // Auto-start if begin() was already called and autoStart is enabled
    if ((flags_ & FLAG_BEGUN) && autoStart) {
        if (startTask(id) != StartResult::Success) {
            // Auto-start failed - delete the task and return -1.
            // Error is already set by startTask(). Use autoStart=false to handle failures manually.
            ArdaError savedError = error_;
            deleteTask(id);
            error_ = savedError;
            return -1;
        }
    }

    error_ = ArdaError::Ok;
    return id;
}

#ifdef ARDA_NO_NAMES

// Nameless createTask - primary implementation when ARDA_NO_NAMES is defined
int8_t Arda::createTask(TaskCallback setup, TaskCallback loop,
                        uint32_t intervalMs, TaskCallback teardown, bool autoStart) {
    int8_t id = allocateSlot();
    if (id == -1) {
        error_ = ArdaError::MaxTasks;
        return -1;
    }
    clearDeleted(tasks[id]);
    return initTaskFields_(id, setup, loop, intervalMs, teardown, autoStart);
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

    int8_t id = allocateSlot();
    if (id == -1) {
        error_ = ArdaError::MaxTasks;
        return -1;
    }

    // Copy name (length already validated above, but use strncpy for defense in depth)
    strncpy(tasks[id].name, name, ARDA_MAX_NAME_LEN - 1);
    tasks[id].name[ARDA_MAX_NAME_LEN - 1] = '\0';

    return initTaskFields_(id, setup, loop, intervalMs, teardown, autoStart);
}

#endif  // ARDA_NO_NAMES

#ifndef ARDA_NO_PRIORITY
int8_t Arda::createTask(const char* name, TaskCallback setup, TaskCallback loop,
                        uint32_t intervalMs, TaskCallback teardown,
                        bool autoStart, TaskPriority priority) {
    // Validate priority before creating task
    uint8_t rawPriority = static_cast<uint8_t>(priority);
    constexpr uint8_t maxPriority = static_cast<uint8_t>(TaskPriority::Highest);
    if (rawPriority > maxPriority) {
        error_ = ArdaError::InvalidValue;
        return -1;
    }
    // Create task without auto-start so we can set priority first
    int8_t id = createTask(name, setup, loop, intervalMs, teardown, false);
    if (id >= 0) {
        updatePriority(tasks[id], rawPriority);
        // Handle auto-start: if begin() called, start now; otherwise set bit for begin()
        if (autoStart) {
            if (flags_ & FLAG_BEGUN) {
                if (startTask(id) != StartResult::Success) {
                    // Auto-start failed - delete the task and return -1.
                    // Error is already set by startTask(). Use autoStart=false to handle failures manually.
                    ArdaError savedError = error_;
                    deleteTask(id);
                    error_ = savedError;
                    return -1;
                }
            } else {
                // begin() not called yet - set autoStart bit for begin() to pick up
                tasks[id].flags |= ARDA_TASK_RAN_BIT;
            }
        }
    }
    return id;
}
#endif

#if defined(ARDA_TASK_RECOVERY) && !defined(ARDA_NO_PRIORITY)
int8_t Arda::createTask(const char* name, TaskCallback setup, TaskCallback loop,
                        uint32_t intervalMs, TaskCallback teardown, bool autoStart,
                        TaskPriority priority, uint32_t timeoutMs, TaskCallback recover) {
    // Call the priority-based createTask first
    int8_t id = createTask(name, setup, loop, intervalMs, teardown, autoStart, priority);
    if (id >= 0) {
        tasks[id].timeout = timeoutMs;
        tasks[id].recover = recover;
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

#ifdef ARDA_YIELD
    // Can't delete a task that has yielded and is waiting for yield() to return
    if (checkInYield(tasks[taskId])) {
        error_ = ArdaError::TaskYielded;
        return false;
    }
#endif

    // Mark deleted first to prevent re-deletion if trace callback calls deleteTask.
    // Note: This invalidates the task before the trace event, so callbacks cannot
    // query task info (getTaskName, getTaskState return nullptr/Invalid). The task ID
    // is still valid for identifying which task was deleted.
    markDeleted(tasks[taskId]);

    // Emit trace event (task already invalidated - callbacks get ID only)
    emitTrace(taskId, TraceEvent::TaskDeleted);

    // Clear task data and add to free list
    tasks[taskId].setup = nullptr;
    tasks[taskId].loop = nullptr;
    tasks[taskId].teardown = nullptr;
#ifdef ARDA_TASK_RECOVERY
    tasks[taskId].recover = nullptr;
#endif
    tasks[taskId].lastRun = 0;
    // Note: runCount shares union with nextFree; freeSlot() will set nextFree
#ifdef ARDA_TASK_RECOVERY
    tasks[taskId].timeout = 0;
#endif
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

#ifdef ARDA_SHELL_ACTIVE
    // Track shell deletion for isShellRunning() accuracy
    if (taskId == ARDA_SHELL_TASK_ID) {
        shellDeleted_ = true;
    }
#endif

    error_ = ArdaError::Ok;
    return true;
}

bool Arda::killTask(int8_t taskId) {
    // Validate task ID
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }

    // Fail-fast for currently executing task to avoid partial state changes.
    // Without this check, we'd stop the task then fail on delete, leaving
    // a stopped task while returning false - confusing "atomic" semantics.
    if (taskId == currentTask) {
        error_ = ArdaError::TaskExecuting;
        return false;
    }

    // Stop the task if not already stopped
    TaskState state = extractState(tasks[taskId]);
    if (state != TaskState::Stopped) {
        StopResult sr = stopTask(taskId);
        if (sr == StopResult::TeardownChangedState) {
            // Teardown restarted the task - can't delete
            // error_ already set to StateChanged by stopTask
            return false;
        }
        if (sr == StopResult::Failed) {
            // Couldn't stop (e.g., TaskYielded) - error_ already set
            return false;
        }
        // Success or TeardownSkipped - proceed to delete
    }

    // Delete the task
    return deleteTask(taskId);
}

// =============================================================================
// Task state control
// =============================================================================

StartResult Arda::startTask(int8_t taskId, bool runImmediately) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return StartResult::Failed;
    }
    if (extractState(tasks[taskId]) != TaskState::Stopped) {
        error_ = ArdaError::WrongState;
        return StartResult::Failed;
    }
    // NOTE: We intentionally allow loop == nullptr for "setup-only" tasks that perform
    // one-time initialization and remain resident (e.g., hardware config, event listeners).
    // Such tasks stay in Running state but runInternal() skips them. Callers needing a
    // loop should validate before createTask(). See test_null_loop_function_task.

    updateState(tasks[taskId], TaskState::Running);
    if (runImmediately && tasks[taskId].interval > 0) {
        // Set lastRun far enough in the past to trigger on next run() cycle.
        tasks[taskId].lastRun = millis() - tasks[taskId].interval;
    } else {
        tasks[taskId].lastRun = millis();  // Wait one full interval before first run
    }
    tasks[taskId].runCount = 0;
    // runImmediately controls same-cycle execution for ALL tasks (including zero-interval).
    // If false, mark as already-ran to prevent execution in current cycle.
    // IMPORTANT: If we're inside a run() cycle, always set ranThisCycle=true to prevent
    // a task from executing multiple times per cycle (e.g., task stops and restarts itself).
    if (flags_ & FLAG_IN_RUN) {
        updateRanThisCycle(tasks[taskId], true);  // Already had its chance this cycle
    } else {
        updateRanThisCycle(tasks[taskId], !runImmediately);
    }
#ifdef ARDA_YIELD
    updateInYield(tasks[taskId], false);  // Clear any stale yield state from previous run
#endif

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
            return StartResult::Failed;
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
            return StartResult::SetupChangedState;
        }
    }

    emitTrace(taskId, TraceEvent::TaskStarted);
    error_ = ArdaError::Ok;
    return StartResult::Success;
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

#ifdef ARDA_YIELD
    // Can't stop a task that is mid-yield (teardown would run while stack frame active)
    if (checkInYield(tasks[taskId])) {
        error_ = ArdaError::TaskYielded;
        return StopResult::Failed;
    }
#endif

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

#ifdef ARDA_TASK_RECOVERY
bool Arda::setTaskTimeout(int8_t taskId, uint32_t timeoutMs) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    tasks[taskId].timeout = timeoutMs;

#if ARDA_TASK_RECOVERY_IMPL
    // If this task is currently executing and recovery is armed, re-arm with new timeout.
    // This allows tasks to extend/disable their timeout mid-loop for known-slow operations.
    if (taskId == currentTask && recoveryArmed_) {
        disarmRecoveryTimer();
        if (timeoutMs > 0 && recoveryEnabled_) {
            armRecoveryTimer(taskId, timeoutMs);
        }
    }
#endif

    error_ = ArdaError::Ok;
    return true;
}

#if ARDA_TASK_RECOVERY_IMPL
bool Arda::heartbeat() {
    if (currentTask < 0) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    uint32_t timeout = tasks[currentTask].timeout;
    if (timeout == 0) {
        error_ = ArdaError::WrongState;  // No timeout configured
        return false;
    }
    if (recoveryArmed_) {
        disarmRecoveryTimer();
        armRecoveryTimer(currentTask, timeout);
    }
    error_ = ArdaError::Ok;
    return true;
}
#endif

bool Arda::setTaskRecover(int8_t taskId, TaskCallback recover) {
#if ARDA_TASK_RECOVERY_IMPL
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    tasks[taskId].recover = recover;
    error_ = ArdaError::Ok;
    return true;
#else
    (void)taskId; (void)recover;
    error_ = ArdaError::NotSupported;
    return false;  // Hardware doesn't support task recovery
#endif
}

bool Arda::hasTaskRecover(int8_t taskId) const {
#if ARDA_TASK_RECOVERY_IMPL
    if (!isValidTask(taskId)) return false;
    return tasks[taskId].recover != nullptr;
#else
    (void)taskId;
    return false;
#endif
}

bool Arda::setTaskRecoveryEnabled(bool enabled) {
#if ARDA_TASK_RECOVERY_IMPL
    uint8_t sreg = SREG;
    cli();  // Atomic update

    if (!enabled && recoveryArmed_) {
        // Disabling while timer is armed - disarm it safely
        // Order matches disarmRecoveryTimer() for consistency
        recoveryArmed_ = false;
        TCCR2B = 0;
        TIMSK2 = 0;
        TIFR2 = (1 << TOV2);
        recoveryTask_ = -1;  // Clear for consistency with disarmRecoveryTimer()
    }

    recoveryEnabled_ = enabled;
    SREG = sreg;
#else
    // Non-AVR: software flag only (no hardware abort to control)
    recoveryEnabled_ = enabled;
#endif
    error_ = ArdaError::Ok;
    return true;
}

bool Arda::isTaskRecoveryEnabled() const {
    return recoveryEnabled_;
}
#endif

#ifndef ARDA_NO_PRIORITY
bool Arda::setTaskPriority(int8_t taskId, TaskPriority priority) {
    if (!isValidTask(taskId)) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    uint8_t rawPriority = static_cast<uint8_t>(priority);
    constexpr uint8_t maxPriority = static_cast<uint8_t>(TaskPriority::Highest);
    if (rawPriority > maxPriority) {
        error_ = ArdaError::InvalidValue;
        return false;
    }
    updatePriority(tasks[taskId], rawPriority);
    error_ = ArdaError::Ok;
    return true;
}

TaskPriority Arda::getTaskPriority(int8_t taskId) const {
    if (!isValidTask(taskId)) return TaskPriority::Lowest;
    return static_cast<TaskPriority>(extractPriority(tasks[taskId]));
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
        error_ = ArdaError::Ok;  // No-op is successful
        return 0;
    }
    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t i = 0; i < count; i++) {
        if (startTask(taskIds[i]) == StartResult::Success) {
            succeeded++;
        } else if (firstError == ArdaError::Ok) {
            firstError = error_;  // Capture first failure
            if (failedId) *failedId = taskIds[i];
        }
        // Continue attempting remaining tasks even if one fails
    }
    if (firstError != ArdaError::Ok) {
        error_ = firstError;  // Restore first error (more diagnostic than last)
    } else {
        error_ = ArdaError::Ok;  // Clear stale errors on full success
    }
    return succeeded;
}

int8_t Arda::stopTasks(const int8_t* taskIds, int8_t count, int8_t* failedId) {
    if (failedId) *failedId = -1;
    if (taskIds == nullptr || count <= 0) {
        error_ = ArdaError::Ok;  // No-op is successful
        return 0;
    }
    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t i = 0; i < count; i++) {
        StopResult result = stopTask(taskIds[i]);
        // Only Success and TeardownSkipped mean the task is actually stopped.
        // TeardownChangedState means teardown restarted the task - not stopped!
        if (result == StopResult::Success || result == StopResult::TeardownSkipped) {
            succeeded++;
            // Capture teardown skip warning
            if (result == StopResult::TeardownSkipped && firstError == ArdaError::Ok) {
                firstError = error_;
            }
        } else if (firstError == ArdaError::Ok) {
            firstError = error_;  // Capture first failure (Failed or TeardownChangedState)
            if (failedId) *failedId = taskIds[i];
        }
        // Continue attempting remaining tasks even if one fails
    }
    if (firstError != ArdaError::Ok) {
        error_ = firstError;  // Restore first error (more diagnostic than last)
    } else {
        error_ = ArdaError::Ok;  // Clear stale errors on full success
    }
    return succeeded;
}

int8_t Arda::pauseTasks(const int8_t* taskIds, int8_t count, int8_t* failedId) {
    if (failedId) *failedId = -1;
    if (taskIds == nullptr || count <= 0) {
        error_ = ArdaError::Ok;  // No-op is successful
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
    if (firstError != ArdaError::Ok) {
        error_ = firstError;  // Restore first error (more diagnostic than last)
    } else {
        error_ = ArdaError::Ok;  // Clear stale errors on full success
    }
    return succeeded;
}

int8_t Arda::resumeTasks(const int8_t* taskIds, int8_t count, int8_t* failedId) {
    if (failedId) *failedId = -1;
    if (taskIds == nullptr || count <= 0) {
        error_ = ArdaError::Ok;  // No-op is successful
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
    if (firstError != ArdaError::Ok) {
        error_ = firstError;  // Restore first error (more diagnostic than last)
    } else {
        error_ = ArdaError::Ok;  // Clear stale errors on full success
    }
    return succeeded;
}

int8_t Arda::startAllTasks() {
    // Snapshot task IDs to protect against callbacks modifying the task array
    int8_t snapshot[ARDA_MAX_TASKS];
    int8_t snapshotCount = 0;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i]) && extractState(tasks[i]) == TaskState::Stopped) {
            snapshot[snapshotCount++] = i;
        }
    }

    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t j = 0; j < snapshotCount; j++) {
        int8_t i = snapshot[j];
        if (!isValidTask(i)) continue;  // Task was deleted since snapshot
        if (extractState(tasks[i]) != TaskState::Stopped) continue;  // State changed since snapshot
        if (startTask(i) == StartResult::Success) {
            succeeded++;
        } else if (firstError == ArdaError::Ok) {
            firstError = error_;
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
    // Snapshot task IDs to protect against callbacks modifying the task array
    int8_t snapshot[ARDA_MAX_TASKS];
    int8_t snapshotCount = 0;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i]) && extractState(tasks[i]) != TaskState::Stopped) {
            snapshot[snapshotCount++] = i;
        }
    }

    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t j = 0; j < snapshotCount; j++) {
        int8_t i = snapshot[j];
        if (!isValidTask(i)) continue;  // Task was deleted since snapshot
        if (extractState(tasks[i]) == TaskState::Stopped) continue;  // State changed since snapshot
        StopResult result = stopTask(i);
        // Only Success and TeardownSkipped mean the task is actually stopped.
        // TeardownChangedState means teardown restarted the task - not stopped!
        if (result == StopResult::Success || result == StopResult::TeardownSkipped) {
            succeeded++;
            // Capture teardown skip warning
            if (result == StopResult::TeardownSkipped && firstError == ArdaError::Ok) {
                firstError = error_;
            }
        } else if (firstError == ArdaError::Ok) {
            firstError = error_;  // Capture first failure (Failed or TeardownChangedState)
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
    // Snapshot task IDs to protect against callbacks modifying the task array
    int8_t snapshot[ARDA_MAX_TASKS];
    int8_t snapshotCount = 0;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i]) && extractState(tasks[i]) == TaskState::Running) {
            snapshot[snapshotCount++] = i;
        }
    }

    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t j = 0; j < snapshotCount; j++) {
        int8_t i = snapshot[j];
        if (!isValidTask(i)) continue;  // Task was deleted since snapshot
        if (extractState(tasks[i]) != TaskState::Running) continue;  // State changed since snapshot
        if (pauseTask(i)) {
            succeeded++;
        } else if (firstError == ArdaError::Ok) {
            firstError = error_;
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
    // Snapshot task IDs to protect against callbacks modifying the task array
    int8_t snapshot[ARDA_MAX_TASKS];
    int8_t snapshotCount = 0;
    for (int8_t i = 0; i < taskCount; i++) {
        if (!isDeleted(tasks[i]) && extractState(tasks[i]) == TaskState::Paused) {
            snapshot[snapshotCount++] = i;
        }
    }

    int8_t succeeded = 0;
    ArdaError firstError = ArdaError::Ok;
    for (int8_t j = 0; j < snapshotCount; j++) {
        int8_t i = snapshot[j];
        if (!isValidTask(i)) continue;  // Task was deleted since snapshot
        if (extractState(tasks[i]) != TaskState::Paused) continue;  // State changed since snapshot
        if (resumeTask(i)) {
            succeeded++;
        } else if (firstError == ArdaError::Ok) {
            firstError = error_;
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

#ifdef ARDA_TASK_RECOVERY
uint32_t Arda::getTaskTimeout(int8_t taskId) const {
    if (!isValidTask(taskId)) {
        return 0;
    }
    return tasks[taskId].timeout;
}
#endif

uint32_t Arda::getTaskLastRun(int8_t taskId) const {
    if (!isValidTask(taskId)) {
        return 0;
    }
    // Return 0 if task has never executed its loop() (runCount tracks loop executions).
    // Note: runCount skips 0 on overflow (wraps UINT32_MAX → 1), so runCount == 0
    // reliably indicates "never ran". After overflow, the count is off by 1 per wrap.
    if (tasks[taskId].runCount == 0) {
        return 0;
    }
    return tasks[taskId].lastRun;
}

int8_t Arda::getCurrentTask() const {
    return currentTask;
}

bool Arda::isValidTask(int8_t taskId) const {
    return taskId >= 0 && taskId < taskCount && !isDeleted(tasks[taskId]);
}

#ifdef ARDA_YIELD
bool Arda::isTaskYielded(int8_t taskId) const {
    if (!isValidTask(taskId)) return false;
    return checkInYield(tasks[taskId]);
}
#endif

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

#ifdef ARDA_TASK_RECOVERY
void Arda::setTimeoutCallback(TimeoutCallback callback) {
    timeoutCallback = callback;
}
#endif

void Arda::setStartFailureCallback(StartFailureCallback callback) {
    startFailureCallback = callback;
}

void Arda::setTraceCallback(TraceCallback callback) {
    traceCallback = callback;
}

// =============================================================================
// Utility
// =============================================================================

#ifdef ARDA_YIELD
void Arda::yield() {
    // Give other tasks a chance to run while this task waits.
    // Skips the currently executing task to prevent infinite recursion,
    // but allows other ready tasks to execute.
    // ranThisCycle prevents tasks from running twice in the same cycle.
    if (currentTask >= 0) {
#ifdef ARDA_WATCHDOG
        wdt_reset();
#endif
        int8_t yieldingTask = currentTask;
        bool wasInRun = (flags_ & FLAG_IN_RUN) != 0;  // Save original state

        // If yield() is called outside run() (e.g., from setup/teardown), clear
        // stale ranThisCycle flags from previous cycles to allow ready tasks to run.
        // The yielding task keeps its flag to prevent double execution.
        // Exception: during begin() task-start loop, bit 2 is "shouldAutoStart" not "ranThisCycle"
        if (!wasInRun && !(flags_ & FLAG_IN_BEGIN)) {
            for (int8_t i = 0; i < taskCount; i++) {
                if (!isDeleted(tasks[i]) && i != yieldingTask) {
                    updateRanThisCycle(tasks[i], false);
                }
            }
        }

        updateInYield(tasks[yieldingTask], true);  // Mark as yielding (prevents deletion)
        flags_ &= ~FLAG_IN_RUN;  // Temporarily allow re-entry
        runInternal(yieldingTask);  // Run others, skip current
        if (wasInRun) flags_ |= FLAG_IN_RUN; else flags_ &= ~FLAG_IN_RUN;  // Restore original state
        updateInYield(tasks[yieldingTask], false);  // No longer yielding
    }
}
#endif

uint32_t Arda::uptime() const {
    if (!(flags_ & FLAG_BEGUN)) {
        return 0;
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
#ifdef ARDA_SHORT_MESSAGES
        case ArdaError::Ok:            return "Ok";
        case ArdaError::NullName:      return "NullName";
        case ArdaError::EmptyName:     return "EmptyName";
        case ArdaError::NameTooLong:   return "NameTooLong";
        case ArdaError::DuplicateName: return "DuplicateName";
        case ArdaError::MaxTasks:      return "MaxTasks";
        case ArdaError::InvalidId:     return "InvalidId";
        case ArdaError::WrongState:    return "WrongState";
        case ArdaError::TaskExecuting: return "TaskExecuting";
#ifdef ARDA_YIELD
        case ArdaError::TaskYielded:   return "TaskYielded";
#endif
        case ArdaError::AlreadyBegun:  return "AlreadyBegun";
        case ArdaError::CallbackDepth: return "CallbackDepth";
        case ArdaError::StateChanged:  return "StateChanged";
        case ArdaError::InCallback:    return "InCallback";
        case ArdaError::NotSupported:  return "NotSupported";
        case ArdaError::InvalidValue:  return "InvalidValue";
#ifdef ARDA_TASK_RECOVERY
        case ArdaError::TaskAborted:   return "Aborted";
#endif
        default:                       return "Unknown";
#else
        case ArdaError::Ok:            return "No error";
        case ArdaError::NullName:      return "Task name is null";
        case ArdaError::EmptyName:     return "Task name is empty";
        case ArdaError::NameTooLong:   return "Task name too long";
        case ArdaError::DuplicateName: return "Task name already exists";
        case ArdaError::MaxTasks:      return "Max tasks reached";
        case ArdaError::InvalidId:     return "Invalid task ID";
        case ArdaError::WrongState:    return "Wrong task state";
        case ArdaError::TaskExecuting: return "Task is executing";
#ifdef ARDA_YIELD
        case ArdaError::TaskYielded:   return "Task has yielded";
#endif
        case ArdaError::AlreadyBegun:  return "Already begun";
        case ArdaError::CallbackDepth: return "Callback depth exceeded";
        case ArdaError::StateChanged:  return "State changed unexpectedly";
        case ArdaError::InCallback:    return "Cannot call from callback";
        case ArdaError::NotSupported:  return "Not supported";
        case ArdaError::InvalidValue:  return "Value out of range";
#ifdef ARDA_TASK_RECOVERY
        case ArdaError::TaskAborted:   return "Task forcibly aborted (timeout)";
#endif
        default:                       return "Unknown error";
#endif
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
    // Depth check prevents unbounded recursion if trace callback triggers more traces
    if (traceCallback && callbackDepth < ARDA_MAX_CALLBACK_DEPTH) {
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
#ifdef ARDA_YIELD
static inline bool checkInYield(const Task& task) {
    return (task.flags & ARDA_TASK_YIELD_BIT) != 0;
}
static inline void updateInYield(Task& task, bool val) {
    if (val) task.flags |= ARDA_TASK_YIELD_BIT;
    else task.flags &= ~ARDA_TASK_YIELD_BIT;
}
#endif
#ifndef ARDA_NO_PRIORITY
static inline uint8_t extractPriority(const Task& task) {
    return (task.flags & ARDA_TASK_PRIORITY_MASK) >> ARDA_TASK_PRIORITY_SHIFT;
}
static inline void updatePriority(Task& task, uint8_t priority) {
    constexpr uint8_t maxPriority = static_cast<uint8_t>(TaskPriority::Highest);
    if (priority > maxPriority) priority = maxPriority;
    task.flags = (task.flags & ~ARDA_TASK_PRIORITY_MASK) | (priority << ARDA_TASK_PRIORITY_SHIFT);
}
#endif

// =============================================================================
// Shell Implementation
// =============================================================================

#ifdef ARDA_SHELL_ACTIVE

// Shell loop callback - runs through normal task machinery
// Declared as friend of Arda class to access private members
void ardaShellLoop_() {
    if (!OS.shellStream_) return;
    while (OS.shellStream_->available()) {
        char c = OS.shellStream_->read();
        if (c == '\n' || c == '\r') {
            if (OS.shellBufIdx_ > 0) {
                // Re-entrancy guard: if busy (e.g., exec() called from a callback
                // triggered by a shell command), discard this serial command
                if (OS.shellBusy_) {
                    OS.shellBufIdx_ = 0;
                    continue;
                }
                OS.shellBusy_ = true;
                OS.shellBuf_[OS.shellBufIdx_] = '\0';
                uint8_t len = OS.shellBufIdx_;
                OS.shellBufIdx_ = 0;
                OS.shellCmd_(len);
                OS.shellBusy_ = false;
            }
        } else if (OS.shellBufIdx_ < ARDA_SHELL_BUF_SIZE - 1) {
            OS.shellBuf_[OS.shellBufIdx_++] = c;
        }
    }
}

void Arda::shellCmd_(uint8_t len) {
#ifndef ARDA_NO_SHELL_ECHO
    if (shellEcho_) {
        shellStream_->println();
        shellStream_->print(F("> "));
        shellStream_->println(shellBuf_);
    }
#endif

    char cmd = shellBuf_[0];
    int8_t id = -1;

    // Parse task ID if present: "p 1" or "p 12"
    // Manual parsing with overflow protection (avoids atoi edge cases)
    if (len >= 3 && shellBuf_[1] == ' ' &&
        shellBuf_[2] >= '0' && shellBuf_[2] <= '9') {
        int16_t parsed = 0;  // Use int16_t to detect overflow into int8_t range
        for (uint8_t j = 2; j < len && shellBuf_[j] >= '0' && shellBuf_[j] <= '9'; j++) {
            parsed = parsed * 10 + (shellBuf_[j] - '0');
            if (parsed > 127) {  // int8_t max - prevent overflow
                parsed = -1;
                break;
            }
        }
        id = (int8_t)parsed;
    }

    switch (cmd) {
        // Core task control
        case 'p':
            if (id < 0) { shellStream_->println(F("p <id>")); break; }
            if (pauseTask(id)) shellStream_->println(F("OK"));
            else { shellStream_->print(F("ERR ")); shellStream_->println(errorString(error_)); }
            break;
        case 'r':
            if (id < 0) { shellStream_->println(F("r <id>")); break; }
            if (resumeTask(id)) shellStream_->println(F("OK"));
            else { shellStream_->print(F("ERR ")); shellStream_->println(errorString(error_)); }
            break;
        case 's':
            if (id < 0) { shellStream_->println(F("s <id>")); break; }
            {
                StopResult sr = stopTask(id);
                if (sr == StopResult::Success || sr == StopResult::TeardownSkipped) {
                    shellStream_->println(F("OK"));
                } else if (sr == StopResult::TeardownChangedState) {
                    // Teardown restarted the task - it's not actually stopped
                    shellStream_->println(F("ERR state"));
                } else {
                    shellStream_->print(F("ERR ")); shellStream_->println(errorString(error_));
                }
            }
            break;
        case 'b':
            if (id < 0) { shellStream_->println(F("b <id>")); break; }
            if (startTask(id) == StartResult::Success) shellStream_->println(F("OK"));
            else { shellStream_->print(F("ERR ")); shellStream_->println(errorString(error_)); }
            break;
        case 'd':
            if (id < 0) { shellStream_->println(F("d <id>")); break; }
            // d requires task to be Stopped (use k for stop+delete)
            if (!isValidTask(id)) {
                error_ = ArdaError::InvalidId;
                shellStream_->print(F("ERR "));
                shellStream_->println(errorString(error_));
                break;
            }
            if (extractState(tasks[id]) != TaskState::Stopped) {
                error_ = ArdaError::WrongState;
                shellStream_->print(F("ERR "));
                shellStream_->println(errorString(error_));
                break;
            }
            // Self-delete when Stopped: defer deletion (deleteTask would fail with TaskExecuting)
            if (id == currentTask) {
                pendingSelfDelete_ = id;
                // Note: shellDeleted_ is set when deferred deletion succeeds in runInternal(), not here
                shellStream_->println(F("OK"));
            } else if (deleteTask(id)) {
                if (id == ARDA_SHELL_TASK_ID) shellDeleted_ = true;
                shellStream_->println(F("OK"));
            } else {
                shellStream_->print(F("ERR "));
                shellStream_->println(errorString(error_));
            }
            break;
        case 'k':  // Kill (stop + delete)
            if (id < 0) { shellStream_->println(F("k <id>")); break; }
            // Self-kill requires deferred deletion (can't delete while executing)
            if (id == currentTask) {
                // Stop first (if running), then defer the delete
                if (extractState(tasks[id]) != TaskState::Stopped) {
                    StopResult sr = stopTask(id);
                    if (sr == StopResult::TeardownChangedState || sr == StopResult::Failed) {
                        shellStream_->print(F("ERR "));
                        shellStream_->println(errorString(error_));
                        break;
                    }
                }
                pendingSelfDelete_ = id;
                shellStream_->println(F("OK"));
            } else if (killTask(id)) {
                shellStream_->println(F("OK"));
            } else {
                shellStream_->print(F("ERR "));
                shellStream_->println(errorString(error_));
            }
            break;
        case 'l':
            shellList_();
            break;

#ifndef ARDA_SHELL_MINIMAL
        // Info/debug commands
        case 'i':
            if (id < 0) { shellStream_->println(F("i <id>")); break; }
            shellInfo_(id);
            break;
        case 'w':  // When: last run / next due
            if (id < 0) { shellStream_->println(F("w <id>")); break; }
            if (!isValidTask(id)) {
                error_ = ArdaError::InvalidId;
                shellStream_->print(F("ERR "));
                shellStream_->println(errorString(error_));
                break;
            }
            {
                TaskState st = getTaskState(id);
                uint32_t last = getTaskLastRun(id);
                uint32_t intv = getTaskInterval(id);
                shellStream_->print(F("last:"));
                // Check runCount to determine if task has ever executed its loop()
                if (getTaskRunCount(id) == 0) {
                    shellStream_->print(F("never"));
                } else {
                    uint32_t now = millis();
                    uint32_t elapsed = now - last;
                    shellStream_->print(elapsed);
                    shellStream_->print(F("ms ago"));
                    // Only show next/due timing for running tasks
                    if (st == TaskState::Running && intv > 0) {
                        if (elapsed < intv) {
                            shellStream_->print(F(" next:"));
                            shellStream_->print(intv - elapsed);
                            shellStream_->print(F("ms"));
                        } else {
                            shellStream_->print(F(" due:now"));
                        }
                    }
                }
                if (st == TaskState::Paused) {
                    shellStream_->print(F(" [P]"));
                } else if (st == TaskState::Stopped) {
                    shellStream_->print(F(" [S]"));
                }
                shellStream_->println();
            }
            break;
        case 'g':  // Go: start and run immediately (Stopped tasks only)
            if (id < 0) { shellStream_->println(F("g <id>")); break; }
            if (startTask(id, true) == StartResult::Success) shellStream_->println(F("OK"));
            else { shellStream_->print(F("ERR ")); shellStream_->println(errorString(error_)); }
            break;
        case 'c':  // Clear error
            clearError();
            shellStream_->println(F("OK"));
            break;
        case 'a':  // Adjust interval: "a 1 500"
            if (id < 0) { shellStream_->println(F("a <id> <ms>")); break; }
            {
                // Check if second argument exists (skip id digits, skip spaces, check for digit)
                uint8_t j = 2;
                while (j < len && shellBuf_[j] >= '0' && shellBuf_[j] <= '9') j++;
                while (j < len && shellBuf_[j] == ' ') j++;
                if (j >= len || shellBuf_[j] < '0' || shellBuf_[j] > '9') {
                    shellStream_->println(F("a <id> <ms>"));
                    break;
                }
                uint32_t ms = shellParseArg2_(len);
                if (!setTaskInterval(id, ms)) {
                    shellStream_->print(F("ERR "));
                    shellStream_->println(errorString(error_));
                } else {
                    shellStream_->println(F("OK"));
                }
            }
            break;
#ifdef ARDA_TASK_RECOVERY
        case 't':  // Timeout: "t 1 100"
            if (id < 0) { shellStream_->println(F("t <id> <ms>")); break; }
            {
                uint8_t j = 2;
                while (j < len && shellBuf_[j] >= '0' && shellBuf_[j] <= '9') j++;
                while (j < len && shellBuf_[j] == ' ') j++;
                if (j >= len || shellBuf_[j] < '0' || shellBuf_[j] > '9') {
                    shellStream_->println(F("t <id> <ms>"));
                    break;
                }
                uint32_t ms = shellParseArg2_(len);
                if (!setTaskTimeout(id, ms)) {
                    shellStream_->print(F("ERR "));
                    shellStream_->println(errorString(error_));
                } else {
                    shellStream_->println(F("OK"));
                }
            }
            break;
#endif
#ifndef ARDA_NO_NAMES
        case 'n':  // Rename: "n 1 newname"
            if (id < 0) { shellStream_->println(F("n <id> <name>")); break; }
            {
                // Find where name starts (after "n <id> ")
                uint8_t j = 2;
                while (j < len && shellBuf_[j] >= '0' && shellBuf_[j] <= '9') j++;  // skip id
                while (j < len && shellBuf_[j] == ' ') j++;  // skip space
                if (j >= len) { shellStream_->println(F("n <id> <name>")); break; }
                if (renameTask(id, &shellBuf_[j])) shellStream_->println(F("OK"));
                else { shellStream_->print(F("ERR ")); shellStream_->println(errorString(error_)); }
            }
            break;
#endif
#ifndef ARDA_NO_PRIORITY
        case 'y':  // Priority: "y 1 3" (0-4, Lowest to Highest)
            if (id < 0) { shellStream_->println(F("y <id> <pri>")); break; }
            {
                // Check if second argument exists (skip id digits, skip spaces, check for digit)
                uint8_t j = 2;
                while (j < len && shellBuf_[j] >= '0' && shellBuf_[j] <= '9') j++;
                while (j < len && shellBuf_[j] == ' ') j++;
                if (j >= len || shellBuf_[j] < '0' || shellBuf_[j] > '9') {
                    shellStream_->println(F("y <id> <pri>"));
                    break;
                }
                uint32_t pri = shellParseArg2_(len);
                if (!setTaskPriority(id, static_cast<TaskPriority>(pri))) {
                    shellStream_->print(F("ERR "));
                    shellStream_->println(errorString(error_));
                } else {
                    shellStream_->println(F("OK"));
                }
            }
            break;
#endif
        case 'u':
            shellStream_->print(uptime() / 1000);
            shellStream_->println(F("s"));
            break;
        case 'm':
            shellStream_->print(F("tasks:"));
            shellStream_->print(getTaskCount());
            shellStream_->print('/');
            shellStream_->print(getMaxTasks());
            shellStream_->print(F(" slots:"));
            shellStream_->println(getSlotCount());
            break;
        case 'e':
            shellStream_->println(errorString(error_));
            break;
        case 'v':
            shellStream_->print(F("Arda "));
            shellStream_->println(F(ARDA_VERSION_STRING));
            break;
#endif

#ifndef ARDA_NO_SHELL_ECHO
        case 'o':
            if (len >= 3 && shellBuf_[1] == ' ') {
                shellEcho_ = (shellBuf_[2] == '1');
            }
            shellStream_->println(shellEcho_ ? F("on") : F("off"));
            break;
#endif

        case 'h': case '?':
#ifdef ARDA_SHORT_MESSAGES
            // Short help (minimal flash)
            // Group 1: Task lifecycle (requires ID)
            shellStream_->println(F("b begin"));
            shellStream_->println(F("s stop"));
            shellStream_->println(F("p pause"));
            shellStream_->println(F("r resume"));
            shellStream_->println(F("k kill"));
            shellStream_->println(F("d delete"));
#ifndef ARDA_SHELL_MINIMAL
            shellStream_->println();
            // Group 2: Task query/modify (requires ID)
            shellStream_->println(F("i info"));
            shellStream_->println(F("w when"));
            shellStream_->println(F("a interval"));
#ifdef ARDA_TASK_RECOVERY
            shellStream_->println(F("t timeout"));
#endif
#ifndef ARDA_NO_PRIORITY
            shellStream_->println(F("y priority"));
#endif
#ifndef ARDA_NO_NAMES
            shellStream_->println(F("n rename"));
#endif
            shellStream_->println(F("g go"));
#endif
            shellStream_->println();
            // Group 3: System/global (no ID)
            shellStream_->println(F("l list"));
#ifndef ARDA_SHELL_MINIMAL
            shellStream_->println(F("e error"));
            shellStream_->println(F("c clear"));
            shellStream_->println(F("m memory"));
            shellStream_->println(F("u uptime"));
            shellStream_->println(F("v version"));
#endif
#ifndef ARDA_NO_SHELL_ECHO
            shellStream_->println(F("o echo"));
#endif
#else
            // Full help (descriptive)
            // Group 1: Task lifecycle (requires ID)
            shellStream_->println(F("b <id>        begin task"));
            shellStream_->println(F("s <id>        stop task"));
            shellStream_->println(F("p <id>        pause task"));
            shellStream_->println(F("r <id>        resume task"));
            shellStream_->println(F("k <id>        kill (stop+delete)"));
            shellStream_->println(F("d <id>        delete task"));
#ifndef ARDA_SHELL_MINIMAL
            shellStream_->println();
            // Group 2: Task query/modify (requires ID)
            shellStream_->println(F("i <id>        task info"));
            shellStream_->println(F("w <id>        when (timing info)"));
            shellStream_->println(F("a <id> <ms>   adjust interval"));
#ifdef ARDA_TASK_RECOVERY
            shellStream_->println(F("t <id> <ms>   set timeout"));
#endif
#ifndef ARDA_NO_PRIORITY
            shellStream_->println(F("y <id> <pri>  set priority"));
#endif
#ifndef ARDA_NO_NAMES
            shellStream_->println(F("n <id> <name> rename task"));
#endif
            shellStream_->println(F("g <id>        go (begin+run now)"));
#endif
            shellStream_->println();
            // Group 3: System/global (no ID)
            shellStream_->println(F("l             list all tasks"));
#ifndef ARDA_SHELL_MINIMAL
            shellStream_->println(F("e             last error"));
            shellStream_->println(F("c             clear error"));
            shellStream_->println(F("m             memory info"));
            shellStream_->println(F("u             uptime"));
            shellStream_->println(F("v             version"));
#endif
#ifndef ARDA_NO_SHELL_ECHO
            shellStream_->println(F("o 0|1         echo on/off"));
#endif
#endif
            break;
        default:
            shellStream_->println(F("?"));
    }
}

void Arda::shellList_() {
    for (int8_t i = 0; i < taskCount; i++) {
        if (isDeleted(tasks[i])) continue;
        shellStream_->print(i);
        shellStream_->print(' ');
        TaskState st = getTaskState(i);
        shellStream_->print(st == TaskState::Running ? 'R' :
                           st == TaskState::Paused ? 'P' : 'S');
#ifndef ARDA_NO_NAMES
        shellStream_->print(' ');
        shellStream_->print(tasks[i].name);
#endif
        shellStream_->println();
    }
}

#ifndef ARDA_SHELL_MINIMAL
void Arda::shellInfo_(int8_t id) {
    if (!isValidTask(id)) {
        shellStream_->println(F("invalid"));
        return;
    }
    shellStream_->print(F("int:"));
    shellStream_->print(getTaskInterval(id));
    shellStream_->print(F(" runs:"));
    shellStream_->print(getTaskRunCount(id));
#ifndef ARDA_NO_PRIORITY
    shellStream_->print(F(" pri:"));
    shellStream_->print(static_cast<uint8_t>(getTaskPriority(id)));
#endif
#ifdef ARDA_TASK_RECOVERY
    uint32_t to = getTaskTimeout(id);
    if (to > 0) {
        shellStream_->print(F(" timeout:"));
        shellStream_->print(to);
    }
#endif
    shellStream_->println();
}

uint32_t Arda::shellParseArg2_(uint8_t len) {
    uint8_t j = 2;
    // Skip first number (the id)
    while (j < len && shellBuf_[j] >= '0' && shellBuf_[j] <= '9') j++;
    // Skip space(s)
    while (j < len && shellBuf_[j] == ' ') j++;
    // Parse second number with overflow protection (AVR-safe, no uint64_t)
    uint32_t result = 0;
    while (j < len && shellBuf_[j] >= '0' && shellBuf_[j] <= '9') {
        uint8_t digit = shellBuf_[j] - '0';
        // Check if result * 10 + digit would overflow
        if (result > (UINT32_MAX - digit) / 10) return UINT32_MAX;
        result = result * 10 + digit;
        j++;
    }
    return result;
}
#endif

void Arda::setShellStream(Stream& s) { shellStream_ = &s; }

#ifndef ARDA_NO_SHELL_ECHO
void Arda::setShellEcho(bool enabled) { shellEcho_ = enabled; }
#endif

bool Arda::isShellRunning() const {
    // Check shellDeleted_ to avoid false positive when slot 0 is reused
    return !shellDeleted_ &&
           isValidTask(ARDA_SHELL_TASK_ID) &&
           getTaskState(ARDA_SHELL_TASK_ID) == TaskState::Running;
}

void Arda::exec(const char* cmd) {
    if (!cmd || shellBusy_ || !shellStream_) return;  // Re-entrancy guard + null stream check
    size_t cmdLen = strlen(cmd);
    // Check length before truncating to uint8_t to avoid overflow
    if (cmdLen == 0 || cmdLen >= ARDA_SHELL_BUF_SIZE) return;
    uint8_t len = (uint8_t)cmdLen;

    shellBusy_ = true;
    strncpy(shellBuf_, cmd, ARDA_SHELL_BUF_SIZE - 1);
    shellBuf_[ARDA_SHELL_BUF_SIZE - 1] = '\0';

#ifndef ARDA_NO_SHELL_ECHO
    // Suppress echo for programmatic execution
    bool savedEcho = shellEcho_;
    shellEcho_ = false;
#endif
    shellCmd_(len);
#ifndef ARDA_NO_SHELL_ECHO
    shellEcho_ = savedEcho;
#endif

    shellBusy_ = false;
}

#ifdef ARDA_SHELL_MANUAL_START
bool Arda::startShell() {
    if (shellDeleted_) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    return startTask(ARDA_SHELL_TASK_ID) == StartResult::Success;
}
bool Arda::stopShell() {
    if (shellDeleted_) {
        error_ = ArdaError::InvalidId;
        return false;
    }
    StopResult sr = stopTask(ARDA_SHELL_TASK_ID);
    return sr == StopResult::Success || sr == StopResult::TeardownSkipped;
}
#endif

#endif // ARDA_SHELL_ACTIVE
