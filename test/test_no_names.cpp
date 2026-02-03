// Test for ARDA_NO_NAMES feature
// Build: g++ -std=c++11 -I. -o test_no_names test_no_names.cpp && ./test_no_names
//
// This verifies that:
// 1. Name API is NOT available when ARDA_NO_NAMES is defined
// 2. Task creation and lifecycle still works correctly
// 3. getTaskName returns nullptr, findTaskByName returns -1, renameTask returns false
// 4. REGISTER_TASK macros still work

#include <cstdio>
#include <cstring>
#include <cassert>

// Mock Arduino environment
#include "Arduino.h"
uint32_t _mockMillis = 0;
MockSerial Serial;

// Disable names and shell BEFORE including Arda
#define ARDA_NO_NAMES
#define ARDA_NO_SHELL
#include "../Arda.h"
#include "../Arda.cpp"

// Track execution
static int task0RunCount = 0;
static int task1RunCount = 0;
static int task2RunCount = 0;

void task0_setup() {}
void task0_loop() {
    task0RunCount++;
}

void task1_setup() {}
void task1_loop() {
    task1RunCount++;
}

void task2_setup() {}
void task2_loop() {
    task2RunCount++;
}
void task2_teardown() {}

void test_name_api_not_available() {
    printf("Test: name API gracefully degrades... ");

    Arda os;

    // Create a task using the nameless overload
    int8_t id = os.createTask(task0_setup, task0_loop, 100);
    assert(id >= 0);

    // getTaskName should return nullptr
    assert(os.getTaskName(id) == nullptr);

    // findTaskByName should return -1
    assert(os.findTaskByName("anything") == -1);

    // renameTask should return false with NotSupported error
    assert(os.renameTask(id, "newname") == false);
    assert(os.getError() == ArdaError::NotSupported);

    printf("PASSED\n");
}

void test_nameless_create_task() {
    printf("Test: nameless createTask works... ");

    Arda os;

    // Create tasks without names
    int8_t id0 = os.createTask(task0_setup, task0_loop, 0);
    int8_t id1 = os.createTask(task1_setup, task1_loop, 100);
    int8_t id2 = os.createTask(task2_setup, task2_loop, 200, task2_teardown);

    assert(id0 >= 0);
    assert(id1 >= 0);
    assert(id2 >= 0);
    assert(id0 != id1 && id1 != id2 && id0 != id2);

    assert(os.getTaskCount() == 3);

    printf("PASSED\n");
}

void test_backward_compat_createTask() {
    printf("Test: backward compatible createTask (with name param) works... ");

    Arda os;

    // Use the backward-compatible overload that accepts a name but ignores it
    int8_t id = os.createTask("ignored_name", task0_setup, task0_loop, 100);
    assert(id >= 0);

    // Name is ignored, so getTaskName returns nullptr
    assert(os.getTaskName(id) == nullptr);

    // No duplicate name checking when names are disabled
    // We can create another task with the "same" name (both are ignored)
    int8_t id2 = os.createTask("ignored_name", task1_setup, task1_loop, 100);
    assert(id2 >= 0);
    assert(id != id2);

    printf("PASSED\n");
}

void test_task_lifecycle() {
    printf("Test: task lifecycle works without names... ");

    task0RunCount = 0;

    Arda os;

    int8_t id = os.createTask(task0_setup, task0_loop, 0);
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

    // Delete
    assert(os.deleteTask(id) == true);
    assert(os.isValidTask(id) == false);

    printf("PASSED\n");
}

void test_scheduler_run() {
    printf("Test: scheduler runs tasks without names... ");

    task0RunCount = 0;
    task1RunCount = 0;
    _mockMillis = 0;

    Arda os;

    int8_t id0 = os.createTask(task0_setup, task0_loop, 0);
    int8_t id1 = os.createTask(task1_setup, task1_loop, 0);
    assert(id0 >= 0);
    assert(id1 >= 0);

    os.begin();
    os.run();

    // Both tasks should have run
    assert(task0RunCount == 1);
    assert(task1RunCount == 1);

    os.run();

    assert(task0RunCount == 2);
    assert(task1RunCount == 2);

    printf("PASSED\n");
}

void test_register_task_macros() {
    printf("Test: REGISTER_TASK macros work without names... ");

    task2RunCount = 0;

    // Uses the global OS instance
    int8_t id = REGISTER_TASK(task2, 0);
    assert(id >= 0);

    // Should be able to use REGISTER_TASK_WITH_TEARDOWN too
    // (Not testing here as we'd need a different task name pattern)

    OS.begin();
    OS.run();

    assert(task2RunCount == 1);

    OS.reset();

    printf("PASSED\n");
}

void test_delete_and_reuse_slots() {
    printf("Test: slot reuse works with deleted flag... ");

    Arda os;

    // Create tasks
    int8_t id0 = os.createTask(task0_setup, task0_loop, 0);
    int8_t id1 = os.createTask(task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask(task2_setup, task2_loop, 0);

    assert(id0 == 0);
    assert(id1 == 1);
    assert(id2 == 2);
    assert(os.getTaskCount() == 3);

    // Stop and delete the middle task
    os.startTask(id1);
    os.stopTask(id1);
    assert(os.deleteTask(id1) == true);
    assert(os.getTaskCount() == 2);
    assert(os.isValidTask(id1) == false);

    // Create a new task - should reuse the deleted slot
    int8_t id3 = os.createTask(task1_setup, task1_loop, 0);
    assert(id3 == 1);  // Should reuse slot 1
    assert(os.getTaskCount() == 3);
    assert(os.isValidTask(id3) == true);

    printf("PASSED\n");
}

void test_error_string_includes_not_supported() {
    printf("Test: errorString includes NotSupported... ");

    const char* str = Arda::errorString(ArdaError::NotSupported);
    assert(strncmp(str, "Not supported", 32) == 0);

    printf("PASSED\n");
}

void test_compile_time_checks() {
    printf("Test: compile-time checks pass... ");

    // Verify ARDA_TASK_DELETED_STATE is defined when ARDA_NO_NAMES is set
#ifndef ARDA_TASK_DELETED_STATE
    #error "ARDA_TASK_DELETED_STATE should be defined when ARDA_NO_NAMES is set"
#endif

    // Verify name field is not in Task struct (would cause compile error if present)
    // This is implicit - if the code compiles without name references, it works

    printf("PASSED\n");
}

#ifndef ARDA_NO_PRIORITY
void test_priority_does_not_conflict_with_deleted() {
    printf("Test: priority values don't conflict with deleted marker... ");

    Arda os;

    // Test all 5 priority levels to ensure none are mistaken for deleted
    TaskPriority priorities[] = {
        TaskPriority::Lowest,
        TaskPriority::Low,
        TaskPriority::Normal,
        TaskPriority::High,
        TaskPriority::Highest
    };

    for (int i = 0; i < 5; i++) {
        int8_t id = os.createTask(task0_setup, task0_loop, 0);
        assert(id >= 0);

        os.setTaskPriority(id, priorities[i]);

        // Task should still be valid (not mistakenly marked as deleted)
        assert(os.isValidTask(id) == true);
        assert(os.getTaskPriority(id) == priorities[i]);

        // Clean up for next iteration
        os.deleteTask(id);
    }

    printf("PASSED\n");
}
#endif

int main() {
    printf("\n=== ARDA_NO_NAMES Tests ===\n\n");

    test_name_api_not_available();
    test_nameless_create_task();
    test_backward_compat_createTask();
    test_task_lifecycle();
    test_scheduler_run();
    test_register_task_macros();
    test_delete_and_reuse_slots();
    test_error_string_includes_not_supported();
    test_compile_time_checks();
#ifndef ARDA_NO_PRIORITY
    test_priority_does_not_conflict_with_deleted();
#endif

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
