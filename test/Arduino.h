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

// Base Stream class (Arduino provides this)
class Stream {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual size_t write(uint8_t) = 0;

    // print() overloads - covers all common Arduino types
    virtual void print(const char* s) { printf("%s", s); }
    virtual void print(char c) { printf("%c", c); }
    virtual void print(int n) { printf("%d", n); }
    virtual void print(unsigned int n) { printf("%u", n); }
    virtual void print(long n) { printf("%ld", n); }
    virtual void print(unsigned long n) { printf("%lu", n); }
    virtual void print(int8_t n) { printf("%d", (int)n); }
    virtual void print(uint8_t n) { printf("%u", (unsigned)n); }
    virtual void print(double n, int precision = 2) { printf("%.*f", precision, n); }

    // println() overloads
    virtual void println() { printf("\n"); }
    virtual void println(const char* s) { printf("%s\n", s); }
    virtual void println(char c) { printf("%c\n", c); }
    virtual void println(int n) { printf("%d\n", n); }
    virtual void println(unsigned int n) { printf("%u\n", n); }
    virtual void println(long n) { printf("%ld\n", n); }
    virtual void println(unsigned long n) { printf("%lu\n", n); }
    virtual void println(int8_t n) { printf("%d\n", (int)n); }
    virtual void println(uint8_t n) { printf("%u\n", (unsigned)n); }
    virtual void println(double n, int precision = 2) { printf("%.*f\n", precision, n); }

    virtual int parseInt() { return 0; }

    virtual ~Stream() {}
};

// Mock Serial - provides overloads for common Arduino types
// Note: Avoid int32_t/uint32_t overloads as they may alias int/unsigned int on some platforms
class MockSerial : public Stream {
public:
    void begin(long) {}

    // Input methods (mock - return defaults by default, can be overridden in tests)
    int available() override { return 0; }
    int read() override { return 0; }
    size_t write(uint8_t) override { return 1; }

    operator bool() { return true; }
};

// MockStream with injectable input for testing shell
class MockStream : public Stream {
public:
    const char* inputBuffer;
    size_t inputPos;
    char outputBuffer[1024];
    size_t outputPos;

    MockStream() : inputBuffer(nullptr), inputPos(0), outputPos(0) {
        outputBuffer[0] = '\0';
    }

    void setInput(const char* input) {
        inputBuffer = input;
        inputPos = 0;
    }

    void clearOutput() {
        outputPos = 0;
        outputBuffer[0] = '\0';
    }

    int available() override {
        if (!inputBuffer) return 0;
        return inputBuffer[inputPos] != '\0' ? 1 : 0;
    }

    int read() override {
        if (!inputBuffer || inputBuffer[inputPos] == '\0') return -1;
        return inputBuffer[inputPos++];
    }

    size_t write(uint8_t c) override {
        if (outputPos < sizeof(outputBuffer) - 1) {
            outputBuffer[outputPos++] = c;
            outputBuffer[outputPos] = '\0';
        }
        return 1;
    }

    // Override print/println to capture output
    void print(const char* s) override {
        while (*s && outputPos < sizeof(outputBuffer) - 1) {
            outputBuffer[outputPos++] = *s++;
        }
        outputBuffer[outputPos] = '\0';
    }
    void print(char c) override { write(c); }
    void print(int n) override { outputPos += snprintf(outputBuffer + outputPos, sizeof(outputBuffer) - outputPos, "%d", n); }
    void print(unsigned int n) override { outputPos += snprintf(outputBuffer + outputPos, sizeof(outputBuffer) - outputPos, "%u", n); }
    void print(long n) override { outputPos += snprintf(outputBuffer + outputPos, sizeof(outputBuffer) - outputPos, "%ld", n); }
    void print(unsigned long n) override { outputPos += snprintf(outputBuffer + outputPos, sizeof(outputBuffer) - outputPos, "%lu", n); }
    void print(int8_t n) override { outputPos += snprintf(outputBuffer + outputPos, sizeof(outputBuffer) - outputPos, "%d", (int)n); }
    void print(uint8_t n) override { outputPos += snprintf(outputBuffer + outputPos, sizeof(outputBuffer) - outputPos, "%u", (unsigned)n); }

    void println() override { print("\n"); }
    void println(const char* s) override { print(s); print("\n"); }
    void println(char c) override { print(c); print("\n"); }
    void println(int n) override { print(n); print("\n"); }
    void println(unsigned int n) override { print(n); print("\n"); }
    void println(long n) override { print(n); print("\n"); }
    void println(unsigned long n) override { print(n); print("\n"); }
    void println(int8_t n) override { print(n); print("\n"); }
    void println(uint8_t n) override { print(n); print("\n"); }

    const char* getOutput() const { return outputBuffer; }
};

extern MockSerial Serial;

#endif
