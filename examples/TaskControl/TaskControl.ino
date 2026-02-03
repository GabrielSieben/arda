/*
 * Arda Example: Task Control
 *
 * Demonstrates dynamic task management:
 * - Starting/stopping tasks via serial commands
 * - Pausing/resuming tasks
 * - Querying task status
 *
 * Serial Commands:
 *   p <id> - Pause task
 *   r <id> - Resume task
 *   s <id> - Stop task
 *   t <id> - Start task
 *   l      - List all tasks
 */

#include "Arda.h"

#define LED_PIN 13

// ============================================
// Task 1: Fast Blinker (100ms)
// ============================================
bool led1State = false;

void fastBlink_setup() {
    pinMode(LED_PIN, OUTPUT);
}

void fastBlink_loop() {
    led1State = !led1State;
    digitalWrite(LED_PIN, led1State ? HIGH : LOW);
}

// ============================================
// Task 2: Counter
// ============================================
uint32_t counter = 0;

void counter_setup() {
    counter = 0;
}

void counter_loop() {
    counter++;
    Serial.print(F("Count: "));
    Serial.println(counter);
}

// ============================================
// Task 3: Command Handler (runs every cycle)
// ============================================
void commandHandler_setup() {
    Serial.println(F("Commands: p/r/s/t <id>, l (list)"));
}

void commandHandler_loop() {
    if (Serial.available() > 0) {
        char cmd = Serial.read();
        int8_t taskId = -1;

        // Read task ID if present
        if (Serial.available() > 0) {
            Serial.read(); // consume space
            taskId = Serial.parseInt();
        }

        switch (cmd) {
            case 'p':
                if (OS.pauseTask(taskId)) {
                    Serial.print(F("Paused task "));
                    Serial.println(taskId);
                }
                break;

            case 'r':
                if (OS.resumeTask(taskId)) {
                    Serial.print(F("Resumed task "));
                    Serial.println(taskId);
                }
                break;

            case 's':
                if (OS.stopTask(taskId) != StopResult::Failed) {
                    Serial.print(F("Stopped task "));
                    Serial.println(taskId);
                }
                break;

            case 't':
                if (OS.startTask(taskId)) {
                    Serial.print(F("Started task "));
                    Serial.println(taskId);
                }
                break;

            case 'l':
                listTasks();
                break;
        }

        // Clear remaining input
        while (Serial.available()) Serial.read();
    }
}

void listTasks() {
    Serial.println(F("\n=== Task List ==="));
    // Use getSlotCount() to iterate all slots (including deleted ones)
    // Using getTaskCount() would skip valid tasks at higher indices
    for (int8_t i = 0; i < OS.getSlotCount(); i++) {
        if (!OS.isValidTask(i)) {
            continue;  // Skip deleted slots
        }

        const char* name = OS.getTaskName(i);
        Serial.print(F("["));
        Serial.print(i);
        Serial.print(F("] "));
        Serial.print(name);
        Serial.print(F(" - "));

        switch (OS.getTaskState(i)) {
            case TaskState::Running: Serial.print(F("RUNNING")); break;
            case TaskState::Paused:  Serial.print(F("PAUSED"));  break;
            case TaskState::Stopped: Serial.print(F("STOPPED")); break;
            case TaskState::Invalid: Serial.print(F("DELETED")); break;
        }

        Serial.print(F(" (runs: "));
        Serial.print(OS.getTaskRunCount(i));
        Serial.println(F(")"));
    }
    Serial.println();
}

// ============================================
// Main
// ============================================
void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }

    Serial.println(F("=== Arda Task Control Demo ===\n"));

    // Register tasks and check for errors
    int8_t id1 = OS.createTask("fastBlink", fastBlink_setup, fastBlink_loop, 100);
    int8_t id2 = OS.createTask("counter", counter_setup, counter_loop, 2000);
    int8_t id3 = OS.createTask("cmdHandler", commandHandler_setup, commandHandler_loop, 0);

    if (id1 == -1 || id2 == -1 || id3 == -1) {
        Serial.println(F("ERROR: Failed to create one or more tasks!"));
        while (1) { ; }
    }

    int8_t started = OS.begin();
    if (started == -1) {
        Serial.println(F("ERROR: OS.begin() failed!"));
        while (1) { ; }
    }

    listTasks();
}

void loop() {
    OS.run();
}
