// Mock Arduino.h for compile testing
#ifndef ARDUINO_H
#define ARDUINO_H
#define Arduino_h

#include <cstdint>
#include <cstdio>
#include <cstring>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

typedef uint8_t byte;

// Mock F() macro - on real Arduino this stores strings in PROGMEM
// In testing, it's a no-op that just returns the string
#define F(str) (str)

// Controllable mock time for testing
// Use uint32_t to simulate Arduino's 32-bit millis() for overflow testing
// Declared extern to avoid multiple definition issues
extern uint32_t _mockMillis;

inline void setMockMillis(uint32_t t) {
    _mockMillis = t;
}

inline void advanceMockMillis(uint32_t delta) {
    _mockMillis += delta;
}

// Return as unsigned long to match Arduino API, but overflow at 32 bits
inline unsigned long millis() {
    return _mockMillis;
}

// Microseconds counter (derived from millis for simplicity)
inline unsigned long micros() {
    return _mockMillis * 1000UL;
}

// Digital I/O
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }

// Analog I/O
inline int analogRead(int) { return 0; }
inline void analogWrite(int, int) {}
inline void analogReference(int) {}

// Timing
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}

// Math utilities (commonly used in Arduino code)
// Note: This matches Arduino's actual implementation which uses integer math.
// The order of operations matters to minimize overflow: multiply before divide.
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    // Match Arduino's exact implementation
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
inline int constrain(int x, int a, int b) {
    return (x < a) ? a : ((x > b) ? b : x);
}
inline long constrain(long x, long a, long b) {
    return (x < a) ? a : ((x > b) ? b : x);
}

// Random
inline long random(long /* max */) { return 0; }
inline long random(long min, long /* max */) { return min; }
inline void randomSeed(unsigned long) {}

// Bits and bytes
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))
#define bit(b) (1UL << (b))
#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))

// Mock Serial - provides overloads for common Arduino types
// Note: Avoid int32_t/uint32_t overloads as they may alias int/unsigned int on some platforms
class MockSerial {
public:
    void begin(long) {}

    // print() overloads - covers all common Arduino types
    void print(const char* s) { printf("%s", s); }
    void print(char c) { printf("%c", c); }
    void print(int n) { printf("%d", n); }
    void print(unsigned int n) { printf("%u", n); }
    void print(long n) { printf("%ld", n); }
    void print(unsigned long n) { printf("%lu", n); }
    void print(int8_t n) { printf("%d", (int)n); }
    void print(uint8_t n) { printf("%u", (unsigned)n); }
    void print(double n, int precision = 2) { printf("%.*f", precision, n); }

    // println() overloads
    void println() { printf("\n"); }
    void println(const char* s) { printf("%s\n", s); }
    void println(char c) { printf("%c\n", c); }
    void println(int n) { printf("%d\n", n); }
    void println(unsigned int n) { printf("%u\n", n); }
    void println(long n) { printf("%ld\n", n); }
    void println(unsigned long n) { printf("%lu\n", n); }
    void println(int8_t n) { printf("%d\n", (int)n); }
    void println(uint8_t n) { printf("%u\n", (unsigned)n); }
    void println(double n, int precision = 2) { printf("%.*f\n", precision, n); }

    // Input methods (mock - return defaults)
    int available() { return 0; }
    char read() { return 0; }
    int parseInt() { return 0; }

    operator bool() { return true; }
};

extern MockSerial Serial;

#endif
