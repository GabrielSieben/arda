// Compile test for BasicMultitasking example
#include <cstdio>
#include "Arduino.h"

// Define mock variables
uint32_t _mockMillis = 0;  // 32-bit to simulate Arduino's millis() overflow
MockSerial Serial;

// Disable shell for example compile test
#define ARDA_NO_SHELL
#include "../Arda.h"
#include "../Arda.cpp"

#define LED_PIN 13
#define BUTTON_PIN 2

// ============================================
// Task 1: LED Blinker
// ============================================
bool ledState = false;

TASK_SETUP(blinker) {
    pinMode(LED_PIN, OUTPUT);
    Serial.println("[Blinker] Initialized");
}

TASK_LOOP(blinker) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
}

// ============================================
// Task 2: Serial Reporter
// ============================================
TASK_SETUP(reporter) {
    Serial.println("[Reporter] Initialized");
}

TASK_LOOP(reporter) {
    Serial.print("[Reporter] Uptime: ");
    Serial.print(OS.uptime() / 1000);
    Serial.println(" seconds");
}

// ============================================
// Task 3: Button Monitor
// ============================================
bool lastButtonState = HIGH;

TASK_SETUP(button) {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.println("[Button] Initialized");
}

TASK_LOOP(button) {
    bool currentState = digitalRead(BUTTON_PIN);

    if (currentState != lastButtonState) {
        lastButtonState = currentState;
        if (currentState == LOW) {
            Serial.println("[Button] Pressed!");
        } else {
            Serial.println("[Button] Released!");
        }
    }
}

// ============================================
// Main
// ============================================
void setup() {
    Serial.begin(115200);

    Serial.println("=== Arda Starting ===");

    REGISTER_TASK(blinker, 500);
    REGISTER_TASK(reporter, 1000);
    REGISTER_TASK(button, 0);

    OS.begin();

    Serial.print("Registered ");
    Serial.print(OS.getTaskCount());
    Serial.println(" tasks");
    Serial.println("=== Arda Running ===\n");
}

void loop() {
    OS.run();
}

int main() {
    setup();
    // Simulate a few scheduler cycles with time advancing
    for (int i = 0; i < 10; i++) {
        advanceMockMillis(500);  // Advance 500ms each cycle
        loop();
    }
    printf("\nExample compiled and ran successfully!\n");
    return 0;
}
