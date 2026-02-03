// Test file for ARDA_SHELL_MINIMAL feature
//
// Build: g++ -std=c++11 -I. -o test_shell_minimal test_shell_minimal.cpp && ./test_shell_minimal
//
// Verifies that only core commands are available when ARDA_SHELL_MINIMAL is defined.

#include <cstdio>
#include <cstring>
#include <cassert>
#include <new>
#include "Arduino.h"

// Define mock variables
uint32_t _mockMillis = 0;
MockSerial Serial;

// Enable minimal shell BEFORE including Arda
#define ARDA_SHELL_MINIMAL
#include "../Arda.h"
#include "../Arda.cpp"

// Test task callbacks
static int task1LoopCalled = 0;
void task1_setup() {}
void task1_loop() { task1LoopCalled++; }

// Reset global OS instance to clean state between tests.
void resetGlobalOS() {
    OS.~Arda();
    new (&OS) Arda();
}

void resetTestCounters() {
    task1LoopCalled = 0;
    setMockMillis(0);
    resetGlobalOS();
}

void test_minimal_core_commands_work() {
    printf("Test: core commands work in minimal mode... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    int8_t taskId = OS.createTask("task1", task1_setup, task1_loop, 100);
    OS.begin();

    // Pause (p)
    mockStream.setInput("p 1\n");
    mockStream.clearOutput();
    OS.run();
    assert(OS.getTaskState(taskId) == TaskState::Paused);
    assert(strstr(mockStream.getOutput(), "OK") != nullptr);

    // Resume (r)
    mockStream.setInput("r 1\n");
    mockStream.clearOutput();
    OS.run();
    assert(OS.getTaskState(taskId) == TaskState::Running);
    assert(strstr(mockStream.getOutput(), "OK") != nullptr);

    // Stop (s)
    mockStream.setInput("s 1\n");
    mockStream.clearOutput();
    OS.run();
    assert(OS.getTaskState(taskId) == TaskState::Stopped);
    assert(strstr(mockStream.getOutput(), "OK") != nullptr);

    // Begin (b)
    mockStream.setInput("b 1\n");
    mockStream.clearOutput();
    OS.run();
    assert(OS.getTaskState(taskId) == TaskState::Running);
    assert(strstr(mockStream.getOutput(), "OK") != nullptr);

    // Stop again for delete
    OS.stopTask(taskId);

    // Delete (d)
    mockStream.setInput("d 1\n");
    mockStream.clearOutput();
    OS.run();
    assert(!OS.isValidTask(taskId));
    assert(strstr(mockStream.getOutput(), "OK") != nullptr);

    printf("PASSED\n");
}

void test_minimal_list_command_works() {
    printf("Test: list command works in minimal mode... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    OS.createTask("task1", task1_setup, task1_loop, 100);
    OS.begin();

    // List (l)
    mockStream.setInput("l\n");
    mockStream.clearOutput();
    OS.run();

    const char* output = mockStream.getOutput();
    assert(strstr(output, "0 R sh") != nullptr);
    assert(strstr(output, "1 R task1") != nullptr);

    printf("PASSED\n");
}

void test_minimal_help_command_works() {
    printf("Test: help command works in minimal mode... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    OS.begin();

    // Help (h)
    mockStream.setInput("h\n");
    mockStream.clearOutput();
    OS.run();

    const char* output = mockStream.getOutput();
    // In minimal mode, only core commands should be listed
    assert(strstr(output, "pause") != nullptr);
    assert(strstr(output, "list") != nullptr);
    assert(strstr(output, "kill") != nullptr);  // New command
    // Extended commands should NOT be in minimal mode help
    assert(strstr(output, "info") == nullptr);
    assert(strstr(output, "uptime") == nullptr);

    printf("PASSED\n");
}

void test_minimal_debug_commands_return_unknown() {
    printf("Test: debug commands return '?' in minimal mode... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    OS.begin();

    // Info (i) - should be unknown
    mockStream.setInput("i 0\n");
    mockStream.clearOutput();
    OS.run();
    assert(strstr(mockStream.getOutput(), "?") != nullptr);

    // Uptime (u) - should be unknown
    mockStream.setInput("u\n");
    mockStream.clearOutput();
    OS.run();
    assert(strstr(mockStream.getOutput(), "?") != nullptr);

    // Memory (m) - should be unknown
    mockStream.setInput("m\n");
    mockStream.clearOutput();
    OS.run();
    assert(strstr(mockStream.getOutput(), "?") != nullptr);

    // Error (e) - should be unknown
    mockStream.setInput("e\n");
    mockStream.clearOutput();
    OS.run();
    assert(strstr(mockStream.getOutput(), "?") != nullptr);

    // Version (v) - should be unknown
    mockStream.setInput("v\n");
    mockStream.clearOutput();
    OS.run();
    assert(strstr(mockStream.getOutput(), "?") != nullptr);

    printf("PASSED\n");
}

void test_minimal_exec_works() {
    printf("Test: exec() works in minimal mode... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    int8_t taskId = OS.createTask("task1", task1_setup, task1_loop, 100);
    OS.begin();

    // exec() should work with core commands
    mockStream.clearOutput();
    OS.exec("p 1");
    assert(OS.getTaskState(taskId) == TaskState::Paused);

    printf("PASSED\n");
}

int main() {
    printf("\n=== ARDA_SHELL_MINIMAL Tests ===\n\n");

    test_minimal_core_commands_work();
    test_minimal_list_command_works();
    test_minimal_help_command_works();
    test_minimal_debug_commands_return_unknown();
    test_minimal_exec_works();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
