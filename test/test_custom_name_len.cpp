// Test for custom ARDA_MAX_NAME_LEN
// Build: g++ -std=c++11 -I. -o test_custom_name_len test_custom_name_len.cpp && ./test_custom_name_len

#include <cstdio>
#include <cstring>
#include <cassert>

// Mock Arduino environment
#include "Arduino.h"
uint32_t _mockMillis = 0;
MockSerial Serial;

// Set custom name length and disable shell BEFORE including Arda
#define ARDA_MAX_NAME_LEN 8  // Only 7 usable chars + null terminator
#define ARDA_NO_SHELL
#include "../Arda.h"
#include "../Arda.cpp"

void dummy_setup() {}
void dummy_loop() {}

void test_custom_name_length_limit() {
    printf("Test: custom ARDA_MAX_NAME_LEN enforced... ");

    Arda os;

    // Should accept names up to 7 chars (ARDA_MAX_NAME_LEN - 1)
    int8_t id1 = os.createTask("1234567", dummy_setup, dummy_loop, 0);
    assert(id1 >= 0);
    assert(strncmp(os.getTaskName(id1), "1234567", ARDA_MAX_NAME_LEN) == 0);

    // Should reject names of 8 chars or more
    int8_t id2 = os.createTask("12345678", dummy_setup, dummy_loop, 0);
    assert(id2 == -1);
    assert(os.getError() == ArdaError::NameTooLong);

    int8_t id3 = os.createTask("123456789", dummy_setup, dummy_loop, 0);
    assert(id3 == -1);
    assert(os.getError() == ArdaError::NameTooLong);

    printf("PASSED\n");
}

void test_short_names_work() {
    printf("Test: short names work with custom limit... ");

    Arda os;

    int8_t id1 = os.createTask("a", dummy_setup, dummy_loop, 0);
    int8_t id2 = os.createTask("ab", dummy_setup, dummy_loop, 0);
    int8_t id3 = os.createTask("abc", dummy_setup, dummy_loop, 0);

    assert(id1 >= 0);
    assert(id2 >= 0);
    assert(id3 >= 0);

    assert(strncmp(os.getTaskName(id1), "a", ARDA_MAX_NAME_LEN) == 0);
    assert(strncmp(os.getTaskName(id2), "ab", ARDA_MAX_NAME_LEN) == 0);
    assert(strncmp(os.getTaskName(id3), "abc", ARDA_MAX_NAME_LEN) == 0);

    printf("PASSED\n");
}

void test_rename_respects_limit() {
    printf("Test: rename respects custom limit... ");

    Arda os;

    int8_t id = os.createTask("short", dummy_setup, dummy_loop, 0);
    assert(id >= 0);

    // Should allow rename to max length
    assert(os.renameTask(id, "7chars!") == true);

    // Should reject rename to too-long name
    assert(os.renameTask(id, "8chars!!") == false);
    assert(os.getError() == ArdaError::NameTooLong);

    // Name should be unchanged after failed rename
    assert(strncmp(os.getTaskName(id), "7chars!", ARDA_MAX_NAME_LEN) == 0);

    printf("PASSED\n");
}

int main() {
    printf("\n=== Custom ARDA_MAX_NAME_LEN Tests ===\n\n");

    test_custom_name_length_limit();
    test_short_names_work();
    test_rename_respects_limit();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
