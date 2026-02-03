/*
 * Arda Example: Task Control
 *
 * Demonstrates dynamic task management via serial commands.
 * Shows how to pause, resume, stop, and start tasks at runtime.
 *
 * Serial Commands:
 *   p <id> - Pause task
 *   r <id> - Resume task
 *   s <id> - Stop task
 *   t <id> - Start task
 *   l      - List all tasks
 *   h      - Show help
 */

#include "Arda.h"

// ============================================
// Task 1: LED Blinker
// ============================================
bool ledState = false;

TASK_SETUP(blinker) {
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.println(F("[Blinker] Started"));
}

TASK_LOOP(blinker) {
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
}

TASK_TEARDOWN(blinker) {
    digitalWrite(LED_BUILTIN, LOW);  // Turn off LED when stopped
    Serial.println(F("[Blinker] Stopped, LED off"));
}

// ============================================
// Task 2: Heartbeat (prints periodically)
// ============================================
uint32_t heartbeatCount = 0;

TASK_SETUP(heartbeat) {
    heartbeatCount = 0;
    Serial.println(F("[Heartbeat] Started"));
}

TASK_LOOP(heartbeat) {
    heartbeatCount++;
    Serial.print(F("[Heartbeat] #"));
    Serial.print(heartbeatCount);
    Serial.print(F(" at "));
    Serial.print(OS.uptime() / 1000);
    Serial.println(F("s"));
}

// ============================================
// Task 3: Command Handler
// ============================================
char cmdBuffer[16];
uint8_t cmdIndex = 0;

TASK_SETUP(commander) {
    cmdIndex = 0;
    printHelp();
}

TASK_LOOP(commander) {
    while (Serial.available()) {
        char c = Serial.read();

        if (c == '\n' || c == '\r') {
            if (cmdIndex > 0) {
                cmdBuffer[cmdIndex] = '\0';
                processCommand();
                cmdIndex = 0;
            }
        } else if (cmdIndex < sizeof(cmdBuffer) - 1) {
            cmdBuffer[cmdIndex++] = c;
        }
    }
}

void printHelp() {
    Serial.println(F("\n=== Commands ==="));
    Serial.println(F("  p <id> - Pause task"));
    Serial.println(F("  r <id> - Resume task"));
    Serial.println(F("  s <id> - Stop task"));
    Serial.println(F("  t <id> - Start task"));
    Serial.println(F("  l      - List tasks"));
    Serial.println(F("  h      - This help"));
    Serial.println();
}

void processCommand() {
    char cmd = cmdBuffer[0];
    int8_t taskId = -1;

    // Parse task ID if present (skip space after command)
    if (cmdIndex >= 3 && cmdBuffer[1] == ' ') {
        // Validate that we have a digit (atoi returns 0 for non-numeric, but 0 is a valid task ID)
        char firstDigit = cmdBuffer[2];
        if (firstDigit >= '0' && firstDigit <= '9') {
            taskId = atoi(&cmdBuffer[2]);
        }
    }

    switch (cmd) {
        case 'p':
            if (taskId < 0) {
                Serial.println(F("Usage: p <task_id>"));
            } else if (OS.pauseTask(taskId)) {
                Serial.print(F("Paused task "));
                Serial.println(taskId);
            } else {
                Serial.print(F("ERROR: Cannot pause task "));
                Serial.print(taskId);
                Serial.print(F(" - "));
                Serial.println(OS.errorString(OS.getError()));
            }
            break;

        case 'r':
            if (taskId < 0) {
                Serial.println(F("Usage: r <task_id>"));
            } else if (OS.resumeTask(taskId)) {
                Serial.print(F("Resumed task "));
                Serial.println(taskId);
            } else {
                Serial.print(F("ERROR: Cannot resume task "));
                Serial.print(taskId);
                Serial.print(F(" - "));
                Serial.println(OS.errorString(OS.getError()));
            }
            break;

        case 's': {
            if (taskId < 0) {
                Serial.println(F("Usage: s <task_id>"));
            } else {
                StopResult result = OS.stopTask(taskId);
                if (result == StopResult::Failed) {
                    Serial.print(F("ERROR: Cannot stop task "));
                    Serial.print(taskId);
                    Serial.print(F(" - "));
                    Serial.println(OS.errorString(OS.getError()));
                } else {
                    Serial.print(F("Stopped task "));
                    Serial.println(taskId);
                    if (result == StopResult::TeardownSkipped) {
                        Serial.println(F("  (teardown skipped)"));
                    }
                }
            }
            break;
        }

        case 't':
            if (taskId < 0) {
                Serial.println(F("Usage: t <task_id>"));
            } else if (OS.startTask(taskId)) {
                Serial.print(F("Started task "));
                Serial.println(taskId);
            } else {
                Serial.print(F("ERROR: Cannot start task "));
                Serial.print(taskId);
                Serial.print(F(" - "));
                Serial.println(OS.errorString(OS.getError()));
            }
            break;

        case 'l':
            listTasks();
            break;

        case 'h':
        case '?':
            printHelp();
            break;

        default:
            Serial.print(F("Unknown command: "));
            Serial.println(cmd);
            Serial.println(F("Type 'h' for help"));
            break;
    }
}

void listTasks() {
    Serial.println(F("\n=== Tasks ==="));
    Serial.println(F("ID  Name        State    Runs"));
    Serial.println(F("--  ----        -----    ----"));

    for (int8_t i = 0; i < OS.getSlotCount(); i++) {
        if (!OS.isValidTask(i)) continue;

        Serial.print(i);
        Serial.print(F("   "));

        const char* name = OS.getTaskName(i);
        Serial.print(name);

        // Pad name to 12 chars
        size_t nameLen = strlen(name);
        for (size_t j = nameLen; j < 12; j++) {
            Serial.print(' ');
        }

        switch (OS.getTaskState(i)) {
            case TaskState::Running: Serial.print(F("RUNNING  ")); break;
            case TaskState::Paused:  Serial.print(F("PAUSED   ")); break;
            case TaskState::Stopped: Serial.print(F("STOPPED  ")); break;
            default:                 Serial.print(F("???      ")); break;
        }

        Serial.println(OS.getTaskRunCount(i));
    }
    Serial.println();
}

// ============================================
// Main
// ============================================
void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }

    Serial.println(F("=== Arda Task Control Demo ==="));

    // Create tasks with teardown for blinker
    int8_t blinkId = OS.createTask("blinker", blinker_setup, blinker_loop,
                                    200, blinker_teardown);
    int8_t heartId = OS.createTask("heart", heartbeat_setup, heartbeat_loop, 2000);
    int8_t cmdId = OS.createTask("cmdr", commander_setup, commander_loop, 0);

    if (blinkId == -1 || heartId == -1 || cmdId == -1) {
        Serial.print(F("ERROR: Task creation failed: "));
        Serial.println(OS.errorString(OS.getError()));
        while (1) { ; }
    }

    int8_t started = OS.begin();
    if (started == -1) {
        Serial.print(F("ERROR: OS.begin() failed: "));
        Serial.println(OS.errorString(OS.getError()));
        while (1) { ; }
    }

    Serial.print(F("Started "));
    Serial.print(started);
    Serial.println(F(" tasks"));

    listTasks();
}

void loop() {
    OS.run();
}
