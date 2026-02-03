/*
 * Arda Example: Blink
 *
 * The classic Arduino "hello world" - blink an LED.
 * This shows the minimal Arda setup for a single task.
 */

#include "Arda.h"

bool ledState = false;

void blink_setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.println(F("[Blink] LED initialized"));
}

void blink_loop() {
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
}

void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }

    Serial.println(F("=== Arda Blink Example ===\n"));

    int8_t blinkId = OS.createTask("blink", blink_setup, blink_loop, 500);

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
