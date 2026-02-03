// Compile test for PriorityScheduling example
#include <cstdio>
#include "Arduino.h"

// Define mock variables
uint32_t _mockMillis = 0;
MockSerial Serial;

// Disable shell for priority example compile test
#define ARDA_NO_SHELL
#include "../Arda.h"
#include "../Arda.cpp"

// ============================================
// Low Priority Task (TaskPriority::Low)
// ============================================
TASK_SETUP(lowPri) {
    Serial.println("[Low] Initialized");
}

TASK_LOOP(lowPri) {
    Serial.println("  3. [Low] Running");
}

// ============================================
// High Priority Task (TaskPriority::High)
// ============================================
TASK_SETUP(highPri) {
    Serial.println("[High] Initialized");
}

TASK_LOOP(highPri) {
    Serial.println("  1. [High] Running");
}

// ============================================
// Medium Priority Task (TaskPriority::Normal)
// ============================================
TASK_SETUP(medPri) {
    Serial.println("[Medium] Initialized");
}

TASK_LOOP(medPri) {
    Serial.println("  2. [Medium] Running");
}

// ============================================
// Critical Task - demonstrates runtime priority change
// ============================================
int criticalRunCount = 0;

TASK_SETUP(critical) {
    Serial.println("[Critical] Initialized at Highest priority");
}

TASK_LOOP(critical) {
    criticalRunCount++;
    Serial.print(">>> [Critical] Urgent work #");
    Serial.println(criticalRunCount);

    // After 3 runs, lower our priority and slow down
    if (criticalRunCount >= 3) {
        Serial.println(">>> [Critical] Urgent work done, lowering priority");
        OS.setTaskPriority(OS.getCurrentTask(), TaskPriority::Low);
        OS.setTaskInterval(OS.getCurrentTask(), 2000);
    }
}

// ============================================
// Main
// ============================================
void setup() {
    Serial.begin(115200);

    Serial.println("=== Arda Priority Scheduling Demo ===\n");

    // Create tasks in "wrong" order to show priority overrides creation order
    // Using full priority overload: (name, setup, loop, interval, teardown, autoStart, priority)
    int8_t lowId = OS.createTask("lowPri", lowPri_setup, lowPri_loop,
                                  500, nullptr, true, TaskPriority::Low);

    int8_t highId = OS.createTask("highPri", highPri_setup, highPri_loop,
                                   500, nullptr, true, TaskPriority::High);

    int8_t medId = OS.createTask("medPri", medPri_setup, medPri_loop,
                                  500, nullptr, true, TaskPriority::Normal);

    // Critical task starts at highest priority, runs every cycle initially
    int8_t critId = OS.createTask("crit", critical_setup, critical_loop,
                                   0, nullptr, true, TaskPriority::Highest);

    if (lowId == -1 || highId == -1 || medId == -1 || critId == -1) {
        Serial.println("ERROR: Failed to create tasks!");
        Serial.print("Last error: ");
        Serial.println(OS.errorString(OS.getError()));
        return;
    }

    // Print the priorities we assigned
    Serial.println("Task priorities:");
    Serial.print("  lowPri:   "); Serial.println(static_cast<uint8_t>(OS.getTaskPriority(lowId)));
    Serial.print("  highPri:  "); Serial.println(static_cast<uint8_t>(OS.getTaskPriority(highId)));
    Serial.print("  medPri:   "); Serial.println(static_cast<uint8_t>(OS.getTaskPriority(medId)));
    Serial.print("  critical: "); Serial.println(static_cast<uint8_t>(OS.getTaskPriority(critId)));
    Serial.println();

    // Start the scheduler
    int8_t started = OS.begin();
    Serial.print("Started ");
    Serial.print(started);
    Serial.println(" tasks\n");

    Serial.println("Watch the execution order - higher priority always runs first!\n");
}

void loop() {
    OS.run();
}

int main() {
    setup();

    // Simulate several scheduler cycles
    for (int i = 0; i < 8; i++) {
        printf("\n--- Cycle %d (t=%lums) ---\n", i, (unsigned long)millis());
        loop();
        advanceMockMillis(500);
    }

    printf("\n=== Priority Example Test Passed ===\n");
    return 0;
}
