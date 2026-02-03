/*
 * Arda Example: Basic Multitasking
 *
 * Demonstrates running multiple tasks simultaneously:
 * - Task 1: Blinks LED at 500ms intervals
 * - Task 2: Prints to Serial every 1 second
 * - Task 3: Monitors a button (runs every cycle)
 */

#include "Arda.h"

#define LED_PIN 13
#define BUTTON_PIN 2

// ============================================
// Task 1: LED Blinker
// ============================================
bool ledState = false;

TASK_SETUP(blinker) {
    pinMode(LED_PIN, OUTPUT);
    Serial.println(F("[Blinker] Initialized"));
}

TASK_LOOP(blinker) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
}

// ============================================
// Task 2: Serial Reporter
// ============================================
TASK_SETUP(reporter) {
    Serial.println(F("[Reporter] Initialized"));
}

TASK_LOOP(reporter) {
    Serial.print(F("[Reporter] Uptime: "));
    Serial.print(OS.uptime() / 1000);
    Serial.println(F(" seconds"));
}

// ============================================
// Task 3: Button Monitor
// ============================================
bool lastButtonState = HIGH;

TASK_SETUP(button) {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.println(F("[Button] Initialized"));
}

TASK_LOOP(button) {
    bool currentState = digitalRead(BUTTON_PIN);

    if (currentState != lastButtonState) {
        lastButtonState = currentState;
        if (currentState == LOW) {
            Serial.println(F("[Button] Pressed!"));
        } else {
            Serial.println(F("[Button] Released!"));
        }
    }
}

// ============================================
// Main Arduino Setup & Loop
// ============================================
void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }

    Serial.println(F("=== Arda Starting ==="));

    // Register tasks with their intervals (in milliseconds)
    // Check return values to ensure tasks were created successfully
    REGISTER_TASK_ID(blinkerId, blinker, 500);   // Run every 500ms
    REGISTER_TASK_ID(reporterId, reporter, 1000); // Run every 1 second
    REGISTER_TASK_ID(buttonId, button, 0);        // Run every cycle (for responsive input)

    if (blinkerId == -1 || reporterId == -1 || buttonId == -1) {
        Serial.println(F("ERROR: Failed to create one or more tasks!"));
        while (1) { ; }  // Halt
    }

    // Start the OS
    int8_t started = OS.begin();
    if (started == -1) {
        Serial.println(F("ERROR: OS.begin() failed (already begun?)"));
        while (1) { ; }  // Halt
    }

    Serial.print(F("Started "));
    Serial.print(started);
    Serial.print(F(" of "));
    Serial.print(OS.getTaskCount());
    Serial.println(F(" tasks"));
    Serial.println(F("=== Arda Running ===\n"));
}

void loop() {
    // Run the scheduler
    OS.run();
}
