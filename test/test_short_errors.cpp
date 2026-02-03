// Test file for ARDA_SHORT_MESSAGES feature
//
// Build: g++ -std=c++11 -I. -o test_short_errors test_short_errors.cpp && ./test_short_errors
//
// Verifies that messages are short when ARDA_SHORT_MESSAGES is defined.

#include <cstdio>
#include <cstring>
#include <cassert>
#include "Arduino.h"

// Define mock variables
uint32_t _mockMillis = 0;
MockSerial Serial;
MockStream mockStream;

// Enable short messages BEFORE including Arda
#define ARDA_SHORT_MESSAGES
#include "../Arda.h"
#include "../Arda.cpp"

void test_short_error_strings() {
    printf("Test: error strings are short PascalCase... ");

    // Verify each error string is the short version
    assert(strncmp(OS.errorString(ArdaError::Ok), "Ok", sizeof("Ok")) == 0);
    assert(strncmp(OS.errorString(ArdaError::NullName), "NullName", sizeof("NullName")) == 0);
    assert(strncmp(OS.errorString(ArdaError::EmptyName), "EmptyName", sizeof("EmptyName")) == 0);
    assert(strncmp(OS.errorString(ArdaError::NameTooLong), "NameTooLong", sizeof("NameTooLong")) == 0);
    assert(strncmp(OS.errorString(ArdaError::DuplicateName), "DuplicateName", sizeof("DuplicateName")) == 0);
    assert(strncmp(OS.errorString(ArdaError::MaxTasks), "MaxTasks", sizeof("MaxTasks")) == 0);
    assert(strncmp(OS.errorString(ArdaError::InvalidId), "InvalidId", sizeof("InvalidId")) == 0);
    assert(strncmp(OS.errorString(ArdaError::WrongState), "WrongState", sizeof("WrongState")) == 0);
    assert(strncmp(OS.errorString(ArdaError::TaskExecuting), "TaskExecuting", sizeof("TaskExecuting")) == 0);
#ifdef ARDA_YIELD
    assert(strncmp(OS.errorString(ArdaError::TaskYielded), "TaskYielded", sizeof("TaskYielded")) == 0);
#endif
    assert(strncmp(OS.errorString(ArdaError::AlreadyBegun), "AlreadyBegun", sizeof("AlreadyBegun")) == 0);
    assert(strncmp(OS.errorString(ArdaError::CallbackDepth), "CallbackDepth", sizeof("CallbackDepth")) == 0);
    assert(strncmp(OS.errorString(ArdaError::StateChanged), "StateChanged", sizeof("StateChanged")) == 0);
    assert(strncmp(OS.errorString(ArdaError::InCallback), "InCallback", sizeof("InCallback")) == 0);
    assert(strncmp(OS.errorString(ArdaError::NotSupported), "NotSupported", sizeof("NotSupported")) == 0);

    printf("PASSED\n");
}

void test_short_errors_are_shorter() {
    printf("Test: short errors are actually shorter than default... ");

    // Verify short strings are reasonably short (< 15 chars each)
    assert(strlen(OS.errorString(ArdaError::InvalidId)) < 15);
    assert(strlen(OS.errorString(ArdaError::WrongState)) < 15);
    assert(strlen(OS.errorString(ArdaError::DuplicateName)) < 15);

    printf("PASSED\n");
}

void test_short_help_output() {
    printf("Test: help output is short format... ");

    OS.setShellStream(mockStream);
    OS.begin();
    mockStream.clearOutput();

    // Execute help command
    OS.exec("h");

    const char* output = mockStream.getOutput();

    // Short format should be "p pause" not "p <id>        pause task"
    assert(strstr(output, "p pause") != nullptr);
    assert(strstr(output, "r resume") != nullptr);
    assert(strstr(output, "k kill") != nullptr);
    assert(strstr(output, "l list") != nullptr);

    // Should NOT contain the long format
    assert(strstr(output, "pause task") == nullptr);
    assert(strstr(output, "<id>") == nullptr);

    printf("PASSED\n");
}

int main() {
    printf("\n=== ARDA_SHORT_MESSAGES Tests ===\n\n");

    test_short_error_strings();
    test_short_errors_are_shorter();
    test_short_help_output();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
