// Test file for ARDA_SHELL_MANUAL_START feature
//
// Build: g++ -std=c++11 -I. -o test_shell_manual_start test_shell_manual_start.cpp && ./test_shell_manual_start
//
// Tests startShell() and stopShell() functions which are only available
// when ARDA_SHELL_MANUAL_START is defined.

#include <cstdio>
#include <cstring>
#include <cassert>
#include <new>
#include "Arduino.h"

// Define mock variables
uint32_t _mockMillis = 0;
MockSerial Serial;

// Enable manual shell start/stop
#define ARDA_SHELL_MANUAL_START

// Include Arda (unity build - header and implementation together)
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

void test_start_shell_basic() {
    printf("Test: startShell() starts stopped shell... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    // With ARDA_SHELL_MANUAL_START, begin() does not auto-start the shell
    OS.begin();

    // Shell should exist but be stopped
    assert(OS.isValidTask(ARDA_SHELL_TASK_ID));
    assert(OS.getTaskState(ARDA_SHELL_TASK_ID) == TaskState::Stopped);
    assert(!OS.isShellRunning());

    // startShell() should start it
    bool result = OS.startShell();
    assert(result);
    assert(OS.getTaskState(ARDA_SHELL_TASK_ID) == TaskState::Running);
    assert(OS.isShellRunning());

    printf("PASSED\n");
}

void test_stop_shell_basic() {
    printf("Test: stopShell() stops running shell... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    // With ARDA_SHELL_MANUAL_START, begin() does NOT auto-start the shell
    OS.begin();

    // Shell should exist but be stopped (manual start mode)
    assert(OS.isValidTask(ARDA_SHELL_TASK_ID));
    assert(OS.getTaskState(ARDA_SHELL_TASK_ID) == TaskState::Stopped);

    // Start it manually first
    OS.startShell();
    assert(OS.isShellRunning());
    assert(OS.getTaskState(ARDA_SHELL_TASK_ID) == TaskState::Running);

    // stopShell() should stop it
    bool result = OS.stopShell();
    assert(result);
    assert(OS.getTaskState(ARDA_SHELL_TASK_ID) == TaskState::Stopped);
    assert(!OS.isShellRunning());

    printf("PASSED\n");
}

void test_start_shell_already_running() {
    printf("Test: startShell() on running shell returns false... ");
    resetTestCounters();

    OS.begin();
    // Manual start: shell is stopped after begin()
    OS.startShell();
    assert(OS.isShellRunning());

    // startShell() on already running shell should fail
    bool result = OS.startShell();
    assert(!result);
    // Shell should still be running
    assert(OS.isShellRunning());

    printf("PASSED\n");
}

void test_stop_shell_already_stopped() {
    printf("Test: stopShell() on stopped shell returns false (WrongState)... ");
    resetTestCounters();

    OS.begin();  // With ARDA_SHELL_MANUAL_START, shell stays stopped
    assert(!OS.isShellRunning());
    assert(OS.getTaskState(ARDA_SHELL_TASK_ID) == TaskState::Stopped);

    // stopShell() on already stopped shell returns false (stopTask returns Failed + WrongState)
    bool result = OS.stopShell();
    assert(!result);
    assert(OS.getError() == ArdaError::WrongState);

    printf("PASSED\n");
}

void test_start_shell_after_deleted() {
    printf("Test: startShell() after shell deleted returns false with InvalidId... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    OS.begin();
    // Manual start: shell is stopped after begin(), start it
    OS.startShell();
    assert(OS.isShellRunning());

    // Stop and delete the shell
    OS.stopShell();
    bool deleted = OS.deleteTask(ARDA_SHELL_TASK_ID);
    assert(deleted);
    assert(!OS.isValidTask(ARDA_SHELL_TASK_ID));

    // Clear any previous error
    OS.clearError();

    // startShell() should return false with InvalidId error
    bool result = OS.startShell();
    assert(!result);
    assert(OS.getError() == ArdaError::InvalidId);

    printf("PASSED\n");
}

void test_stop_shell_after_deleted() {
    printf("Test: stopShell() after shell deleted returns false with InvalidId... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    OS.begin();
    // Shell is stopped in manual mode; delete it directly
    bool deleted = OS.deleteTask(ARDA_SHELL_TASK_ID);
    assert(deleted);
    assert(!OS.isValidTask(ARDA_SHELL_TASK_ID));

    // Clear any previous error
    OS.clearError();

    // stopShell() should return false with InvalidId error
    bool result = OS.stopShell();
    assert(!result);
    assert(OS.getError() == ArdaError::InvalidId);

    printf("PASSED\n");
}

void test_start_shell_after_slot_reused() {
    printf("Test: startShell() after slot 0 reused by user task... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    OS.begin();

    // Shell is stopped in manual mode; delete it directly
    OS.deleteTask(ARDA_SHELL_TASK_ID);

    // Create a user task - it should get slot 0
    int8_t newTask = OS.createTask("user", task1_setup, task1_loop, 100);
    assert(newTask == 0);

    // Start the user task
    OS.startTask(newTask);
    assert(OS.getTaskState(newTask) == TaskState::Running);

    // Clear any previous error
    OS.clearError();

    // startShell() should NOT affect the user task
    bool result = OS.startShell();
    assert(!result);
    assert(OS.getError() == ArdaError::InvalidId);

    // User task should still be running (not affected)
    assert(OS.getTaskState(0) == TaskState::Running);
    assert(strncmp(OS.getTaskName(0), "user", ARDA_MAX_NAME_LEN) == 0);

    printf("PASSED\n");
}

void test_stop_shell_after_slot_reused() {
    printf("Test: stopShell() after slot 0 reused by user task... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    OS.begin();

    // Shell is stopped in manual mode; delete it directly
    OS.deleteTask(ARDA_SHELL_TASK_ID);

    // Create a user task - it should get slot 0
    int8_t newTask = OS.createTask("user", task1_setup, task1_loop, 100);
    assert(newTask == 0);

    // Start the user task
    OS.startTask(newTask);
    assert(OS.getTaskState(newTask) == TaskState::Running);

    // Clear any previous error
    OS.clearError();

    // stopShell() should NOT affect the user task
    bool result = OS.stopShell();
    assert(!result);
    assert(OS.getError() == ArdaError::InvalidId);

    // User task should still be running (not affected)
    assert(OS.getTaskState(0) == TaskState::Running);

    printf("PASSED\n");
}

void test_start_stop_shell_cycle() {
    printf("Test: start/stop shell multiple times... ");
    resetTestCounters();

    MockStream mockStream;
    OS.setShellStream(mockStream);

    OS.begin();  // With ARDA_SHELL_MANUAL_START, shell stays stopped
    assert(!OS.isShellRunning());

    // Cycle start/stop multiple times
    for (int i = 0; i < 3; i++) {
        assert(OS.startShell());
        assert(OS.isShellRunning());

        assert(OS.stopShell());
        assert(!OS.isShellRunning());
    }

    printf("PASSED\n");
}

int main() {
    printf("\n=== Arda Shell Manual Start Tests ===\n\n");

    test_start_shell_basic();
    test_stop_shell_basic();
    test_start_shell_already_running();
    test_stop_shell_already_stopped();
    test_start_shell_after_deleted();
    test_stop_shell_after_deleted();
    test_start_shell_after_slot_reused();
    test_stop_shell_after_slot_reused();
    test_start_stop_shell_cycle();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
