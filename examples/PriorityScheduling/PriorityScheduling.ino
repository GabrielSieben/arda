/*
 * Arda Example: Priority Scheduling
 *
 * Demonstrates the 16-level priority system (0-15):
 * - Higher priority tasks run before lower priority tasks
 * - Tasks at the same priority run in creation order
 * - Priority only matters when multiple tasks are ready simultaneously
 *
 * This example creates three tasks that all run every 500ms.
 * Watch the Serial output to see them always execute in priority order
 * (high -> medium -> low), regardless of creation order.
 */

#include "Arda.h"

// ============================================
// Low Priority Task (priority 4)
// ============================================
TASK_SETUP(lowPri) {
    Serial.println(F("[Low] Initialized"));
}

TASK_LOOP(lowPri) {
    Serial.println(F("  3. [Low] Running"));
}

// ============================================
// High Priority Task (priority 12)
// ============================================
TASK_SETUP(highPri) {
    Serial.println(F("[High] Initialized"));
}

TASK_LOOP(highPri) {
    Serial.println(F("  1. [High] Running"));
}

// ============================================
// Medium Priority Task (priority 8 = Normal)
// ============================================
TASK_SETUP(medPri) {
    Serial.println(F("[Medium] Initialized"));
}

TASK_LOOP(medPri) {
    Serial.println(F("  2. [Medium] Running"));
}

// ============================================
// Critical Task - demonstrates runtime priority change
// ============================================
int criticalRunCount = 0;

TASK_SETUP(critical) {
    Serial.println(F("[Critical] Initialized at Highest priority"));
}

TASK_LOOP(critical) {
    criticalRunCount++;
    Serial.print(F(">>> [Critical] Urgent work #"));
    Serial.println(criticalRunCount);

    // After 3 runs, lower our priority and slow down
    if (criticalRunCount >= 3) {
        Serial.println(F(">>> [Critical] Urgent work done, lowering priority"));
        OS.setTaskPriority(OS.getCurrentTask(), static_cast<uint8_t>(TaskPriority::Low));
        OS.setTaskInterval(OS.getCurrentTask(), 2000);  // Slow down to every 2s
    }
}

// ============================================
// Main Arduino Setup & Loop
// ============================================
void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }

    Serial.println(F("=== Arda Priority Scheduling Demo ===\n"));

    // Create tasks in "wrong" order to show priority overrides creation order
    // Using the priority-enabled createTask overload:
    //   createTask(name, setup, loop, interval, priority, teardown, autoStart)

    int8_t lowId = OS.createTask("lowPri", lowPri_setup, lowPri_loop,
                                  500, static_cast<uint8_t>(TaskPriority::Low));

    int8_t highId = OS.createTask("highPri", highPri_setup, highPri_loop,
                                   500, static_cast<uint8_t>(TaskPriority::High));

    int8_t medId = OS.createTask("medPri", medPri_setup, medPri_loop,
                                  500, static_cast<uint8_t>(TaskPriority::Normal));

    // Critical task starts at highest priority, runs every cycle initially
    int8_t critId = OS.createTask("crit", critical_setup, critical_loop,
                                   0, static_cast<uint8_t>(TaskPriority::Highest));

    if (lowId == -1 || highId == -1 || medId == -1 || critId == -1) {
        Serial.println(F("ERROR: Failed to create tasks!"));
        Serial.print(F("Last error: "));
        Serial.println(OS.errorString(OS.getError()));
        while (1) { ; }
    }

    // Print the priorities we assigned
    Serial.println(F("Task priorities:"));
    Serial.print(F("  lowPri:   ")); Serial.println(OS.getTaskPriority(lowId));
    Serial.print(F("  highPri:  ")); Serial.println(OS.getTaskPriority(highId));
    Serial.print(F("  medPri:   ")); Serial.println(OS.getTaskPriority(medId));
    Serial.print(F("  critical: ")); Serial.println(OS.getTaskPriority(critId));
    Serial.println();

    // Start the scheduler
    int8_t started = OS.begin();
    Serial.print(F("Started "));
    Serial.print(started);
    Serial.println(F(" tasks\n"));

    Serial.println(F("Watch the execution order - higher priority always runs first!\n"));
}

void loop() {
    static uint32_t lastPrint = 0;
    uint32_t now = millis();

    // Print a separator every 500ms to show each scheduling cycle
    if (now - lastPrint >= 500) {
        lastPrint = now;
        Serial.print(F("\n--- Cycle at "));
        Serial.print(now / 1000.0, 1);
        Serial.println(F("s ---"));
    }

    OS.run();
}
