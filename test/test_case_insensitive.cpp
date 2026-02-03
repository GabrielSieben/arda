// Test for ARDA_CASE_INSENSITIVE_NAMES feature
// Build: g++ -std=c++11 -I. -o test_case_insensitive test_case_insensitive.cpp && ./test_case_insensitive

#include <cstdio>
#include <cstring>
#include <cassert>

// Mock Arduino environment
#include "Arduino.h"
uint32_t _mockMillis = 0;
MockSerial Serial;

// Enable case-insensitive names and disable shell BEFORE including Arda
#define ARDA_CASE_INSENSITIVE_NAMES
#define ARDA_NO_SHELL
#include "../Arda.h"
#include "../Arda.cpp"

void dummy_setup() {}
void dummy_loop() {}

void test_case_insensitive_find() {
    printf("Test: case-insensitive findTaskByName... ");

    Arda os;
    int8_t id = os.createTask("MyTask", dummy_setup, dummy_loop, 0);

    // Should find with exact case
    assert(os.findTaskByName("MyTask") == id);

    // Should find with different cases
    assert(os.findTaskByName("mytask") == id);
    assert(os.findTaskByName("MYTASK") == id);
    assert(os.findTaskByName("mYtAsK") == id);

    // Should not find non-matching names
    assert(os.findTaskByName("MyTask2") == -1);
    assert(os.findTaskByName("Task") == -1);

    printf("PASSED\n");
}

void test_case_insensitive_duplicate_rejection() {
    printf("Test: case-insensitive duplicate rejection... ");

    Arda os;
    int8_t id1 = os.createTask("TaskOne", dummy_setup, dummy_loop, 0);
    assert(id1 >= 0);

    // Should reject duplicates with different cases
    int8_t id2 = os.createTask("taskone", dummy_setup, dummy_loop, 0);
    assert(id2 == -1);
    assert(os.getError() == ArdaError::DuplicateName);

    int8_t id3 = os.createTask("TASKONE", dummy_setup, dummy_loop, 0);
    assert(id3 == -1);
    assert(os.getError() == ArdaError::DuplicateName);

    // Should accept different name and clear error
    int8_t id4 = os.createTask("TaskTwo", dummy_setup, dummy_loop, 0);
    assert(id4 >= 0);
    assert(os.getError() == ArdaError::Ok);

    printf("PASSED\n");
}

void test_case_insensitive_rename() {
    printf("Test: case-insensitive rename... ");

    Arda os;
    int8_t id1 = os.createTask("First", dummy_setup, dummy_loop, 0);
    int8_t id2 = os.createTask("Second", dummy_setup, dummy_loop, 0);

    // Should reject rename to case-variant of existing name
    assert(os.renameTask(id1, "SECOND") == false);
    assert(os.getError() == ArdaError::DuplicateName);

    // Should allow rename to case-variant of own name and clear error
    assert(os.renameTask(id1, "FIRST") == true);
    assert(os.getError() == ArdaError::Ok);
    assert(strncmp(os.getTaskName(id1), "FIRST", ARDA_MAX_NAME_LEN) == 0);

    (void)id2;
    printf("PASSED\n");
}

int main() {
    printf("\n=== ARDA_CASE_INSENSITIVE_NAMES Tests ===\n\n");

    test_case_insensitive_find();
    test_case_insensitive_duplicate_rejection();
    test_case_insensitive_rename();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
