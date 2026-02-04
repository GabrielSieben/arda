/*
 * TaskRecovery - Demonstrating Arda's Soft Watchdog Feature
 *
 * =============================================================================
 * WHY THIS MATTERS
 * =============================================================================
 *
 * In cooperative multitasking, a single misbehaving task can freeze your entire
 * system. If a task's loop() never returns - whether due to a bug, waiting on
 * unresponsive hardware, or an unexpected infinite loop - no other tasks run.
 *
 * Traditional solutions:
 *   - Hardware watchdog: Resets the ENTIRE MCU. You lose all state, all tasks
 *     restart, and if the problem persists, you're stuck in a reset loop.
 *   - Hope for the best: Cross your fingers that your code is perfect.
 *
 * Arda's Task Recovery provides a better option (AVR only):
 *   - Automatically aborts ONLY the stuck task after a configurable timeout
 *   - Other tasks continue running normally
 *   - A recovery callback lets you clean up partial state
 *   - You can stop, restart, or reconfigure the misbehaving task
 *   - No MCU reset required
 *
 * =============================================================================
 * HOW IT WORKS (AVR with Timer2)
 * =============================================================================
 *
 * Before each task's loop() runs, Arda:
 *   1. Saves the current execution context using setjmp()
 *   2. Arms Timer2 with your specified timeout
 *   3. Runs your task's loop()
 *
 * If loop() returns normally:
 *   - Timer2 is disarmed, everything continues as usual
 *
 * If loop() exceeds the timeout:
 *   - Timer2 ISR fires and calls longjmp()
 *   - Execution jumps back to the scheduler (skipping the rest of loop())
 *   - Your recovery callback runs (if set)
 *   - The scheduler moves on to the next task
 *
 * This is similar to how operating systems handle runaway processes, but
 * implemented in a way that works on tiny 8-bit microcontrollers.
 *
 * =============================================================================
 * PLATFORM SUPPORT
 * =============================================================================
 *
 * This feature requires AVR with Timer2 (Arduino Uno, Nano, Mega, Pro Mini).
 * On other platforms, this example will demonstrate what WOULD happen if a
 * task blocks - the entire scheduler freezes until the task returns.
 *
 * =============================================================================
 * THIS EXAMPLE
 * =============================================================================
 *
 * We create two tasks:
 *   - slowTask: Deliberately blocks for 500ms (simulating stuck hardware)
 *               On AVR: has 100ms timeout, gets aborted repeatedly
 *               On other platforms: blocks the entire scheduler
 *   - fastTask: Should run every 200ms, proving scheduler health
 *
 * On AVR: Recovery callback counts aborts and stops slowTask after 3 failures.
 * On other platforms: You'll see slowTask block everything for 500ms each time.
 */

#include <Arda.h>

// =============================================================================
// Global state for tracking and demonstration
// =============================================================================

int8_t slowTaskId = -1;      // Need this to stop the task from recovery callback
int recoveryCount = 0;       // How many times has slowTask been aborted? (AVR only)
int slowLoopCount = 0;       // How many times has slowTask started its loop?
int fastLoopCount = 0;       // How many times has fastTask completed its loop?

// =============================================================================
// Timeout Callback (informational) - AVR only
// =============================================================================
#ifdef ARDA_TASK_RECOVERY
void onTimeout(int8_t taskId, uint32_t actualDurationMs) {
    Serial.print(F("TIMEOUT: Task "));
    Serial.print(OS.getTaskName(taskId));
    Serial.print(F(" took "));
    Serial.print(actualDurationMs);
    Serial.println(F("ms (exceeded limit)"));
}
#endif

// =============================================================================
// Recovery Callback (AVR only)
// =============================================================================
#ifdef ARDA_TASK_RECOVERY
void slowTask_recover() {
    recoveryCount++;

    Serial.print(F("RECOVERY #"));
    Serial.print(recoveryCount);
    Serial.println(F(": slowTask was forcibly aborted"));

    // Example: Take corrective action after repeated failures
    if (recoveryCount >= 3) {
        Serial.println(F("  -> 3 failures detected, stopping misbehaving task"));
        OS.stopTask(slowTaskId);
    }
}
#endif

// =============================================================================
// Slow Task (deliberately misbehaves)
// =============================================================================

void slowTask_setup() {
    Serial.println(F("slowTask: Initialized (simulating unreliable hardware)"));
}

void slowTask_loop() {
    slowLoopCount++;
    Serial.print(F("slowTask: Attempting operation #"));
    Serial.println(slowLoopCount);

    // Simulate waiting for unresponsive hardware
    // On AVR with task recovery: This will be forcibly aborted after 100ms
    // On other platforms: This blocks the ENTIRE scheduler for 500ms
    delay(500);

    // With hardware abort: You will never see this (task is aborted before here)
    // Without hardware abort: You will see this every time
    Serial.println(F("slowTask: Operation completed (blocking finished)"));

    // Without hardware abort, stop after 3 iterations to end the demo
    if (!Arda::isTaskRecoveryAvailable() && slowLoopCount >= 3) {
        Serial.println(F("  -> Stopping slowTask after 3 iterations"));
        OS.stopTask(slowTaskId);
    }
}

// =============================================================================
// Fast Task (well-behaved)
// =============================================================================

void fastTask_setup() {
    Serial.println(F("fastTask: Initialized"));
}

void fastTask_loop() {
    fastLoopCount++;

    // Only print first few iterations to avoid flooding serial
    if (fastLoopCount <= 10 || fastLoopCount % 10 == 0) {
        Serial.print(F("fastTask: Loop #"));
        Serial.println(fastLoopCount);
    }
}

// =============================================================================
// Arduino Setup
// =============================================================================

void setup() {
    Serial.begin(115200);
    while (!Serial) { }  // Wait for USB serial on Leonardo/Micro
    delay(100);

    Serial.println(F("\n========================================"));
    Serial.println(F("   Arda Task Recovery Demonstration"));
    Serial.println(F("========================================\n"));

#ifdef ARDA_TASK_RECOVERY
    Serial.print(F("ARDA_TASK_RECOVERY: Enabled"));
    if (Arda::isTaskRecoveryAvailable()) {
        Serial.println(F(" (hardware abort available)"));
        Serial.println(F(""));
        Serial.println(F("slowTask will be forcibly aborted after 100ms,"));
        Serial.println(F("while fastTask continues running normally."));
    } else {
        Serial.println(F(" (soft only - no hardware abort)"));
        Serial.println(F(""));
        Serial.println(F("Timeout callback will fire, but tasks cannot be"));
        Serial.println(F("forcibly aborted. slowTask will block for 500ms."));
    }
#else
    Serial.println(F("ARDA_TASK_RECOVERY: Not compiled in"));
    Serial.println(F(""));
    Serial.println(F("WARNING: slowTask will BLOCK the entire scheduler"));
    Serial.println(F("for 500ms each iteration. Watch how fastTask stalls."));
    Serial.println(F("This demonstrates WHY task recovery matters!"));
#endif
    Serial.println();

#ifdef ARDA_TASK_RECOVERY
    // Register the timeout callback
    OS.setTimeoutCallback(onTimeout);

    // Create the slow task with timeout and recovery
    slowTaskId = OS.createTask(
        "slowTask",           // Task name
        slowTask_setup,       // Setup callback
        slowTask_loop,        // Loop callback
        1000,                 // Run every 1000ms
        nullptr,              // No teardown
        true,                 // Auto-start
        TaskPriority::Normal, // Normal priority
        100,                  // TIMEOUT: Abort if loop() takes > 100ms
        slowTask_recover      // RECOVERY: Called after forced abort
    );
#else
    // Without task recovery, create a normal task (no timeout protection)
    slowTaskId = OS.createTask("slowTask", slowTask_setup, slowTask_loop, 1000);
#endif

    Serial.print(F("Created slowTask (id="));
    Serial.print(slowTaskId);
    Serial.println(F(")"));

    // Create the fast task (no timeout needed - it's well-behaved)
    int8_t fastId = OS.createTask("fastTask", fastTask_setup, fastTask_loop, 200);
    Serial.print(F("Created fastTask (id="));
    Serial.print(fastId);
    Serial.println(F(") with 200ms interval"));

    Serial.println();
    OS.begin();
    Serial.println(F("Scheduler started...\n"));
}

// =============================================================================
// Arduino Loop
// =============================================================================

void loop() {
    OS.run();

    // Print summary after 5 seconds
    static bool reported = false;
    if (!reported && millis() > 5000) {
        reported = true;

        Serial.println(F("\n========================================"));
        Serial.println(F("   Results After 5 Seconds"));
        Serial.println(F("========================================"));

        Serial.print(F("slowTask iterations: "));
        Serial.println(slowLoopCount);

#ifdef ARDA_TASK_RECOVERY
        Serial.print(F("slowTask aborted:    "));
        Serial.print(recoveryCount);
        Serial.println(F(" times"));
#endif

        Serial.print(F("slowTask state:      "));
        Serial.println(OS.getTaskState(slowTaskId) == TaskState::Stopped ?
                       F("STOPPED") : F("RUNNING"));

        Serial.print(F("fastTask completed:  "));
        Serial.print(fastLoopCount);
        Serial.println(F(" loops"));

        if (Arda::isTaskRecoveryAvailable()) {
            Serial.println(F("\nSUCCESS: fastTask kept running normally"));
            Serial.println(F("even while slowTask was being aborted!"));
        } else {
            Serial.println(F("\nNOTICE: On AVR with Timer2, fastTask would run"));
            Serial.println(F("~25 times by now instead of being blocked."));
            Serial.println(F("Task recovery prevents this starvation!"));
        }
        Serial.println(F("========================================\n"));

        // Stop all tasks - demonstration complete
        OS.stopAllTasks();
        Serial.println(F("Demo complete. All tasks stopped."));
    }
}
