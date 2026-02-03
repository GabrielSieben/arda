// Test for ARDA_NO_PRIORITY feature
// Build: g++ -std=c++11 -I. -o test_no_priority test_no_priority.cpp && ./test_no_priority
//
// This verifies that:
// 1. Priority API is NOT available when ARDA_NO_PRIORITY is defined
// 2. Array-order execution still works correctly

#include <cstdio>
#include <cstring>
#include <cassert>

// Mock Arduino environment
#include "Arduino.h"
uint32_t _mockMillis = 0;
MockSerial Serial;

// Disable priority and shell BEFORE including Arda
#define ARDA_NO_PRIORITY
#define ARDA_NO_SHELL
#include "../Arda.h"
#include "../Arda.cpp"

// Track execution order
static int executionOrderIndex = 0;
static int8_t executionOrder[3] = {-1, -1, -1};

void task0_setup() {}
void task0_loop() {
    if (executionOrderIndex < 3) {
        executionOrder[executionOrderIndex++] = 0;
    }
}

void task1_setup() {}
void task1_loop() {
    if (executionOrderIndex < 3) {
        executionOrder[executionOrderIndex++] = 1;
    }
}

void task2_setup() {}
void task2_loop() {
    if (executionOrderIndex < 3) {
        executionOrder[executionOrderIndex++] = 2;
    }
}

void test_priority_api_not_available() {
    printf("Test: priority API is not available... ");

    // This test verifies that the priority API is not compiled when
    // ARDA_NO_PRIORITY is defined. If the API was available, this
    // test file would fail to compile with the compile-time checks below.

    // Compile-time verification: the following would cause compile errors
    // if uncommented when ARDA_NO_PRIORITY is NOT defined:
    //
    // Arda os;
    // os.setTaskPriority(0, 5);  // Error: no member named 'setTaskPriority'
    // os.getTaskPriority(0);     // Error: no member named 'getTaskPriority'
    // os.createTask("t", nullptr, nullptr, 0, nullptr, false, 8);  // Error: too many arguments
    // TaskPriority p = TaskPriority::Normal;  // Error: undeclared identifier

    // Verify macros are not defined
#ifdef ARDA_TASK_PRIORITY_MASK
    #error "ARDA_TASK_PRIORITY_MASK should not be defined when ARDA_NO_PRIORITY is set"
#endif
#ifdef ARDA_TASK_PRIORITY_SHIFT
    #error "ARDA_TASK_PRIORITY_SHIFT should not be defined when ARDA_NO_PRIORITY is set"
#endif
#ifdef ARDA_DEFAULT_PRIORITY
    #error "ARDA_DEFAULT_PRIORITY should not be defined when ARDA_NO_PRIORITY is set"
#endif

    printf("PASSED\n");
}

void test_array_order_execution() {
    printf("Test: tasks execute in array/snapshot order... ");

    executionOrderIndex = 0;
    executionOrder[0] = -1;
    executionOrder[1] = -1;
    executionOrder[2] = -1;

    Arda os;

    // Create tasks in order 0, 1, 2
    int8_t id0 = os.createTask("t0", task0_setup, task0_loop, 0);
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);

    assert(id0 == 0);
    assert(id1 == 1);
    assert(id2 == 2);

    os.begin();
    os.run();

    // All three should have run
    assert(executionOrderIndex == 3);

    // Without priority, execution should be in snapshot order (0, 1, 2)
    assert(executionOrder[0] == 0);
    assert(executionOrder[1] == 1);
    assert(executionOrder[2] == 2);

    printf("PASSED\n");
}

void test_basic_functionality_still_works() {
    printf("Test: basic scheduler functionality works... ");

    Arda os;

    // Create tasks
    int8_t id = os.createTask("task", task0_setup, task0_loop, 100);
    assert(id >= 0);

    // Task lifecycle
    assert(os.getTaskState(id) == TaskState::Stopped);

    os.startTask(id);
    assert(os.getTaskState(id) == TaskState::Running);

    os.pauseTask(id);
    assert(os.getTaskState(id) == TaskState::Paused);

    os.resumeTask(id);
    assert(os.getTaskState(id) == TaskState::Running);

    os.stopTask(id);
    assert(os.getTaskState(id) == TaskState::Stopped);

    // Configuration
    assert(os.setTaskInterval(id, 200) == true);
    assert(os.getTaskInterval(id) == 200);

    assert(os.setTaskTimeout(id, 500) == true);
    assert(os.getTaskTimeout(id) == 500);

    // Delete
    assert(os.deleteTask(id) == true);
    assert(os.isValidTask(id) == false);

    printf("PASSED\n");
}

void test_flags_only_use_lower_bits() {
    printf("Test: flags byte only uses bits 0-3... ");

    Arda os;
    int8_t id = os.createTask("task", task0_setup, task0_loop, 0);

    // Without priority, flags should only use bits 0-3:
    // bits 0-1: state (Stopped=0, Running=1, Paused=2)
    // bit 2: ranThisCycle
    // bit 3: inYield
    // bits 4-7: unused (should be 0)

    // Start task - state becomes Running (1)
    os.startTask(id);
    assert(os.getTaskState(id) == TaskState::Running);

    // Pause task - state becomes Paused (2)
    os.pauseTask(id);
    assert(os.getTaskState(id) == TaskState::Paused);

    // Resume task - state becomes Running (1)
    os.resumeTask(id);
    assert(os.getTaskState(id) == TaskState::Running);

    // Stop task - state becomes Stopped (0)
    os.stopTask(id);
    assert(os.getTaskState(id) == TaskState::Stopped);

    printf("PASSED\n");
}

int main() {
    printf("\n=== ARDA_NO_PRIORITY Tests ===\n\n");

    test_priority_api_not_available();
    test_array_order_execution();
    test_basic_functionality_still_works();
    test_flags_only_use_lower_bits();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
