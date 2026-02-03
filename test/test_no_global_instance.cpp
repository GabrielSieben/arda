// Test for ARDA_NO_GLOBAL_INSTANCE feature
// Build: g++ -std=c++11 -I. -o test_no_global_instance test_no_global_instance.cpp && ./test_no_global_instance

#include <cstdio>
#include <cstring>
#include <cassert>

// Mock Arduino environment
#include "Arduino.h"
uint32_t _mockMillis = 0;
MockSerial Serial;

// Disable global OS instance BEFORE including Arda
// Note: Shell requires global OS instance, so it's automatically disabled
#define ARDA_NO_GLOBAL_INSTANCE
#include "../Arda.h"
#include "../Arda.cpp"

void dummy_setup() {}
void dummy_loop() {}

void test_no_global_instance() {
    printf("Test: no global OS instance exists... ");

    // This test verifies that the code compiles without the global OS instance.
    // If ARDA_NO_GLOBAL_INSTANCE is working, 'OS' would be undefined and this
    // file wouldn't compile if we tried to use it.

    // Create our own scheduler instances
    Arda scheduler1;
    Arda scheduler2;

    int8_t id1 = scheduler1.createTask("task1", dummy_setup, dummy_loop, 100);
    int8_t id2 = scheduler2.createTask("task2", dummy_setup, dummy_loop, 200);

    assert(id1 >= 0);
    assert(id2 >= 0);

    // Each scheduler is independent
    assert(scheduler1.getTaskCount() == 1);
    assert(scheduler2.getTaskCount() == 1);

    assert(strncmp(scheduler1.getTaskName(id1), "task1", ARDA_MAX_NAME_LEN) == 0);
    assert(strncmp(scheduler2.getTaskName(id2), "task2", ARDA_MAX_NAME_LEN) == 0);

    // Verify tasks don't cross schedulers
    assert(scheduler1.findTaskByName("task2") == -1);
    assert(scheduler2.findTaskByName("task1") == -1);

    printf("PASSED\n");
}

void test_register_task_on_macros() {
    printf("Test: REGISTER_TASK_ON macros work... ");

    Arda myScheduler;

    // Use the _ON variants that work without global instance
    REGISTER_TASK_ID_ON(taskId, myScheduler, dummy, 100);

    assert(taskId >= 0);
    assert(strncmp(myScheduler.getTaskName(taskId), "dummy", ARDA_MAX_NAME_LEN) == 0);

    printf("PASSED\n");
}

int main() {
    printf("\n=== ARDA_NO_GLOBAL_INSTANCE Tests ===\n\n");

    test_no_global_instance();
    test_register_task_on_macros();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
