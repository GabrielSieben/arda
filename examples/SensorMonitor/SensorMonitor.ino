/*
 * Arda Example: Sensor Monitor
 *
 * A practical IoT example: read multiple sensors at different intervals.
 * Demonstrates real-world use of cooperative multitasking where different
 * sensors need different polling rates.
 *
 * This example simulates sensors using analog reads. Connect real sensors
 * for actual measurements, or use potentiometers to test.
 *
 * Wiring (optional - works with floating pins for demo):
 *   A0 - Temperature sensor (e.g., TMP36) or potentiometer
 *   A1 - Light sensor (e.g., photoresistor) or potentiometer
 *   Pin 2 - Motion sensor (PIR) or button to ground
 */

#include "Arda.h"
#include <math.h>

#define TEMP_PIN A0
#define LIGHT_PIN A1
#define MOTION_PIN 2

// ============================================
// Temperature Task - reads every 2 seconds
// (Temperature changes slowly, no need to poll fast)
// ============================================
float lastTemp = 0;

TASK_SETUP(temperature) {
    Serial.println(F("[Temp] Monitoring A0"));
}

TASK_LOOP(temperature) {
    int raw = analogRead(TEMP_PIN);

    // Convert to temperature (assumes TMP36: 10mV/C, 500mV at 0C)
    // For demo: just show raw value if no sensor connected
    float voltage = raw * (5.0 / 1023.0);
    float tempC = (voltage - 0.5) * 100.0;

    // Only report significant changes (0.5C threshold)
    if (fabs(tempC - lastTemp) >= 0.5) {
        lastTemp = tempC;
        Serial.print(F("[Temp] "));
        Serial.print(tempC, 1);
        Serial.print(F("C (raw: "));
        Serial.print(raw);
        Serial.println(F(")"));
    }
}

// ============================================
// Light Task - reads every 500ms
// (Light can change faster, needs quicker response)
// ============================================
int lastLight = -999;  // Sentinel to force first reading to print

TASK_SETUP(light) {
    Serial.println(F("[Light] Monitoring A1"));
}

TASK_LOOP(light) {
    int raw = analogRead(LIGHT_PIN);

    // Map to percentage (0-100%)
    int percent = map(raw, 0, 1023, 0, 100);

    // Only report changes of 5% or more
    if (lastLight == -999 || abs(percent - lastLight) >= 5) {
        lastLight = percent;
        Serial.print(F("[Light] "));
        Serial.print(percent);
        Serial.print(F("% (raw: "));
        Serial.print(raw);
        Serial.println(F(")"));
    }
}

// ============================================
// Motion Task - checks every cycle
// (Motion detection needs immediate response)
// ============================================
bool lastMotion = false;
uint32_t motionTime = 0;

TASK_SETUP(motion) {
    pinMode(MOTION_PIN, INPUT_PULLUP);
    Serial.println(F("[Motion] Monitoring pin 2"));
}

TASK_LOOP(motion) {
    bool detected = (digitalRead(MOTION_PIN) == LOW);  // Active low with pullup

    if (detected != lastMotion) {
        lastMotion = detected;
        if (detected) {
            motionTime = OS.uptime();
            Serial.println(F("[Motion] DETECTED!"));
        } else {
            uint32_t duration = OS.uptime() - motionTime;
            Serial.print(F("[Motion] Cleared after "));
            Serial.print(duration);
            Serial.println(F("ms"));
        }
    }
}

// ============================================
// Status Task - periodic summary
// ============================================
TASK_SETUP(status) {
    Serial.println(F("[Status] Will report every 10 seconds"));
}

TASK_LOOP(status) {
    Serial.println(F("\n--- Status Report ---"));
    Serial.print(F("Uptime: "));
    Serial.print(OS.uptime() / 1000);
    Serial.println(F(" seconds"));

    Serial.print(F("Temp: "));
    Serial.print(lastTemp, 1);
    Serial.println(F("C"));

    Serial.print(F("Light: "));
    if (lastLight == -999) {
        Serial.println(F("(no reading yet)"));
    } else {
        Serial.print(lastLight);
        Serial.println(F("%"));
    }

    Serial.print(F("Motion: "));
    Serial.println(lastMotion ? F("ACTIVE") : F("clear"));
    Serial.println(F("---------------------\n"));
}

// ============================================
// Main
// ============================================
void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }

    Serial.println(F("=== Arda Sensor Monitor ===\n"));

    // Create tasks with appropriate intervals
    int8_t tempId = OS.createTask("temp", temperature_setup, temperature_loop, 2000);
    int8_t lightId = OS.createTask("light", light_setup, light_loop, 500);
    int8_t motionId = OS.createTask("motion", motion_setup, motion_loop, 0);  // Every cycle
    int8_t statusId = OS.createTask("status", status_setup, status_loop, 10000);

    if (tempId == -1 || lightId == -1 || motionId == -1 || statusId == -1) {
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

    Serial.print(F("Monitoring "));
    Serial.print(started);
    Serial.println(F(" sensors...\n"));
}

void loop() {
    OS.run();
}
