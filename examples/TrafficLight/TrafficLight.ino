/*
 * Arda Example: Traffic Light
 *
 * A state machine example using tasks. Demonstrates how to use
 * setTaskInterval() to change timing based on state, and how
 * tasks can work together.
 *
 * Wiring:
 *   Pin 11 - Red LED (with resistor)
 *   Pin 12 - Yellow LED (with resistor)
 *   LED_BUILTIN - Green LED (built-in LED)
 *   Pin 2  - Button to ground (pedestrian crossing request)
 */

#include "Arda.h"

#define RED_PIN 11
#define YELLOW_PIN 12
#define GREEN_PIN LED_BUILTIN
#define BUTTON_PIN 2

// Traffic light states
enum class LightState : uint8_t {
    Green,
    Yellow,
    Red,
    PedestrianWalk    // Extended red for pedestrians
};

LightState currentState;
bool pedestrianRequest = false;
int8_t lightTaskId = -1;

// Timing constants (milliseconds)
const uint32_t GREEN_TIME = 5000;
const uint32_t YELLOW_TIME = 2000;
const uint32_t RED_TIME = 5000;
const uint32_t WALK_TIME = 8000;

// ============================================
// Traffic Light Controller Task
// ============================================
void setLights(bool red, bool yellow, bool green) {
    digitalWrite(RED_PIN, red ? HIGH : LOW);
    digitalWrite(YELLOW_PIN, yellow ? HIGH : LOW);
    digitalWrite(GREEN_PIN, green ? HIGH : LOW);
}

void light_setup() {
    pinMode(RED_PIN, OUTPUT);
    pinMode(YELLOW_PIN, OUTPUT);
    pinMode(GREEN_PIN, OUTPUT);

    // Start with red
    currentState = LightState::Red;
    setLights(true, false, false);
    Serial.println(F("[Light] RED"));
}

void changeInterval(uint32_t newInterval) {
    if (!OS.setTaskInterval(lightTaskId, newInterval)) {
        Serial.print(F("ERROR: setTaskInterval failed: "));
        Serial.println(OS.errorString(OS.getError()));
    }
}

void light_loop() {
    // State machine transitions
    switch (currentState) {
        case LightState::Red:
            // Check for pedestrian request before going green
            if (pedestrianRequest) {
                pedestrianRequest = false;
                currentState = LightState::PedestrianWalk;
                Serial.println(F("[Light] WALK - pedestrian crossing"));
                changeInterval(WALK_TIME);
            } else {
                currentState = LightState::Green;
                setLights(false, false, true);
                Serial.println(F("[Light] GREEN"));
                changeInterval(GREEN_TIME);
            }
            break;

        case LightState::PedestrianWalk:
            // After walk period, go to green
            currentState = LightState::Green;
            setLights(false, false, true);
            Serial.println(F("[Light] GREEN"));
            changeInterval(GREEN_TIME);
            break;

        case LightState::Green:
            currentState = LightState::Yellow;
            setLights(false, true, false);
            Serial.println(F("[Light] YELLOW"));
            changeInterval(YELLOW_TIME);
            break;

        case LightState::Yellow:
            currentState = LightState::Red;
            setLights(true, false, false);
            Serial.println(F("[Light] RED"));
            changeInterval(RED_TIME);
            break;
    }
}

// ============================================
// Pedestrian Button Task
// ============================================
bool lastButtonState = HIGH;

void button_setup() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.println(F("[Button] Ready (press for crosswalk)"));
}

void button_loop() {
    bool buttonState = digitalRead(BUTTON_PIN);

    // Detect button press (falling edge)
    if (buttonState == LOW && lastButtonState == HIGH) {
        if (!pedestrianRequest && currentState != LightState::PedestrianWalk) {
            pedestrianRequest = true;
            Serial.println(F("[Button] Crosswalk requested!"));
        }
    }

    lastButtonState = buttonState;
}

// ============================================
// Main
// ============================================
void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }

    Serial.println(F("=== Arda Traffic Light ===\n"));

    // Create tasks
    lightTaskId = OS.createTask("light", light_setup, light_loop, RED_TIME);
    int8_t buttonId = OS.createTask("button", button_setup, button_loop, 0);

    if (lightTaskId == -1 || buttonId == -1) {
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

    Serial.println(F("Traffic light running. Press button for crosswalk.\n"));
}

void loop() {
    OS.run();
}
