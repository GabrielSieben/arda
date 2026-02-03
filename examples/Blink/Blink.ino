/*
 * Arda Example: Blink
 *
 * The classic Arduino "hello world" - blink an LED.
 * This shows the minimal Arda setup for a single task.
 */

#include "Arda.h"

bool ledState = false;

TASK_SETUP(blink) {
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.println(F("[Blink] LED initialized"));
}

TASK_LOOP(blink) {
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
}

void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }

    Serial.println(F("=== Arda Blink Example ===\n"));

    REGISTER_TASK_ID(blinkId, blink, 500);  // Blink every 500ms

    if (blinkId == -1) {
        Serial.print(F("ERROR: Failed to create task: "));
        Serial.println(OS.errorString(OS.getError()));
        while (1) { ; }
    }

    int8_t started = OS.begin();
    if (started == -1) {
        Serial.print(F("ERROR: OS.begin() failed: "));
        Serial.println(OS.errorString(OS.getError()));
        while (1) { ; }
    }

    Serial.println(F("Blinking LED...\n"));
}

void loop() {
    OS.run();
}
