// Test file for ARDA_NO_SHELL feature
//
// Build: g++ -std=c++11 -I. -o test_no_shell test_no_shell.cpp && ./test_no_shell
//
// Verifies that shell is completely removed when ARDA_NO_SHELL is defined.

#include <cstdio>
#include <cstring>
#include <cassert>
#include <new>
#include "Arduino.h"

// Define mock variables
uint32_t _mockMillis = 0;
MockSerial Serial;

// Disable shell BEFORE including Arda
#define ARDA_NO_SHELL
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

void test_no_shell_user_tasks_start_at_0() {
    printf("Test: user tasks start at ID 0 when shell disabled... ");
    resetTestCounters();

    // First task should get ID 0 (no shell to occupy it)
    int8_t id1 = OS.createTask("task1", task1_setup, task1_loop, 100);
    assert(id1 == 0);

    int8_t id2 = OS.createTask("task2", task1_setup, task1_loop, 100);
    assert(id2 == 1);

    // Total tasks: just user tasks
    assert(OS.getTaskCount() == 2);

    printf("PASSED\n");
}

void test_no_shell_api_not_available() {
    printf("Test: shell API not available when disabled... ");
    resetTestCounters();

    // This test verifies that the shell API is not available.
    // If ARDA_NO_SHELL is defined, the following should NOT compile:
    // - OS.setShellStream()
    // - OS.isShellRunning()
    // - OS.getShellTaskId()
    // - OS.exec()
    //
    // Since we can't have compile errors in a test file, we verify
    // by checking that ARDA_SHELL_ACTIVE is NOT defined.
#ifdef ARDA_SHELL_ACTIVE
    assert(false && "ARDA_SHELL_ACTIVE should not be defined when ARDA_NO_SHELL is set");
#endif

    printf("PASSED\n");
}

void test_no_shell_normal_operation() {
    printf("Test: scheduler works normally without shell... ");
    resetTestCounters();

    int8_t id = OS.createTask("task1", task1_setup, task1_loop, 0);
    OS.begin();

    assert(OS.getTaskState(id) == TaskState::Running);

    OS.run();
    assert(task1LoopCalled == 1);

    OS.run();
    assert(task1LoopCalled == 2);

    printf("PASSED\n");
}

void test_no_shell_task_operations() {
    printf("Test: task operations work without shell... ");
    resetTestCounters();

    int8_t id = OS.createTask("task1", task1_setup, task1_loop, 100);
    OS.begin();

    // All normal task operations should work
    assert(OS.pauseTask(id));
    assert(OS.getTaskState(id) == TaskState::Paused);

    assert(OS.resumeTask(id));
    assert(OS.getTaskState(id) == TaskState::Running);

    assert(OS.stopTask(id) != StopResult::Failed);
    assert(OS.getTaskState(id) == TaskState::Stopped);

    assert(OS.deleteTask(id));
    assert(!OS.isValidTask(id));

    printf("PASSED\n");
}

void test_no_shell_max_tasks() {
    printf("Test: can use all task slots without shell... ");
    resetTestCounters();

    // Create tasks to fill all slots
    static char names[ARDA_MAX_TASKS][16];
    for (int i = 0; i < ARDA_MAX_TASKS; i++) {
        snprintf(names[i], sizeof(names[i]), "task%d", i);
        int8_t id = OS.createTask(names[i], nullptr, nullptr, 0);
        assert(id == i);
    }

    // All slots should be used
    assert(OS.getTaskCount() == ARDA_MAX_TASKS);

    // Should fail when full
    int8_t overflow = OS.createTask("overflow", nullptr, nullptr, 0);
    assert(overflow == -1);

    printf("PASSED\n");
}

int main() {
    printf("\n=== ARDA_NO_SHELL Tests ===\n\n");

    test_no_shell_user_tasks_start_at_0();
    test_no_shell_api_not_available();
    test_no_shell_normal_operation();
    test_no_shell_task_operations();
    test_no_shell_max_tasks();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
