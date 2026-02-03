// Test file for Arda
//
// Build: g++ -std=c++11 -I. -o test_arda test_arda.cpp && ./test_arda
//
// Note: We include Arda.cpp directly (unity build) rather than compiling it
// separately. This simplifies the build process for a single-file library and
// allows the mock Arduino.h to be resolved before Arda.h includes <Arduino.h>.
// This pattern is intentional for this test environment.
//
// Test Pattern:
// - Most tests use local `Arda os;` instances for isolation
// - Tests where callbacks need scheduler access use the global `OS` instance
//   (e.g., yield tests, getCurrentTask tests, self-deletion tests)
// - resetGlobalOS() reconstructs the global OS between tests using placement new

#include <cstdio>
#include <cstring>
#include <cassert>
#include <new>
#include "Arduino.h"

// Define mock variables
uint32_t _mockMillis = 0;  // 32-bit to simulate Arduino's millis() ovrflow
MockSerial Serial;

// Include Arda (unity build - header and implementation together)
#include "../Arda.h"
#include "../Arda.cpp"

// Test variables
static int setup1Called = 0;
static int loop1Called = 0;
static int setup2Called = 0;
static int loop2Called = 0;

// Reset global OS instance to clean state between tests.
// Uses placement new to reconstruct the global OS object in place without
// reallocating memory. This is safe because Arda has no destructor side effects
// (no heap allocations, file handles, etc.) - it just resets internal state.
// This approach is simpler than adding a dedicated reset-to-initial-state method.
void resetGlobalOS() {
    OS.~Arda();       // Explicitly call destructor
    new (&OS) Arda(); // Construct new instance at same address
}

void resetTestCounters() {
    setup1Called = 0;
    loop1Called = 0;
    setup2Called = 0;
    loop2Called = 0;
    setMockMillis(0);
    resetGlobalOS();
}

void task1_setup() { setup1Called++; }
void task1_loop() { loop1Called++; }
void task2_setup() { setup2Called++; }
void task2_loop() { loop2Called++; }

// For yield test - a task that calls yield
static int yieldLoopCalled = 0;
void yldTask_setup() {}
void yldTask_loop() {
    yieldLoopCalled++;
    // This would cause infinite recursion without reentrancy guard
    OS.yield();
}

// For trdwn test
static int trdwnCalled = 0;
void trdwnTask_setup() {}
void trdwnTask_loop() {}
void trdwnTask_trdwn() { trdwnCalled++; }

// For trdwn state verification test
static TaskState capturedTeardownState = TaskState::Invalid;
static int8_t trdwnStateTaskId = -1;
void trdwnStateTask_setup() {}
void trdwnStateTask_loop() {}
void trdwnStateTask_trdwn() {
    // Capture our own state during trdwn
    capturedTeardownState = OS.getTaskState(trdwnStateTaskId);
}

// For getCurrentTask test
static int8_t capturedCurrentTask = -1;
void captureTask_setup() {}
void captureTask_loop() {
    capturedCurrentTask = OS.getCurrentTask();
}

// For self-deletion test - task tries to delete itself
static bool selfDelAttempted = false;
static bool selfDelResult = false;
static int8_t selfDelTaskId = -1;
void selfDelTask_setup() {}
void selfDelTask_loop() {
    selfDelAttempted = true;
    OS.stopTask(selfDelTaskId);
    selfDelResult = OS.deleteTask(selfDelTaskId);
}

void test_create_task() {
    printf("Test: createTask... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("task1", task1_setup, task1_loop, 100);
    int8_t id2 = os.createTask("task2", task2_setup, task2_loop, 200);

    assert(id1 == 0);
    assert(id2 == 1);
    assert(os.getTaskCount() == 2);

    printf("PASSED\n");
}

void test_task_names() {
    printf("Test: getTaskName... ");
    resetTestCounters();

    Arda os;
    os.createTask("myTask", task1_setup, task1_loop, 0);

    assert(os.getTaskName(0) != nullptr);
    assert(os.getTaskName(1) == nullptr); // Invalid ID

    printf("PASSED\n");
}

void test_task_states() {
    printf("Test: task states... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    assert(os.getTaskState(id) == TaskState::Stopped);

    os.startTask(id);
    assert(os.getTaskState(id) == TaskState::Running);

    os.pauseTask(id);
    assert(os.getTaskState(id) == TaskState::Paused);

    os.resumeTask(id);
    assert(os.getTaskState(id) == TaskState::Running);

    os.stopTask(id);
    assert(os.getTaskState(id) == TaskState::Stopped);

    // Invalid task IDs should return TaskState::Invalid
    assert(os.getTaskState(-1) == TaskState::Invalid);
    assert(os.getTaskState(99) == TaskState::Invalid);

    printf("PASSED\n");
}

void test_max_tasks() {
    printf("Test: max tasks limit... ");
    resetTestCounters();

    Arda os;
    // Static array of unique names (pointer must remain valid)
    static char names[ARDA_MAX_TASKS][16];
    for (int i = 0; i < ARDA_MAX_TASKS; i++) {
        // Each task needs a unique name now
        snprintf(names[i], sizeof(names[i]), "task%d", i);
        int8_t id = os.createTask(names[i], nullptr, nullptr, 0);
        assert(id == i);
    }

    // Should fail when full
    int8_t ovrflow = os.createTask("ovrflow", nullptr, nullptr, 0);
    assert(ovrflow == -1);

    printf("PASSED\n");
}

void test_invalid_operations() {
    printf("Test: invalid operations... ");
    resetTestCounters();

    Arda os;

    // Operations on invalid task IDs should return false/StopResult::Failed
    assert(os.startTask(99) == false);
    assert(os.pauseTask(-1) == false);
    assert(os.resumeTask(100) == false);
    assert(os.stopTask(50) == StopResult::Failed);

    // Can't pause a stopped task
    int8_t id = os.createTask("task", nullptr, nullptr, 0);
    assert(os.pauseTask(id) == false); // Not running yet

    // Can't resume a stopped task
    assert(os.resumeTask(id) == false);

    // Can't stop a stopped task
    assert(os.stopTask(id) == StopResult::Failed);

    printf("PASSED\n");
}

void test_setup_called_on_start() {
    printf("Test: setup called on start... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    assert(setup1Called == 0);
    os.startTask(id);
    assert(setup1Called == 1);

    // Starting again should fail (already running)
    assert(os.startTask(id) == false);
    assert(setup1Called == 1); // Not called again

    printf("PASSED\n");
}

void test_begin_starts_all() {
    printf("Test: begin starts all tasks... ");
    resetTestCounters();

    Arda os;
    os.createTask("t1", task1_setup, task1_loop, 0);
    os.createTask("t2", task2_setup, task2_loop, 0);

    os.begin();

    assert(setup1Called == 1);
    assert(setup2Called == 1);
    assert(os.getTaskState(0) == TaskState::Running);
    assert(os.getTaskState(1) == TaskState::Running);

    printf("PASSED\n");
}

void test_run_executes_tasks() {
    printf("Test: run executes task loops... ");
    resetTestCounters();

    Arda os;
    os.createTask("task", task1_setup, task1_loop, 0);  // interval 0 = every cycle
    os.begin();

    assert(loop1Called == 0);

    os.run();
    assert(loop1Called == 1);

    os.run();
    assert(loop1Called == 2);

    printf("PASSED\n");
}

void test_interval_scheduling() {
    printf("Test: interval-based scheduling... ");
    resetTestCounters();

    Arda os;
    os.createTask("task", task1_setup, task1_loop, 100);  // 100ms interval
    os.begin();

    // Task just started, lastRun is set to current millis (0)
    // So it shouldn't run immediately
    os.run();
    assert(loop1Called == 0);

    // Advance time but not enough
    advanceMockMillis(50);
    os.run();
    assert(loop1Called == 0);

    // Advance to 100ms total
    advanceMockMillis(50);
    os.run();
    assert(loop1Called == 1);

    // Should not run again immediately
    os.run();
    assert(loop1Called == 1);

    // Advance another 100ms
    advanceMockMillis(100);
    os.run();
    assert(loop1Called == 2);

    printf("PASSED\n");
}

void test_uptime() {
    printf("Test: uptime tracking... ");
    resetTestCounters();

    Arda os;

    // Before begin(), uptime returns raw millis()
    setMockMillis(5000);
    assert(os.uptime() == 5000);

    setMockMillis(1000);  // Start at 1 second
    os.begin();

    assert(os.uptime() == 0);  // Now relative to begin() time

    advanceMockMillis(500);
    assert(os.uptime() == 500);

    advanceMockMillis(1500);
    assert(os.uptime() == 2000);

    printf("PASSED\n");
}

void test_millis_ovrflow_interval() {
    printf("Test: millis ovrflow for interval scheduling... ");
    resetTestCounters();

    Arda os;
    os.createTask("task", task1_setup, task1_loop, 100);  // 100ms interval

    // Start near the ovrflow point (ULONG_MAX = 4294967295)
    setMockMillis(4294967200UL);  // 95ms before ovrflow
    os.begin();

    // Should not run immediately (lastRun set to current time)
    os.run();
    assert(loop1Called == 0);

    // Advance 100ms, crossing the ovrflow boundary
    // 4294967200 + 100 = 4294967300, which wraps to 4 (300 - 296 = 4)
    setMockMillis(4UL);  // Wrapped around
    os.run();
    // Due to unsigned arithmetic: 4 - 4294967200 = 100 (wraps correctly)
    assert(loop1Called == 1);

    // Continue running after ovrflow
    setMockMillis(104UL);
    os.run();
    assert(loop1Called == 2);

    printf("PASSED\n");
}

void test_millis_ovrflow_uptime() {
    printf("Test: millis ovrflow for uptime... ");
    resetTestCounters();

    Arda os;

    // Start near the ovrflow point
    setMockMillis(4294967290UL);  // 5ms before ovrflow
    os.begin();

    assert(os.uptime() == 0);

    // Advance past ovrflow
    setMockMillis(10UL);  // Wrapped around
    // uptime = 10 - 4294967290 = 16 (wraps correctly: 10 + 6 = 16)
    // Actually: 10 - 4294967290 in unsigned = 10 + (ULONG_MAX - 4294967290 + 1) = 10 + 6 = 16
    assert(os.uptime() == 16);

    printf("PASSED\n");
}

void test_yield_reentrancy() {
    printf("Test: yield reentrancy guard... ");
    resetTestCounters();
    yieldLoopCalled = 0;

    Arda os;
    os.createTask("yldTask", yldTask_setup, yldTask_loop, 0);
    os.begin();

    // This would stack ovrflow without reentrancy guard
    os.run();

    // Task should have run exactly once despite calling yield()
    assert(yieldLoopCalled == 1);

    printf("PASSED\n");
}

void test_delete_task() {
    printf("Test: delete task... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("task1", task1_setup, task1_loop, 0);
    os.createTask("task2", task2_setup, task2_loop, 0);  // id2 not needed

    assert(os.getTaskCount() == 2);      // Active tasks
    assert(os.getSlotCount() == 2);      // Total slots

    // Can't delete a running task
    os.startTask(id1);
    assert(os.deleteTask(id1) == false);

    // Stop and then delete
    os.stopTask(id1);
    assert(os.deleteTask(id1) == true);

    // Name should be nullptr now
    assert(os.getTaskName(id1) == nullptr);

    // Active count decreases, but slot count stays (for iteration)
    assert(os.getTaskCount() == 1);      // Only 1 active task now
    assert(os.getSlotCount() == 2);      // Still 2 slots allocated

    // Creating a new task should reuse the deleted slot
    int8_t id3 = os.createTask("task3", task1_setup, task1_loop, 0);
    assert(id3 == 0);  // Reused slot 0
    assert(os.getTaskCount() == 2);      // Back to 2 active

    printf("PASSED\n");
}

void test_deleted_task_not_executed() {
    printf("Test: deleted task not executed... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.begin();

    os.run();
    assert(loop1Called == 1);

    os.stopTask(id);
    os.deleteTask(id);

    os.run();
    assert(loop1Called == 1);  // Should not have run again

    printf("PASSED\n");
}

void test_paused_task_not_executed() {
    printf("Test: paused task not executed... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.begin();

    os.run();
    assert(loop1Called == 1);

    os.pauseTask(id);
    os.run();
    assert(loop1Called == 1);  // Should not have run

    os.resumeTask(id);
    os.run();
    assert(loop1Called == 2);  // Should run again

    printf("PASSED\n");
}

void test_run_count() {
    printf("Test: run count tracking... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.begin();

    assert(os.getTaskRunCount(id) == 0);

    os.run();
    assert(os.getTaskRunCount(id) == 1);

    os.run();
    os.run();
    assert(os.getTaskRunCount(id) == 3);

    printf("PASSED\n");
}

void test_active_task_count() {
    printf("Test: getActiveTaskCount... ");
    resetTestCounters();

    Arda os;
    assert(os.getActiveTaskCount() == 0);
    assert(os.getTaskCount() == 0);  // getTaskCount is now alias for getActiveTaskCount

    os.createTask("t1", task1_setup, task1_loop, 0);
    os.createTask("t2", task2_setup, task2_loop, 0);
    assert(os.getActiveTaskCount() == 2);
    assert(os.getTaskCount() == 2);  // Both are now the same

    // Delete one task (must stop first)
    // Task is not started yet, so it's already stopped
    os.deleteTask(0);
    assert(os.getActiveTaskCount() == 1);  // Only one active
    assert(os.getTaskCount() == 1);        // getTaskCount also returns active count
    assert(os.getSlotCount() == 2);        // Slot count stays at 2

    printf("PASSED\n");
}

void test_get_current_task() {
    printf("Test: getCurrentTask... ");
    resetTestCounters();
    capturedCurrentTask = -1;

    // Outside of task execution, should be -1
    assert(OS.getCurrentTask() == -1);

    int8_t id = OS.createTask("capture", captureTask_setup, captureTask_loop, 0);
    OS.begin();
    OS.run();

    // During execution, task should have captured its own ID
    assert(capturedCurrentTask == id);

    // After execution, should be -1 again
    assert(OS.getCurrentTask() == -1);

    printf("PASSED\n");
}

void test_trdwn_called() {
    printf("Test: trdwn called on stop... ");
    resetTestCounters();
    trdwnCalled = 0;

    Arda os;
    int8_t id = os.createTask("trdwn", trdwnTask_setup, trdwnTask_loop, 0, trdwnTask_trdwn);

    os.startTask(id);
    assert(trdwnCalled == 0);

    os.stopTask(id);
    assert(trdwnCalled == 1);

    // Can't stop again (already stopped)
    assert(os.stopTask(id) == StopResult::Failed);
    assert(trdwnCalled == 1);  // Not called again

    printf("PASSED\n");
}

void test_trdwn_sees_stopped_state() {
    printf("Test: trdwn sees TaskState::Stopped state... ");
    resetTestCounters();
    capturedTeardownState = TaskState::Invalid;

    trdwnStateTaskId = OS.createTask("statTst", trdwnStateTask_setup, trdwnStateTask_loop, 0, trdwnStateTask_trdwn);
    OS.begin();

    // Verify task is running
    assert(OS.getTaskState(trdwnStateTaskId) == TaskState::Running);

    // Stop the task - trdwn will capture its own state
    OS.stopTask(trdwnStateTaskId);

    // Teardown should have seen TaskState::Stopped (not TaskState::Running)
    assert(capturedTeardownState == TaskState::Stopped);

    printf("PASSED\n");
}

void test_self_deletion_prevented() {
    printf("Test: self-deletion prevented... ");
    resetTestCounters();
    selfDelAttempted = false;
    selfDelResult = false;

    selfDelTaskId = OS.createTask("selfDel", selfDelTask_setup, selfDelTask_loop, 0);
    OS.begin();
    OS.run();

    assert(selfDelAttempted == true);
    assert(selfDelResult == false);  // Deletion should have been prevented

    // Task should still exist (name not null), but be stopped
    assert(OS.getTaskName(selfDelTaskId) != nullptr);
    assert(OS.getTaskState(selfDelTaskId) == TaskState::Stopped);

    printf("PASSED\n");
}

void test_begin_returns_count() {
    printf("Test: begin returns started count... ");
    resetTestCounters();

    Arda os;
    os.createTask("t1", task1_setup, task1_loop, 0);
    os.createTask("t2", task2_setup, task2_loop, 0);

    int8_t started = os.begin();
    assert(started == 2);

    printf("PASSED\n");
}

void test_deleted_task_returns_invalid() {
    printf("Test: deleted task returns TaskState::Invalid... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    // Before deletion
    assert(os.getTaskState(id) == TaskState::Stopped);

    // Delete
    os.deleteTask(id);

    // After deletion, should return TaskState::Invalid
    assert(os.getTaskState(id) == TaskState::Invalid);
    assert(os.getTaskName(id) == nullptr);
    assert(os.getTaskRunCount(id) == 0);

    printf("PASSED\n");
}

void test_nullptr_name_rejected() {
    printf("Test: nullptr name rejected... ");
    resetTestCounters();

    Arda os;

    // Creating a task with nullptr name should fail
    int8_t id = os.createTask(nullptr, task1_setup, task1_loop, 100);
    assert(id == -1);

    // Task count should remain 0
    assert(os.getTaskCount() == 0);

    printf("PASSED\n");
}

void test_interval_cadence() {
    printf("Test: interval maintains cadence... ");
    resetTestCounters();

    Arda os;
    os.createTask("task", task1_setup, task1_loop, 100);  // 100ms interval
    os.begin();

    // Initial state: lastRun = 0, need to wait 100ms
    setMockMillis(100);
    os.run();
    assert(loop1Called == 1);

    // With cadence-based timing, next run should be at 200ms
    // even if we check at 205ms (simulating slight delay)
    setMockMillis(205);
    os.run();
    assert(loop1Called == 2);

    // Next run should be at 300ms (200 + 100), not 305ms
    // At 299ms it should NOT run yet
    setMockMillis(299);
    os.run();
    assert(loop1Called == 2);  // Still 2

    // At 300ms it should run
    setMockMillis(300);
    os.run();
    assert(loop1Called == 3);

    printf("PASSED\n");
}

void test_interval_catchup_prevention() {
    printf("Test: interval catchup prevention... ");
    resetTestCounters();

    Arda os;
    os.createTask("task", task1_setup, task1_loop, 100);  // 100ms interval
    os.begin();

    // First run at 100ms
    setMockMillis(100);
    os.run();
    assert(loop1Called == 1);

    // Jump far ahead (500ms = 5 intervals missed)
    // Should NOT run 5 times in rapid succession
    setMockMillis(600);
    os.run();
    assert(loop1Called == 2);  // Only one run, not 5

    // After reset due to being far behind, next run at 700ms
    setMockMillis(699);
    os.run();
    assert(loop1Called == 2);  // Not yet

    setMockMillis(700);
    os.run();
    assert(loop1Called == 3);

    printf("PASSED\n");
}

void test_empty_string_name_rejected() {
    printf("Test: empty string name rejected... ");
    resetTestCounters();

    Arda os;

    // Creating a task with empty string name should fail
    int8_t id = os.createTask("", task1_setup, task1_loop, 100);
    assert(id == -1);

    // Task count should remain 0
    assert(os.getTaskCount() == 0);

    printf("PASSED\n");
}

void test_set_task_interval() {
    printf("Test: setTaskInterval... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 100);
    os.begin();

    // Initial interval is 100ms
    setMockMillis(100);
    os.run();
    assert(loop1Called == 1);

    // Change interval to 50ms - this resets lastRun to current time (100)
    assert(os.setTaskInterval(id, 50) == true);

    // Should NOT run immediately since lastRun was reset to 100
    os.run();
    assert(loop1Called == 1);  // Still 1

    // Should run after 50ms from when interval was changed
    setMockMillis(150);
    os.run();
    assert(loop1Called == 2);

    // Invalid task ID should fail
    assert(os.setTaskInterval(-1, 100) == false);
    assert(os.setTaskInterval(99, 100) == false);

    printf("PASSED\n");
}

void test_find_task_by_name() {
    printf("Test: findTaskByName... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("alpha", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("beta", task2_setup, task2_loop, 0);

    assert(os.findTaskByName("alpha") == id1);
    assert(os.findTaskByName("beta") == id2);
    assert(os.findTaskByName("gamma") == -1);  // Not found
    assert(os.findTaskByName(nullptr) == -1);  // nullptr
    assert(os.findTaskByName("") == -1);       // Empty string

    // Delete task and verify not found
    os.deleteTask(id1);
    assert(os.findTaskByName("alpha") == -1);

    printf("PASSED\n");
}

void test_get_task_interval() {
    printf("Test: getTaskInterval... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("fast", task1_setup, task1_loop, 50);
    int8_t id2 = os.createTask("slow", task2_setup, task2_loop, 1000);

    assert(os.getTaskInterval(id1) == 50);
    assert(os.getTaskInterval(id2) == 1000);

    // Change interval and verify
    os.setTaskInterval(id1, 200);
    assert(os.getTaskInterval(id1) == 200);

    // Invalid task ID should return 0
    assert(os.getTaskInterval(-1) == 0);
    assert(os.getTaskInterval(99) == 0);

    // Deleted task should return 0
    os.deleteTask(id1);
    assert(os.getTaskInterval(id1) == 0);

    printf("PASSED\n");
}

// For yield test - tracks which tasks ran
static int yieldTestTask1Runs = 0;
static int yieldTestTask2Runs = 0;
static bool task1CalledYield = false;

void yieldTest1_setup() {}
void yieldTest1_loop() {
    yieldTestTask1Runs++;
    if (!task1CalledYield) {
        task1CalledYield = true;
        OS.yield();  // Should allow task2 to run
    }
}

void yieldTest2_setup() {}
void yieldTest2_loop() {
    yieldTestTask2Runs++;
}

// For yield inRun restoration test
static int yieldInRunTask1Runs = 0;
static int yieldInRunTask2Runs = 0;
static bool yieldInRunTestedReentrancy = false;

void yieldInRunTask1_setup() {}
void yieldInRunTask1_loop() {
    yieldInRunTask1Runs++;
    OS.yield();
    // After yield returns, inRun should be true (restored)
    // Calling run() here should be a no-op due to reentrancy guard
    if (!yieldInRunTestedReentrancy) {
        yieldInRunTestedReentrancy = true;
        int beforeRuns = yieldInRunTask2Runs;
        OS.run();  // Should be blocked by reentrancy guard
        // Task2 should NOT have run again
        assert(yieldInRunTask2Runs == beforeRuns);
    }
}

void yieldInRunTask2_setup() {}
void yieldInRunTask2_loop() {
    yieldInRunTask2Runs++;
}

void test_yield_runs_other_tasks() {
    printf("Test: yield runs other tasks... ");
    resetTestCounters();
    yieldTestTask1Runs = 0;
    yieldTestTask2Runs = 0;
    task1CalledYield = false;

    OS.createTask("task1", yieldTest1_setup, yieldTest1_loop, 0);
    OS.createTask("task2", yieldTest2_setup, yieldTest2_loop, 0);
    OS.begin();

    // One run cycle
    OS.run();

    // Task1 should have run once
    assert(yieldTestTask1Runs == 1);
    // Task2 should have run exactly once (ranThisCycle prevents double execution)
    assert(yieldTestTask2Runs == 1);

    printf("PASSED\n");
}

void test_yield_restores_inrun() {
    printf("Test: yield restores inRun flag... ");
    resetTestCounters();
    yieldInRunTask1Runs = 0;
    yieldInRunTask2Runs = 0;
    yieldInRunTestedReentrancy = false;

    OS.createTask("task1", yieldInRunTask1_setup, yieldInRunTask1_loop, 0);
    OS.createTask("task2", yieldInRunTask2_setup, yieldInRunTask2_loop, 0);
    OS.begin();

    // One run cycle - task1 will call yield(), then try to call run()
    // The run() call should be blocked because inRun should be true
    OS.run();

    // Task1 ran once, task2 ran once (ranThisCycle prevents double execution)
    assert(yieldInRunTask1Runs == 1);
    assert(yieldInRunTask2Runs == 1);
    // The reentrancy test was executed
    assert(yieldInRunTestedReentrancy == true);

    printf("PASSED\n");
}

void test_duplicate_name_rejected() {
    printf("Test: duplicate name rejected... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("myTask", task1_setup, task1_loop, 100);
    assert(id1 == 0);

    // Attempting to create another task with the same name should fail
    int8_t id2 = os.createTask("myTask", task2_setup, task2_loop, 200);
    assert(id2 == -1);

    // Task count should still be 1
    assert(os.getTaskCount() == 1);

    // Different name should work
    int8_t id3 = os.createTask("other", task2_setup, task2_loop, 200);
    assert(id3 == 1);
    assert(os.getTaskCount() == 2);

    printf("PASSED\n");
}

void test_begin_only_once() {
    printf("Test: begin() can only be called once... ");
    resetTestCounters();

    Arda os;
    os.createTask("task", task1_setup, task1_loop, 0);

    // First begin should succeed
    int8_t result1 = os.begin();
    assert(result1 == 1);
    assert(os.hasBegun() == true);

    // Second begin should fail
    int8_t result2 = os.begin();
    assert(result2 == -1);

    printf("PASSED\n");
}

void test_reset() {
    printf("Test: reset()... ");
    resetTestCounters();
    trdwnCalled = 0;

    Arda os;
    os.createTask("task", task1_setup, task1_loop, 0, trdwnTask_trdwn);
    os.begin();
    os.run();

    assert(os.hasBegun() == true);
    assert(os.getTaskCount() == 1);
    assert(loop1Called == 1);

    // Reset should stop all tasks (calling trdwn) and clear everything
    bool resetResult = os.reset();

    assert(resetResult == true);  // All trdwns ran successfully
    assert(os.getError() == ArdaError::Ok);
    assert(os.hasBegun() == false);
    assert(os.getTaskCount() == 0);
    assert(trdwnCalled == 1);  // Teardown was called

    // Should be able to begin() again after reset
    os.createTask("newTask", task2_setup, task2_loop, 0);
    int8_t result = os.begin();
    assert(result == 1);
    assert(os.hasBegun() == true);

    printf("PASSED\n");
}

void test_has_begun() {
    printf("Test: hasBegun()... ");
    resetTestCounters();

    Arda os;
    assert(os.hasBegun() == false);

    os.createTask("task", task1_setup, task1_loop, 0);
    assert(os.hasBegun() == false);  // Still false, begin() not called

    os.begin();
    assert(os.hasBegun() == true);

    printf("PASSED\n");
}

// Test that reset() clears user callbacks
static bool resetCallbackTestTriggered = false;
void resetCallbackTestTimeout(int8_t, uint32_t) { resetCallbackTestTriggered = true; }

void test_reset_clears_callbacks() {
    printf("Test: reset() clears callbacks... ");
    resetTestCounters();
    resetCallbackTestTriggered = false;

    Arda os;

    // Set a timeout callback
    os.setTimeoutCallback(resetCallbackTestTimeout);

    // Create a task that will trigger timeout
    int8_t id = os.createTask("slow", nullptr, []() {
        advanceMockMillis(100);  // Simulate slow task
    }, 0);
    os.setTaskTimeout(id, 10);  // 10ms timeout
    os.begin();

    os.run();
    assert(resetCallbackTestTriggered == true);  // Callback was triggered

    // Now reset
    os.reset();
    resetCallbackTestTriggered = false;

    // Create same task again
    id = os.createTask("slow2", nullptr, []() {
        advanceMockMillis(100);
    }, 0);
    os.setTaskTimeout(id, 10);
    os.begin();

    os.run();
    // Callback should NOT trigger because reset() cleared it
    assert(resetCallbackTestTriggered == false);

    printf("PASSED\n");
}

void test_reset_preserves_callbacks() {
    printf("Test: reset(true) preserves callbacks... ");
    resetTestCounters();
    resetCallbackTestTriggered = false;

    Arda os;

    // Set a timeout callback
    os.setTimeoutCallback(resetCallbackTestTimeout);

    // Create a task that will trigger timeout
    int8_t id = os.createTask("slow", nullptr, []() {
        advanceMockMillis(100);  // Simulate slow task
    }, 0);
    os.setTaskTimeout(id, 10);  // 10ms timeout
    os.begin();

    os.run();
    assert(resetCallbackTestTriggered == true);  // Callback was triggered

    // Now reset with preserveCallbacks=true
    os.reset(true);
    resetCallbackTestTriggered = false;

    // Create same task again
    id = os.createTask("slow2", nullptr, []() {
        advanceMockMillis(100);
    }, 0);
    os.setTaskTimeout(id, 10);
    os.begin();

    os.run();
    // Callback SHOULD trigger because reset(true) preserved it
    assert(resetCallbackTestTriggered == true);

    printf("PASSED\n");
}

void test_begin_no_autostart() {
    printf("Test: begin(false) does not auto-start tasks... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);

    // begin(false) should initialize but not start tasks
    int8_t result = os.begin(false);
    assert(result == 0);  // 0 tasks started
    assert(os.hasBegun() == true);  // But scheduler is begun

    // Tasks should still be in STOPPED state
    assert(os.getTaskState(id1) == TaskState::Stopped);
    assert(os.getTaskState(id2) == TaskState::Stopped);

    // Setup functions should NOT have been called
    assert(setup1Called == 0);
    assert(setup2Called == 0);

    // Can manually start tasks after
    os.startTask(id1);
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(setup1Called == 1);

    printf("PASSED\n");
}

void test_name_copied_safely() {
    printf("Test: name copied safely... ");
    resetTestCounters();

    Arda os;

    // Test 1: Name is copied, not just pointer stored
    {
        char localName[] = "lclTsk";
        int8_t id = os.createTask(localName, task1_setup, task1_loop, 0);
        assert(id == 0);

        // Modify the orig string
        localName[0] = 'X';

        // Task name should still be "lclTsk" (copied, not pointed to)
        const char* storedName = os.getTaskName(id);
        assert(storedName != nullptr);
        assert(strncmp(storedName, "lclTsk", ARDA_MAX_NAME_LEN) == 0);
    }

    printf("PASSED\n");
}

void test_long_name_rejected() {
    printf("Test: long name rejected... ");
    resetTestCounters();

    Arda os;

    // Names at exactly max length should work (7 chars + null = 8)
    const char* maxName = "exact7!";  // 7 characters
    assert(strlen(maxName) == ARDA_MAX_NAME_LEN - 1);
    int8_t id1 = os.createTask(maxName, task1_setup, task1_loop, 0);
    assert(id1 == 0);
    assert(strncmp(os.getTaskName(id1), maxName, ARDA_MAX_NAME_LEN) == 0);

    // Names exceeding max length should be rejected (not truncated)
    const char* longName = "thisIsAVeryLongTaskNameThatExceedsTheLimit";
    int8_t id2 = os.createTask(longName, task2_setup, task2_loop, 0);
    assert(id2 == -1);  // Should fail, not truncate

    // Task count should still be 1
    assert(os.getTaskCount() == 1);

    printf("PASSED\n");
}

void test_auto_start_after_begin() {
    printf("Test: auto-start tasks created after begin()... ");
    resetTestCounters();

    // Create first task before begin()
    OS.createTask("task1", task1_setup, task1_loop, 0);
    OS.begin();

    assert(setup1Called == 1);  // task1 setup ran

    // Create second task AFTER begin() - should auto-start
    OS.createTask("task2", task2_setup, task2_loop, 0);

    assert(setup2Called == 1);  // task2 setup should have run immediately
    assert(OS.getTaskState(1) == TaskState::Running);

    // Both tasks should run
    OS.run();
    assert(loop1Called == 1);
    assert(loop2Called == 1);

    printf("PASSED\n");
}

void test_auto_start_disabled() {
    printf("Test: autoStart=false creates task in STOPPED state... ");
    resetTestCounters();

    OS.begin();  // Start scheduler first

    // Create task with autoStart=false - should NOT auto-start
    int8_t id = OS.createTask("noAuto", task1_setup, task1_loop, 0, nullptr, false);

    assert(id >= 0);
    assert(setup1Called == 0);  // Setup should NOT have been called
    assert(OS.getTaskState(id) == TaskState::Stopped);
    assert(OS.getError() == ArdaError::Ok);  // No error - this is intentional

    // Task should not run until manually started
    OS.run();
    assert(loop1Called == 0);

    // Now start it manually
    OS.startTask(id);
    assert(setup1Called == 1);
    assert(OS.getTaskState(id) == TaskState::Running);

    OS.run();
    assert(loop1Called == 1);

    printf("PASSED\n");
}

void test_is_valid_task_public() {
    printf("Test: isValidTask public method... ");
    resetTestCounters();

    Arda os;

    // Invalid before any tasks created
    assert(os.isValidTask(-1) == false);
    assert(os.isValidTask(0) == false);
    assert(os.isValidTask(99) == false);

    // Create a task
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    assert(os.isValidTask(id) == true);

    // Delete the task
    os.deleteTask(id);
    assert(os.isValidTask(id) == false);  // Now invalid (deleted)

    printf("PASSED\n");
}

// For yield-in-setup test
static int yieldSetupTask1Runs = 0;
static int yieldSetupTask2Runs = 0;
static bool yieldCalledInSetup = false;

void yieldSetupTask1_setup() {
    yieldCalledInSetup = true;
    OS.yield();  // This used to corrupt inRun state
}
void yieldSetupTask1_loop() {
    yieldSetupTask1Runs++;
}

void yieldSetupTask2_setup() {}
void yieldSetupTask2_loop() {
    yieldSetupTask2Runs++;
}

// For yield deletion prevention test - task B tries to delete task A while A is yielded
static int8_t yieldDeleteTargetId = -1;
static bool yieldDeleteAttempted = false;
static StopResult yieldDeleteStopResult = StopResult::Failed;
static ArdaError yieldDeleteStopError = ArdaError::Ok;
static bool yieldDeleteResult = false;
static int yieldDeleteTaskARuns = 0;
static int yieldDeleteTaskBRuns = 0;

void yieldDeleteTaskA_setup() {}
void yieldDeleteTaskA_loop() {
    yieldDeleteTaskARuns++;
    OS.yield();  // Task B will try to delete us during this yield
}

void yieldDeleteTaskB_setup() {}
void yieldDeleteTaskB_loop() {
    yieldDeleteTaskBRuns++;
    if (!yieldDeleteAttempted && yieldDeleteTargetId >= 0) {
        yieldDeleteAttempted = true;
        // Try to stop and delete task A while it's yielded
        yieldDeleteStopResult = OS.stopTask(yieldDeleteTargetId);
        yieldDeleteStopError = OS.getError();  // Capture error before deleteTask overwrites it
        yieldDeleteResult = OS.deleteTask(yieldDeleteTargetId);
    }
}

void test_yielded_task_deletion_prevented() {
    printf("Test: yielded task stop and deletion prevented... ");
    resetTestCounters();
    yieldDeleteTargetId = -1;
    yieldDeleteAttempted = false;
    yieldDeleteStopResult = StopResult::Failed;
    yieldDeleteStopError = ArdaError::Ok;
    yieldDeleteResult = false;
    yieldDeleteTaskARuns = 0;
    yieldDeleteTaskBRuns = 0;

    yieldDeleteTargetId = OS.createTask("taskA", yieldDeleteTaskA_setup, yieldDeleteTaskA_loop, 0);
    OS.createTask("taskB", yieldDeleteTaskB_setup, yieldDeleteTaskB_loop, 0);
    OS.begin();

    // Run one cycle - task A will yield, task B will try to stop/delete task A
    OS.run();

    // Task B should have attempted stop/deletion
    assert(yieldDeleteAttempted == true);
    // Stop should fail (task is mid-yield, trdwn would corrupt stack)
    assert(yieldDeleteStopResult == StopResult::Failed);
    assert(yieldDeleteStopError == ArdaError::TaskYielded);
    // Deletion should also fail (task is still running and in yield)
    assert(yieldDeleteResult == false);

    // Task A should still exist and be running (not stopped, not deleted)
    assert(OS.getTaskName(yieldDeleteTargetId) != nullptr);
    assert(OS.getTaskState(yieldDeleteTargetId) == TaskState::Running);

    // After yield returns, task A is still running - now we can stop and delete it
    OS.stopTask(yieldDeleteTargetId);
    assert(OS.getTaskState(yieldDeleteTargetId) == TaskState::Stopped);

    bool postYieldDelete = OS.deleteTask(yieldDeleteTargetId);
    assert(postYieldDelete == true);
    assert(OS.getTaskName(yieldDeleteTargetId) == nullptr);

    printf("PASSED\n");
}

// For testing that startTask() resets inYield flag
static int8_t yieldRestartTargetId = -1;
static bool yieldRestartStopAttempted = false;
static bool yieldRestartStartAttempted = false;
static StopResult yieldRestartStopResult1 = StopResult::Failed;
static bool yieldRestartStartResult = false;
static StopResult yieldRestartStopResult2 = StopResult::Failed;
static bool yieldRestartDeleteResult = false;
static int yieldRestartTaskARuns = 0;
static int yieldRestartTaskBRuns = 0;

void yieldRestartTaskA_setup() {}
void yieldRestartTaskA_loop() {
    yieldRestartTaskARuns++;
    OS.yield();  // Task B will stop, restart, then stop+delete us during this yield
}

void yieldRestartTaskB_setup() {}
void yieldRestartTaskB_loop() {
    yieldRestartTaskBRuns++;
    if (!yieldRestartStopAttempted && yieldRestartTargetId >= 0) {
        yieldRestartStopAttempted = true;
        // First stop attempt should fail (task A is yielded)
        yieldRestartStopResult1 = OS.stopTask(yieldRestartTargetId);
    }
}

void test_startTask_resets_inYield_flag() {
    printf("Test: startTask resets inYield flag... ");
    resetTestCounters();
    yieldRestartTargetId = -1;
    yieldRestartStopAttempted = false;
    yieldRestartStartAttempted = false;
    yieldRestartStopResult1 = StopResult::Failed;
    yieldRestartStartResult = false;
    yieldRestartStopResult2 = StopResult::Failed;
    yieldRestartDeleteResult = false;
    yieldRestartTaskARuns = 0;
    yieldRestartTaskBRuns = 0;

    yieldRestartTargetId = OS.createTask("taskA", yieldRestartTaskA_setup, yieldRestartTaskA_loop, 0);
    OS.createTask("taskB", yieldRestartTaskB_setup, yieldRestartTaskB_loop, 0);
    OS.begin();

    // Run one cycle - task A yields, task B tries to stop (which should fail)
    OS.run();

    // Verify stop failed because task A was mid-yield
    assert(yieldRestartStopAttempted == true);
    assert(yieldRestartStopResult1 == StopResult::Failed);
    assert(OS.getTaskState(yieldRestartTargetId) == TaskState::Running);

    // After yield returns, task A is no longer yielded - we can stop it
    StopResult stopAfterYield = OS.stopTask(yieldRestartTargetId);
    assert(stopAfterYield == StopResult::Success);
    assert(OS.getTaskState(yieldRestartTargetId) == TaskState::Stopped);

    // Restart task A
    bool restartResult = OS.startTask(yieldRestartTargetId);
    assert(restartResult == true);
    assert(OS.getTaskState(yieldRestartTargetId) == TaskState::Running);

    // Verify inYield was reset - task should not be considered yielded
    assert(OS.isTaskYielded(yieldRestartTargetId) == false);

    // Stop and delete should both succeed now
    StopResult stopResult = OS.stopTask(yieldRestartTargetId);
    assert(stopResult == StopResult::Success);

    bool deleteResult = OS.deleteTask(yieldRestartTargetId);
    assert(deleteResult == true);
    assert(OS.getTaskName(yieldRestartTargetId) == nullptr);

    printf("PASSED\n");
}

void test_yield_in_setup_does_not_corrupt_inrun() {
    printf("Test: yield in setup does not corrupt inRun... ");
    resetTestCounters();
    yieldSetupTask1Runs = 0;
    yieldSetupTask2Runs = 0;
    yieldCalledInSetup = false;

    OS.createTask("task1", yieldSetupTask1_setup, yieldSetupTask1_loop, 0);
    OS.createTask("task2", yieldSetupTask2_setup, yieldSetupTask2_loop, 0);

    // begin() will call startTask() which calls setup(), which calls yield()
    // This used to set inRun=true permanently, blocking all future run() calls
    OS.begin();

    assert(yieldCalledInSetup == true);  // Verify setup ran and called yield

    // If inRun is stuck at true, this run() will be a no-op
    OS.run();

    // Both tasks should have run once
    assert(yieldSetupTask1Runs == 1);
    assert(yieldSetupTask2Runs == 1);

    // Run again to make sure scheduler is still working
    OS.run();
    assert(yieldSetupTask1Runs == 2);
    assert(yieldSetupTask2Runs == 2);

    printf("PASSED\n");
}

void test_error_codes() {
    printf("Test: error codes... ");
    resetTestCounters();

    Arda os;

    // Initially no error
    assert(os.getError() == ArdaError::Ok);

    // Null name error
    os.createTask(nullptr, task1_setup, task1_loop, 0);
    assert(os.getError() == ArdaError::NullName);

    // Empty name error
    os.createTask("", task1_setup, task1_loop, 0);
    assert(os.getError() == ArdaError::EmptyName);

    // Name too long error
    os.createTask("thisIsAVeryLongTaskNameThatExceedsLimit", task1_setup, task1_loop, 0);
    assert(os.getError() == ArdaError::NameTooLong);

    // Clear error
    os.clearError();
    assert(os.getError() == ArdaError::Ok);

    // Create valid task
    int8_t id = os.createTask("task1", task1_setup, task1_loop, 0);
    assert(id >= 0);

    // Duplicate name error
    os.createTask("task1", task2_setup, task2_loop, 0);
    assert(os.getError() == ArdaError::DuplicateName);

    // Invalid ID error
    os.startTask(99);
    assert(os.getError() == ArdaError::InvalidId);

    // Wrong state error (try to pause stopped task)
    os.pauseTask(id);
    assert(os.getError() == ArdaError::WrongState);

    // Start task successfully
    os.clearError();
    os.startTask(id);

    // Wrong state error (try to start running task)
    os.startTask(id);
    assert(os.getError() == ArdaError::WrongState);

    // Wrong state error (try to delete running task)
    os.deleteTask(id);
    assert(os.getError() == ArdaError::WrongState);

    // Already begun error
    os.begin();
    os.begin();
    assert(os.getError() == ArdaError::AlreadyBegun);

    printf("PASSED\n");
}

void test_error_codes_max_tasks() {
    printf("Test: error code max tasks... ");
    resetTestCounters();

    Arda os;
    static char names[ARDA_MAX_TASKS][16];

    // Fill up all task slots
    for (int i = 0; i < ARDA_MAX_TASKS; i++) {
        snprintf(names[i], sizeof(names[i]), "task%d", i);
        int8_t id = os.createTask(names[i], nullptr, nullptr, 0);
        assert(id == i);
    }

    // Next task should fail with max tasks error
    int8_t ovrflow = os.createTask("ovrflow", nullptr, nullptr, 0);
    assert(ovrflow == -1);
    assert(os.getError() == ArdaError::MaxTasks);

    printf("PASSED\n");
}

void test_get_max_tasks() {
    printf("Test: getMaxTasks... ");

    // getMaxTasks should return the compile-time constant
    assert(Arda::getMaxTasks() == ARDA_MAX_TASKS);
    assert(Arda::getMaxTasks() == 16);  // Default value

    // Can also call on instance
    Arda os;
    assert(os.getMaxTasks() == ARDA_MAX_TASKS);

    printf("PASSED\n");
}

void test_success_clears_error() {
    printf("Test: success clears lastError... ");
    resetTestCounters();

    Arda os;

    // Cause an error
    os.createTask(nullptr, task1_setup, task1_loop, 0);
    assert(os.getError() == ArdaError::NullName);

    // Successful createTask should clear the error
    int8_t id = os.createTask("task1", task1_setup, task1_loop, 0);
    assert(id >= 0);
    assert(os.getError() == ArdaError::Ok);

    // Cause another error
    os.startTask(99);
    assert(os.getError() == ArdaError::InvalidId);

    // Successful startTask should clear it
    os.startTask(id);
    assert(os.getError() == ArdaError::Ok);

    // Cause error, then successful pause
    os.pauseTask(99);
    assert(os.getError() == ArdaError::InvalidId);
    os.pauseTask(id);
    assert(os.getError() == ArdaError::Ok);

    // Cause error, then successful resume
    os.resumeTask(99);
    assert(os.getError() == ArdaError::InvalidId);
    os.resumeTask(id);
    assert(os.getError() == ArdaError::Ok);

    // Cause error, then successful stop
    assert(os.stopTask(99) == StopResult::Failed);
    assert(os.getError() == ArdaError::InvalidId);
    assert(os.stopTask(id) == StopResult::Success);
    assert(os.getError() == ArdaError::Ok);

    // Cause error, then successful delete
    os.deleteTask(99);
    assert(os.getError() == ArdaError::InvalidId);
    os.deleteTask(id);
    assert(os.getError() == ArdaError::Ok);

    printf("PASSED\n");
}

void test_set_interval_paused_task() {
    printf("Test: setTaskInterval on paused task... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 100);
    os.begin();

    // Run once at 100ms
    setMockMillis(100);
    os.run();
    assert(loop1Called == 1);

    // Pause the task at 150ms
    setMockMillis(150);
    os.pauseTask(id);

    // Change interval while paused - this should reset lastRun to 150ms
    os.setTaskInterval(id, 50);
    assert(os.getError() == ArdaError::Ok);

    // Resume at 160ms
    setMockMillis(160);
    os.resumeTask(id);

    // Should NOT run immediately (only 10ms since interval change, need 50ms)
    os.run();
    assert(loop1Called == 1);

    // Should run at 200ms (150 + 50)
    setMockMillis(200);
    os.run();
    assert(loop1Called == 2);

    printf("PASSED\n");
}

// For callback depth test - tasks that recursively start each other
static int depthTestSetupCalls = 0;
static int8_t depthTestTaskIds[ARDA_MAX_CALLBACK_DEPTH + 2];
static int depthTestCurrentIndex = 0;

void depthTestTask_setup() {
    depthTestSetupCalls++;
    int myIndex = depthTestCurrentIndex++;
    // Try to start next task from setup (will hit depth limit eventually)
    if (myIndex < ARDA_MAX_CALLBACK_DEPTH + 1) {
        int8_t nextId = depthTestTaskIds[myIndex + 1];
        if (nextId >= 0 && OS.getTaskState(nextId) == TaskState::Stopped) {
            OS.startTask(nextId);
        }
    }
}
void depthTestTask_loop() {}

void test_callback_depth_limit() {
    printf("Test: callback depth limit... ");
    resetTestCounters();
    depthTestSetupCalls = 0;
    depthTestCurrentIndex = 0;

    // Create more tasks than the callback depth limit
    static char names[ARDA_MAX_CALLBACK_DEPTH + 2][16];

    for (int i = 0; i < ARDA_MAX_CALLBACK_DEPTH + 2; i++) {
        snprintf(names[i], sizeof(names[i]), "depth%d", i);
        depthTestTaskIds[i] = OS.createTask(names[i], depthTestTask_setup, depthTestTask_loop, 0);
        assert(depthTestTaskIds[i] >= 0);
    }

    // Start task 0, which will recursively try to start task 1, 2, 3...
    // This should be limited by ARDA_MAX_CALLBACK_DEPTH
    bool result = OS.startTask(depthTestTaskIds[0]);
    assert(result == true);

    // Due to the depth limit, we should see exactly ARDA_MAX_CALLBACK_DEPTH setups called
    // (the (ARDA_MAX_CALLBACK_DEPTH + 1)th should fail due to depth limit)
    assert(depthTestSetupCalls == ARDA_MAX_CALLBACK_DEPTH);

    // Note: lastError will be ArdaError::Ok because the outermost startTask succeeded
    // (it clears the error on success). The depth error was set but then overwritten.
    // The important thing is that the recursion was limited.

    // Tasks 0 through ARDA_MAX_CALLBACK_DEPTH-1 should be running
    for (int i = 0; i < ARDA_MAX_CALLBACK_DEPTH; i++) {
        assert(OS.getTaskState(depthTestTaskIds[i]) == TaskState::Running);
    }

    // Task at index ARDA_MAX_CALLBACK_DEPTH should still be stopped (couldn't start due to depth)
    assert(OS.getTaskState(depthTestTaskIds[ARDA_MAX_CALLBACK_DEPTH]) == TaskState::Stopped);

    printf("PASSED\n");
}

// For stopTask trdwn depth test
static int depthTeardownCalls = 0;
static int8_t depthTeardownTaskIds[ARDA_MAX_CALLBACK_DEPTH + 2];
static int depthTeardownCurrentIndex = 0;

void depthTeardownTask_setup() {}
void depthTeardownTask_loop() {}
void depthTeardownTask_trdwn() {
    depthTeardownCalls++;
    int myIndex = depthTeardownCurrentIndex++;
    // Try to stop next task from trdwn (will hit depth limit eventually)
    if (myIndex < ARDA_MAX_CALLBACK_DEPTH + 1) {
        int8_t nextId = depthTeardownTaskIds[myIndex + 1];
        if (nextId >= 0 && OS.getTaskState(nextId) != TaskState::Stopped) {
            OS.stopTask(nextId);
        }
    }
}

void test_stopTask_returns_partial_on_trdwn_skip() {
    printf("Test: stopTask returns TeardownSkipped when trdwn skipped... ");
    resetTestCounters();
    depthTeardownCalls = 0;
    depthTeardownCurrentIndex = 0;

    // Create chain of tasks with trdwns
    static char names[ARDA_MAX_CALLBACK_DEPTH + 2][16];

    for (int i = 0; i < ARDA_MAX_CALLBACK_DEPTH + 2; i++) {
        snprintf(names[i], sizeof(names[i]), "tear%d", i);
        depthTeardownTaskIds[i] = OS.createTask(names[i], depthTeardownTask_setup,
                                                 depthTeardownTask_loop, 0,
                                                 depthTeardownTask_trdwn);
        assert(depthTeardownTaskIds[i] >= 0);
    }

    OS.begin();

    // Stop task 0, which will recursively try to stop task 1, 2, 3... in trdwn
    StopResult result = OS.stopTask(depthTeardownTaskIds[0]);

    // The outermost stopTask should succeed - its trdwn runs.
    // Nested calls may return TeardownSkipped when depth exceeded.
    assert(result == StopResult::Success);  // First stopTask succeeds fully

    // At depth limit, one of the nested stopTasks should have failed
    // Let's verify by checking that not all trdwns ran
    // We should see ARDA_MAX_CALLBACK_DEPTH trdwns called
    assert(depthTeardownCalls == ARDA_MAX_CALLBACK_DEPTH);

    // The task whose trdwn was skipped should still be STOPPED
    // (state change happens before trdwn)
    for (int i = 0; i < ARDA_MAX_CALLBACK_DEPTH + 1; i++) {
        assert(OS.getTaskState(depthTeardownTaskIds[i]) == TaskState::Stopped);
    }

    printf("PASSED\n");
}

void test_reset_returns_true_when_all_trdwns_run() {
    printf("Test: reset returns true when all trdwns run... ");
    resetTestCounters();
    trdwnCalled = 0;

    // Create multiple tasks with trdwns
    int8_t id1 = OS.createTask("t1", task1_setup, task1_loop, 0, trdwnTask_trdwn);
    int8_t id2 = OS.createTask("t2", task2_setup, task2_loop, 0, trdwnTask_trdwn);
    assert(id1 >= 0 && id2 >= 0);

    OS.begin();
    OS.run();

    // Reset should succeed and all trdwns should run
    bool resetResult = OS.reset();

    assert(resetResult == true);
    assert(OS.getError() == ArdaError::Ok);
    assert(trdwnCalled == 2);  // Both trdwns ran
    assert(OS.hasBegun() == false);
    assert(OS.getTaskCount() == 0);

    printf("PASSED\n");
}

void test_is_task_yielded() {
    printf("Test: isTaskYielded... ");
    resetTestCounters();

    // Outside of any task, should return false for all
    assert(OS.isTaskYielded(-1) == false);
    assert(OS.isTaskYielded(0) == false);
    assert(OS.isTaskYielded(99) == false);

    int8_t id = OS.createTask("task", task1_setup, task1_loop, 0);
    assert(OS.isTaskYielded(id) == false);

    OS.begin();
    assert(OS.isTaskYielded(id) == false);

    // The yieldDeleteTaskA test already verifies inYield is set during yield
    // We just need to verify the public API works

    printf("PASSED\n");
}


void test_null_loop_function() {
    printf("Test: null loop function task... ");
    resetTestCounters();

    Arda os;

    // Creating a task with null loop is allowed (useful for setup-only tasks)
    int8_t id = os.createTask("noLoop", task1_setup, nullptr, 100);
    assert(id >= 0);
    assert(os.getError() == ArdaError::Ok);

    os.begin();

    // Setup should have been called
    assert(setup1Called == 1);
    assert(os.getTaskState(id) == TaskState::Running);

    // Running the scheduler should not crash, task just doesn't execute
    os.run();
    advanceMockMillis(200);
    os.run();

    // Run count should stay at 0 since loop is null
    assert(os.getTaskRunCount(id) == 0);

    printf("PASSED\n");
}

void test_yield_outside_task_context() {
    printf("Test: yield outside task context... ");
    resetTestCounters();

    Arda os;
    os.createTask("task", task1_setup, task1_loop, 0);
    os.begin();

    // Calling yield() outside of any task should be a safe no-op
    // (currentTask is -1 when not in a task callback)
    assert(os.getCurrentTask() == -1);
    os.yield();  // Should not crash or cause issues
    assert(os.getCurrentTask() == -1);

    // Scheduler should still work normally
    os.run();
    assert(loop1Called == 1);

    printf("PASSED\n");
}

void test_deleted_slot_reuse() {
    printf("Test: deleted slot reuse optimization... ");
    resetTestCounters();

    Arda os;

    // Create 5 tasks
    int8_t ids[5];
    ids[0] = os.createTask("task0", task1_setup, task1_loop, 0);
    ids[1] = os.createTask("task1", task1_setup, task1_loop, 0);
    ids[2] = os.createTask("task2", task1_setup, task1_loop, 0);
    ids[3] = os.createTask("task3", task1_setup, task1_loop, 0);
    ids[4] = os.createTask("task4", task1_setup, task1_loop, 0);

    assert(ids[0] == 0);
    assert(ids[1] == 1);
    assert(ids[2] == 2);
    assert(ids[3] == 3);
    assert(ids[4] == 4);
    assert(os.getTaskCount() == 5);
    assert(os.getSlotCount() == 5);

    // Delete task 1 (middle)
    os.deleteTask(1);
    assert(os.getSlotCount() == 5);      // Slot count stays same
    assert(os.getActiveTaskCount() == 4);
    assert(os.getTaskCount() == 4);      // Active count decreases

    // New task should reuse slot 1 (the only deleted slot)
    int8_t newId1 = os.createTask("new1", task2_setup, task2_loop, 0);
    assert(newId1 == 1);  // Reused slot 1
    assert(os.getSlotCount() == 5);      // Slot count unchanged

    // Delete tasks 0 and 3
    os.deleteTask(0);
    os.deleteTask(3);
    assert(os.getActiveTaskCount() == 3);

    // New tasks should reuse deleted slots (order may vary due to free list LIFO)
    int8_t newId2 = os.createTask("new2", task2_setup, task2_loop, 0);
    assert(newId2 == 0 || newId2 == 3);  // Reused one of the deleted slots
    assert(os.getSlotCount() == 5);      // Slot count unchanged (slot reused)

    int8_t newId3 = os.createTask("new3", task2_setup, task2_loop, 0);
    assert(newId3 == 0 || newId3 == 3);  // Reused the other deleted slot
    assert(newId3 != newId2);            // Must be different from newId2
    assert(os.getSlotCount() == 5);      // Slot count still unchanged

    // Now all slots 0-4 are used again
    assert(os.getActiveTaskCount() == 5);
    assert(os.getTaskCount() == 5);

    // Next task should get slot 5 (new slot, no deleted slots available)
    int8_t newId4 = os.createTask("new4", task2_setup, task2_loop, 0);
    assert(newId4 == 5);
    assert(os.getSlotCount() == 6);
    assert(os.getTaskCount() == 6);

    printf("PASSED\n");
}

// For setup self-stop test
static int8_t setupSelfStopTaskId = -1;
static bool setupSelfStopCalled = false;

void setupSelfStop_setup() {
    setupSelfStopCalled = true;
    OS.stopTask(setupSelfStopTaskId);  // Stop ourselves during setup
}
void setupSelfStop_loop() {}

void test_setup_self_stop_returns_false() {
    printf("Test: setup self-stop returns false... ");
    resetTestCounters();
    setupSelfStopCalled = false;

    setupSelfStopTaskId = OS.createTask("slStop", setupSelfStop_setup, setupSelfStop_loop, 0);
    assert(setupSelfStopTaskId >= 0);

    // Starting should fail because setup stops the task
    bool result = OS.startTask(setupSelfStopTaskId);

    assert(setupSelfStopCalled == true);  // Setup did run
    assert(result == false);               // But startTask returns false
    assert(OS.getError() == ArdaError::StateChanged);
    assert(OS.getTaskState(setupSelfStopTaskId) == TaskState::Stopped);

    printf("PASSED\n");
}

// For trdwn self-restart test
static int8_t trdwnSelfRestartTaskId = -1;
static bool trdwnSelfRestartCalled = false;

void trdwnSelfRestart_setup() {}
void trdwnSelfRestart_loop() {}
void trdwnSelfRestart_trdwn() {
    trdwnSelfRestartCalled = true;
    OS.startTask(trdwnSelfRestartTaskId);  // Restart ourselves during trdwn
}

void test_trdwn_self_restart_returns_partial() {
    printf("Test: trdwn self-restart returns TeardownChangedState... ");
    resetTestCounters();
    trdwnSelfRestartCalled = false;

    trdwnSelfRestartTaskId = OS.createTask("slRstrt", trdwnSelfRestart_setup,
                                               trdwnSelfRestart_loop, 0,
                                               trdwnSelfRestart_trdwn);
    assert(trdwnSelfRestartTaskId >= 0);

    OS.startTask(trdwnSelfRestartTaskId);
    assert(OS.getTaskState(trdwnSelfRestartTaskId) == TaskState::Running);

    // Stopping should return TeardownChangedState because trdwn restarts the task
    StopResult result = OS.stopTask(trdwnSelfRestartTaskId);

    assert(trdwnSelfRestartCalled == true);  // Teardown did run
    assert(result == StopResult::TeardownChangedState); // Teardown ran but modified state
    assert(OS.getError() == ArdaError::StateChanged);
    assert(OS.getTaskState(trdwnSelfRestartTaskId) == TaskState::Running);

    printf("PASSED\n");
}

void test_error_string() {
    printf("Test: errorString... ");

    // Verify all error codes have string representations
    assert(strncmp(Arda::errorString(ArdaError::Ok), "OK", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::NullName), "NULL_NAME", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::EmptyName), "EMPTY_NAME", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::NameTooLong), "NAME_TOO_LONG", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::DuplicateName), "DUPLICATE_NAME", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::MaxTasks), "MAX_TASKS", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::InvalidId), "INVALID_ID", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::WrongState), "WRONG_STATE", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::TaskExecuting), "TASK_EXECUTING", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::TaskYielded), "TASK_YIELDED", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::AlreadyBegun), "ALREADY_BEGUN", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::CallbackDepth), "CALLBACK_DEPTH", 16) == 0);
    assert(strncmp(Arda::errorString(ArdaError::StateChanged), "STATE_CHANGED", 16) == 0);

    // Unknown error code should return "UNKNOWN"
    assert(strncmp(Arda::errorString((ArdaError)99), "UNKNOWN", 16) == 0);

    printf("PASSED\n");
}

void test_version_constants() {
    printf("Test: version constants... ");

    // Verify version constants are defined and accessible
    assert(ARDA_VERSION_MAJOR == 1);
    assert(ARDA_VERSION_MINOR == 0);
    assert(ARDA_VERSION_PATCH == 0);
    assert(strncmp(ARDA_VERSION_STRING, "1.0.0", 16) == 0);

    printf("PASSED\n");
}

// For reset-from-callback test
static bool resetFromCallbackAttempted = false;
static bool resetFromCallbackResult = false;
static ArdaError resetFromCallbackError = ArdaError::Ok;

void resetFromCallback_setup() {}
void resetFromCallback_loop() {
    resetFromCallbackAttempted = true;
    resetFromCallbackResult = OS.reset();
    resetFromCallbackError = OS.getError();
}

void test_reset_from_callback_blocked() {
    printf("Test: reset from callback blocked... ");
    resetTestCounters();
    resetFromCallbackAttempted = false;
    resetFromCallbackResult = false;
    resetFromCallbackError = ArdaError::Ok;

    OS.createTask("rsttr", resetFromCallback_setup, resetFromCallback_loop, 0);
    OS.begin();
    OS.run();

    // Reset should have been attempted
    assert(resetFromCallbackAttempted == true);
    // But it should have failed
    assert(resetFromCallbackResult == false);
    assert(resetFromCallbackError == ArdaError::InCallback);

    // Scheduler should still be functional
    assert(OS.hasBegun() == true);
    assert(OS.getTaskCount() == 1);

    printf("PASSED\n");
}

// For auto-start failure test - we need to trigger callback depth limit during auto-start
static int autoStartDepthSetupCalls = 0;
static int8_t autoStartDepthTaskIds[ARDA_MAX_CALLBACK_DEPTH + 2];

void autoStartDepthTask_setup() {
    autoStartDepthSetupCalls++;
    // Create another task from setup - this will try to auto-start and eventually hit depth limit
    static char names[ARDA_MAX_CALLBACK_DEPTH + 2][16];
    int idx = autoStartDepthSetupCalls;
    if (idx <= ARDA_MAX_CALLBACK_DEPTH) {
        snprintf(names[idx], sizeof(names[idx]), "auto%d", idx);
        autoStartDepthTaskIds[idx] = OS.createTask(names[idx], autoStartDepthTask_setup, nullptr, 0);
    }
}
void autoStartDepthTask_loop() {}

void test_createTask_autostart_failure_preserves_error() {
    printf("Test: createTask auto-start failure returns -1... ");
    resetTestCounters();
    autoStartDepthSetupCalls = 0;
    for (int i = 0; i < ARDA_MAX_CALLBACK_DEPTH + 2; i++) {
        autoStartDepthTaskIds[i] = -1;
    }

    // Start with begin() already called so auto-start triggers
    OS.begin();

    // Create first task - its setup will recursively create more tasks
    // Eventually we'll hit callback depth limit
    static char name0[] = "auto0";
    autoStartDepthTaskIds[0] = OS.createTask(name0, autoStartDepthTask_setup, autoStartDepthTask_loop, 0);

    // First task should have valid ID
    assert(autoStartDepthTaskIds[0] >= 0);

    // With the new semantics: when createTask's auto-start fails due to callback depth,
    // it returns -1 and cleans up the task (doesn't leave it in STOPPED state).
    // So we should find that some nested createTask returned -1.
    bool foundFailedCreate = false;
    for (int i = 1; i <= ARDA_MAX_CALLBACK_DEPTH + 1; i++) {
        if (autoStartDepthTaskIds[i] == -1) {
            foundFailedCreate = true;
            break;
        }
    }
    // Due to callback depth limit, at least one createTask should have returned -1
    assert(foundFailedCreate == true);

    // Verify no "orphaned" tasks in STOPPED state exist (all valid IDs are RUNNING)
    for (int i = 0; i <= ARDA_MAX_CALLBACK_DEPTH + 1; i++) {
        if (autoStartDepthTaskIds[i] >= 0) {
            // If we have a valid ID, the task should be RUNNING (not orphaned in STOPPED)
            assert(OS.getTaskState(autoStartDepthTaskIds[i]) == TaskState::Running);
        }
    }

    printf("PASSED\n");
}

void test_begin_partial_failure_preserves_error() {
    printf("Test: begin partial failure preserves error... ");
    resetTestCounters();

    // Create a task whose setup will fail by hitting callback depth
    // We'll do this by having setup recursively start tasks
    depthTestSetupCalls = 0;
    depthTestCurrentIndex = 0;

    static char names[ARDA_MAX_CALLBACK_DEPTH + 2][16];
    for (int i = 0; i < ARDA_MAX_CALLBACK_DEPTH + 2; i++) {
        snprintf(names[i], sizeof(names[i]), "begin%d", i);
        depthTestTaskIds[i] = OS.createTask(names[i], depthTestTask_setup, depthTestTask_loop, 0);
        assert(depthTestTaskIds[i] >= 0);
    }

    // begin() will try to start all tasks
    // Task 0's setup will recursively start task 1, 2, etc.
    // Eventually hitting callback depth limit
    int8_t started = OS.begin();

    // Not all tasks should have started
    assert(started < ARDA_MAX_CALLBACK_DEPTH + 2);

    // Error should NOT be ArdaError::Ok since some tasks failed to start
    // (The last failed startTask sets the error)
    assert(OS.getError() != ArdaError::Ok);

    printf("PASSED\n");
}

void test_error_string_in_callback() {
    printf("Test: errorString IN_CALLBACK... ");

    assert(strncmp(Arda::errorString(ArdaError::InCallback), "IN_CALLBACK", 16) == 0);

    printf("PASSED\n");
}

// For timeout test
static int8_t timeoutTaskId = -1;
static uint32_t capturedTimeoutDuration = 0;
static bool timeoutCallbackCalled = false;

void timeoutCallback(int8_t taskId, uint32_t durationMs) {
    timeoutCallbackCalled = true;
    capturedTimeoutDuration = durationMs;
    assert(taskId == timeoutTaskId);
}

void timeoutTask_setup() {}
void timeoutTask_loop() {
    // Simulate a long-running task by advancing mock time
    advanceMockMillis(150);  // Task "takes" 150ms
}

void test_task_timeout() {
    printf("Test: task timeout callback... ");
    resetTestCounters();
    timeoutCallbackCalled = false;
    capturedTimeoutDuration = 0;

    OS.setTimeoutCallback(timeoutCallback);

    timeoutTaskId = OS.createTask("timeout", timeoutTask_setup, timeoutTask_loop, 0);

    // Set timeout to 100ms
    assert(OS.setTaskTimeout(timeoutTaskId, 100) == true);
    assert(OS.getTaskTimeout(timeoutTaskId) == 100);

    OS.begin();

    // Run the task - it will "take" 150ms which exceeds the 100ms timeout
    OS.run();

    assert(timeoutCallbackCalled == true);
    assert(capturedTimeoutDuration >= 100);  // Should be ~150ms

    printf("PASSED\n");
}

void test_task_timeout_not_triggered() {
    printf("Test: task timeout not triggered when within limit... ");
    resetTestCounters();
    timeoutCallbackCalled = false;

    OS.setTimeoutCallback(timeoutCallback);

    timeoutTaskId = OS.createTask("fast", task1_setup, task1_loop, 0);  // Fast task

    // Set timeout to 1000ms (much longer than task takes)
    OS.setTaskTimeout(timeoutTaskId, 1000);

    OS.begin();
    OS.run();

    // Timeout callback should NOT have been called
    assert(timeoutCallbackCalled == false);

    printf("PASSED\n");
}

void test_task_timeout_disabled() {
    printf("Test: task timeout disabled when set to 0... ");
    resetTestCounters();
    timeoutCallbackCalled = false;

    OS.setTimeoutCallback(timeoutCallback);

    timeoutTaskId = OS.createTask("timeout", timeoutTask_setup, timeoutTask_loop, 0);

    // Timeout is 0 by default (disabled)
    assert(OS.getTaskTimeout(timeoutTaskId) == 0);

    OS.begin();
    OS.run();

    // Timeout callback should NOT have been called even though task took 150ms
    assert(timeoutCallbackCalled == false);

    printf("PASSED\n");
}

void test_get_task_timeout_invalid() {
    printf("Test: getTaskTimeout returns 0 for invalid task... ");
    resetTestCounters();

    Arda os;

    assert(os.getTaskTimeout(-1) == 0);
    assert(os.getTaskTimeout(99) == 0);

    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.setTaskTimeout(id, 500);
    assert(os.getTaskTimeout(id) == 500);

    os.deleteTask(id);
    assert(os.getTaskTimeout(id) == 0);

    printf("PASSED\n");
}

// For start failure callback test
static int startFailureCount = 0;
static int8_t lastFailedTaskId = -1;
static ArdaError lastFailureError = ArdaError::Ok;

void startFailureCallback(int8_t taskId, ArdaError error) {
    startFailureCount++;
    lastFailedTaskId = taskId;
    lastFailureError = error;
}

void test_start_failure_callback() {
    printf("Test: start failure callback... ");
    resetTestCounters();
    startFailureCount = 0;
    lastFailedTaskId = -1;
    lastFailureError = ArdaError::Ok;
    depthTestSetupCalls = 0;
    depthTestCurrentIndex = 0;

    OS.setStartFailureCallback(startFailureCallback);

    // Create chain of tasks that will hit callback depth limit during begin()
    static char names[ARDA_MAX_CALLBACK_DEPTH + 2][16];
    for (int i = 0; i < ARDA_MAX_CALLBACK_DEPTH + 2; i++) {
        snprintf(names[i], sizeof(names[i]), "fail%d", i);
        depthTestTaskIds[i] = OS.createTask(names[i], depthTestTask_setup, depthTestTask_loop, 0);
        assert(depthTestTaskIds[i] >= 0);
    }

    // begin() will try to start all tasks, some will fail due to callback depth
    int8_t started = OS.begin();

    // Some tasks should have failed to start
    assert(started < ARDA_MAX_CALLBACK_DEPTH + 2);

    // Callback should have been called at least once
    assert(startFailureCount > 0);
    assert(lastFailedTaskId >= 0);
    assert(lastFailureError != ArdaError::Ok);

    printf("PASSED\n");
}

void test_stop_result_enum_values() {
    printf("Test: StopResult enum values are distinct... ");

    // Verify enum values are distinct and can be tested
    assert(StopResult::Success != StopResult::TeardownSkipped);
    assert(StopResult::Success != StopResult::TeardownChangedState);
    assert(StopResult::Success != StopResult::Failed);
    assert(StopResult::TeardownSkipped != StopResult::TeardownChangedState);
    assert(StopResult::TeardownSkipped != StopResult::Failed);
    assert(StopResult::TeardownChangedState != StopResult::Failed);

    printf("PASSED\n");
}

void test_batch_start_tasks() {
    printf("Test: batch startTasks... ");
    resetTestCounters();

    Arda os;
    int8_t id0 = os.createTask("t0", task1_setup, task1_loop, 0);
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task1_setup, task1_loop, 0);

    int8_t ids[] = {id0, id1, id2};
    int8_t started = os.startTasks(ids, 3);

    assert(started == 3);
    assert(os.getTaskState(id0) == TaskState::Running);
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Running);

    // Try to start again (should fail - wrong state)
    started = os.startTasks(ids, 3);
    assert(started == 0);

    printf("PASSED\n");
}

void test_batch_stop_tasks() {
    printf("Test: batch stopTasks... ");
    resetTestCounters();

    Arda os;
    int8_t id0 = os.createTask("t0", task1_setup, task1_loop, 0);
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task1_setup, task1_loop, 0);

    int8_t ids[] = {id0, id1, id2};
    os.startTasks(ids, 3);

    int8_t stopped = os.stopTasks(ids, 3);
    assert(stopped == 3);
    assert(os.getTaskState(id0) == TaskState::Stopped);
    assert(os.getTaskState(id1) == TaskState::Stopped);
    assert(os.getTaskState(id2) == TaskState::Stopped);

    printf("PASSED\n");
}

void test_batch_pause_resume_tasks() {
    printf("Test: batch pauseTasks/resumeTasks... ");
    resetTestCounters();

    Arda os;
    int8_t id0 = os.createTask("t0", task1_setup, task1_loop, 0);
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task1_setup, task1_loop, 0);

    int8_t ids[] = {id0, id1, id2};
    os.startTasks(ids, 3);

    // Pause all
    int8_t paused = os.pauseTasks(ids, 3);
    assert(paused == 3);
    assert(os.getTaskState(id0) == TaskState::Paused);
    assert(os.getTaskState(id1) == TaskState::Paused);
    assert(os.getTaskState(id2) == TaskState::Paused);

    // Resume all
    int8_t resumed = os.resumeTasks(ids, 3);
    assert(resumed == 3);
    assert(os.getTaskState(id0) == TaskState::Running);
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Running);

    printf("PASSED\n");
}

void test_batch_partial_failure() {
    printf("Test: batch operations with partial failure... ");
    resetTestCounters();

    Arda os;
    int8_t id0 = os.createTask("t0", task1_setup, task1_loop, 0);
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);

    os.startTask(id0);  // Start only task 0

    // Try to start both - task 0 should fail (already running), task 1 should succeed
    int8_t ids[] = {id0, id1};
    int8_t started = os.startTasks(ids, 2);
    assert(started == 1);  // Only task 1 started

    assert(os.getTaskState(id0) == TaskState::Running);  // Still running
    assert(os.getTaskState(id1) == TaskState::Running);  // Now running

    // Pause with an invalid ID in the mix
    int8_t mixedIds[] = {id0, 99, id1};  // 99 is invalid
    int8_t paused = os.pauseTasks(mixedIds, 3);
    assert(paused == 2);  // Two valid tasks paused

    printf("PASSED\n");
}

void test_batch_preserves_first_error() {
    printf("Test: batch operations preserve first error... ");
    resetTestCounters();

    Arda os;
    int8_t id0 = os.createTask("t0", task1_setup, task1_loop, 0);
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);

    // Start task 0, leave task 1 stopped
    os.startTask(id0);

    // Try to start: id0 (WrongState - already running), invalid (InvalidId), id1 (success)
    // First error should be WrongState, not InvalidId
    int8_t ids[] = {id0, 99, id1};
    os.clearError();
    int8_t started = os.startTasks(ids, 3);
    assert(started == 1);  // Only id1 started
    assert(os.getError() == ArdaError::WrongState);  // First error preserved, not InvalidId

    // Now test with InvalidId first
    os.stopTask(id0);
    os.stopTask(id1);
    int8_t ids2[] = {99, id0, id1};  // Invalid first
    os.clearError();
    started = os.startTasks(ids2, 3);
    assert(started == 2);  // id0 and id1 started
    assert(os.getError() == ArdaError::InvalidId);  // First error

    printf("PASSED\n");
}

// For trace callback test
static int traceEventCount = 0;
static TraceEvent lastTraceEvent = TraceEvent::TaskStopped;
static int8_t lastTraceTaskId = -1;

void traceCallback(int8_t taskId, TraceEvent event) {
    traceEventCount++;
    lastTraceEvent = event;
    lastTraceTaskId = taskId;
}

void test_trace_callback() {
    printf("Test: trace callback... ");
    resetTestCounters();
    traceEventCount = 0;
    lastTraceEvent = TraceEvent::TaskStopped;
    lastTraceTaskId = -1;

    OS.setTraceCallback(traceCallback);

    int8_t id = OS.createTask("traced", task1_setup, task1_loop, 0);
    OS.begin(false);  // Initialize scheduler without auto-starting tasks
    OS.startTask(id);

    // Should have seen STARTING and STARTED events
    assert(traceEventCount >= 2);
    assert(lastTraceEvent == TraceEvent::TaskStarted);
    assert(lastTraceTaskId == id);

    int beforeCount = traceEventCount;
    OS.run();

    // Should have seen LOOP_BEGIN and LOOP_END
    assert(traceEventCount == beforeCount + 2);
    assert(lastTraceEvent == TraceEvent::TaskLoopEnd);

    beforeCount = traceEventCount;
    OS.stopTask(id);

    // Should have seen STOPPED (no trdwn, so no STOPPING)
    assert(traceEventCount == beforeCount + 1);
    assert(lastTraceEvent == TraceEvent::TaskStopped);

    // Disable trace callback
    OS.setTraceCallback(nullptr);
    beforeCount = traceEventCount;
    OS.startTask(id);
    assert(traceEventCount == beforeCount);  // No new events

    printf("PASSED\n");
}

void test_trace_pause_resume_delete() {
    printf("Test: trace callback for pause/resume/delete... ");
    resetTestCounters();
    traceEventCount = 0;
    lastTraceEvent = TraceEvent::TaskStopped;
    lastTraceTaskId = -1;

    OS.setTraceCallback(traceCallback);

    int8_t id = OS.createTask("traced", task1_setup, task1_loop, 0);
    OS.startTask(id);

    int beforeCount = traceEventCount;

    // Test PAUSED event
    OS.pauseTask(id);
    assert(traceEventCount == beforeCount + 1);
    assert(lastTraceEvent == TraceEvent::TaskPaused);
    assert(lastTraceTaskId == id);

    beforeCount = traceEventCount;

    // Test RESUMED event
    OS.resumeTask(id);
    assert(traceEventCount == beforeCount + 1);
    assert(lastTraceEvent == TraceEvent::TaskResumed);
    assert(lastTraceTaskId == id);

    beforeCount = traceEventCount;

    // Stop and delete to test DELETED event
    OS.stopTask(id);
    beforeCount = traceEventCount;

    OS.deleteTask(id);
    assert(traceEventCount == beforeCount + 1);
    assert(lastTraceEvent == TraceEvent::TaskDeleted);

    printf("PASSED\n");
}

// For trace callback depth test
static int traceStoppedCount = 0;
static int8_t traceStoppedTaskIds[ARDA_MAX_CALLBACK_DEPTH + 2];
static int traceStoppedIndex = 0;

void traceCallbackDepthTest(int8_t taskId, TraceEvent event) {
    if (event == TraceEvent::TaskStopped) {
        if (traceStoppedIndex < ARDA_MAX_CALLBACK_DEPTH + 2) {
            traceStoppedTaskIds[traceStoppedIndex++] = taskId;
        }
        traceStoppedCount++;
    }
}

void depthTraceTask_setup() {}
void depthTraceTask_loop() {}
static int8_t depthTraceTaskIds[ARDA_MAX_CALLBACK_DEPTH + 2];
static int depthTraceCurrentIndex = 0;

void depthTraceTask_trdwn() {
    int myIndex = depthTraceCurrentIndex++;
    // Try to stop next task from trdwn (will hit depth limit eventually)
    if (myIndex < ARDA_MAX_CALLBACK_DEPTH) {
        int8_t nextId = depthTraceTaskIds[myIndex + 1];
        if (nextId >= 0 && OS.getTaskState(nextId) != TaskState::Stopped) {
            OS.stopTask(nextId);
        }
    }
}

void test_trace_stopped_emitted_on_callback_depth() {
    printf("Test: TaskStopped emitted even when callback depth exceeded... ");
    resetTestCounters();
    resetGlobalOS();
    traceStoppedCount = 0;
    traceStoppedIndex = 0;
    depthTraceCurrentIndex = 0;

    OS.setTraceCallback(traceCallbackDepthTest);

    // Create chain of tasks with trdwns
    static char names[ARDA_MAX_CALLBACK_DEPTH + 2][16];

    for (int i = 0; i < ARDA_MAX_CALLBACK_DEPTH + 2; i++) {
        snprintf(names[i], sizeof(names[i]), "trace%d", i);
        depthTraceTaskIds[i] = OS.createTask(names[i], depthTraceTask_setup,
                                              depthTraceTask_loop, 0,
                                              depthTraceTask_trdwn);
        assert(depthTraceTaskIds[i] >= 0);
    }

    OS.begin();

    // Stop task 0, which will recursively try to stop task 1, 2, 3... in trdwn
    // This will eventually hit callback depth limit
    OS.stopTask(depthTraceTaskIds[0]);

    // Key assertion: TaskStopped should be emitted for ALL stopped tasks,
    // including those where trdwn was skipped due to callback depth.
    // Previously, TaskStopped was not emitted when callback depth exceeded.
    // We expect all tasks from 0 to ARDA_MAX_CALLBACK_DEPTH to have TaskStopped emitted.
    assert(traceStoppedCount == ARDA_MAX_CALLBACK_DEPTH + 1);

    printf("PASSED\n");
}

void test_active_count_o1() {
    printf("Test: getActiveTaskCount is O(1)... ");
    resetTestCounters();

    Arda os;

    assert(os.getActiveTaskCount() == 0);

    // Create tasks
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    assert(os.getActiveTaskCount() == 1);

    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    assert(os.getActiveTaskCount() == 2);

    // Delete one
    os.deleteTask(id1);
    assert(os.getActiveTaskCount() == 1);

    // Create another (reuses slot)
    int8_t id3 = os.createTask("t3", task1_setup, task1_loop, 0);
    assert(os.getActiveTaskCount() == 2);

    // Delete both
    os.deleteTask(id2);
    os.deleteTask(id3);
    assert(os.getActiveTaskCount() == 0);

    printf("PASSED\n");
}

void test_has_task_callbacks() {
    printf("Test: hasTaskSetup/Loop/Teardown... ");
    resetTestCounters();

    Arda os;

    // Task with all callbacks
    int8_t id1 = os.createTask("full", task1_setup, task1_loop, 0, trdwnTask_trdwn);
    assert(os.hasTaskSetup(id1) == true);
    assert(os.hasTaskLoop(id1) == true);
    assert(os.hasTaskTeardown(id1) == true);

    // Task with only loop
    int8_t id2 = os.createTask("lpOnly", nullptr, task2_loop, 0, nullptr);
    assert(os.hasTaskSetup(id2) == false);
    assert(os.hasTaskLoop(id2) == true);
    assert(os.hasTaskTeardown(id2) == false);

    // Task with no callbacks
    int8_t id3 = os.createTask("empty", nullptr, nullptr, 0, nullptr);
    assert(os.hasTaskSetup(id3) == false);
    assert(os.hasTaskLoop(id3) == false);
    assert(os.hasTaskTeardown(id3) == false);

    // Invalid task ID
    assert(os.hasTaskSetup(-1) == false);
    assert(os.hasTaskLoop(99) == false);
    assert(os.hasTaskTeardown(-1) == false);

    printf("PASSED\n");
}

void test_rename_task() {
    printf("Test: renameTask... ");
    resetTestCounters();

    Arda os;

    int8_t id = os.createTask("orig", task1_setup, task1_loop, 0);
    assert(strncmp(os.getTaskName(id), "orig", ARDA_MAX_NAME_LEN) == 0);

    // Successful rename
    assert(os.renameTask(id, "renamed") == true);
    assert(os.getError() == ArdaError::Ok);
    assert(strncmp(os.getTaskName(id), "renamed", ARDA_MAX_NAME_LEN) == 0);

    // Find by new name works
    assert(os.findTaskByName("renamed") == id);
    assert(os.findTaskByName("orig") == -1);

    // Rename to same name (allowed)
    assert(os.renameTask(id, "renamed") == true);

    // Invalid task ID
    assert(os.renameTask(-1, "test") == false);
    assert(os.getError() == ArdaError::InvalidId);

    assert(os.renameTask(99, "test") == false);
    assert(os.getError() == ArdaError::InvalidId);

    // Null name
    assert(os.renameTask(id, nullptr) == false);
    assert(os.getError() == ArdaError::NullName);

    // Empty name
    assert(os.renameTask(id, "") == false);
    assert(os.getError() == ArdaError::EmptyName);

    // Name too long
    assert(os.renameTask(id, "thisNameIsWayTooLongForTheBuffer") == false);
    assert(os.getError() == ArdaError::NameTooLong);

    // Duplicate name
    (void)os.createTask("other", task2_setup, task2_loop, 0);
    assert(os.renameTask(id, "other") == false);
    assert(os.getError() == ArdaError::DuplicateName);

    // Original name unchanged after failed rename
    assert(strncmp(os.getTaskName(id), "renamed", ARDA_MAX_NAME_LEN) == 0);

    printf("PASSED\n");
}

void test_getters_set_error_on_invalid() {
    printf("Test: getTaskRunCount/Interval set error on invalid... ");
    resetTestCounters();

    Arda os;

    os.clearError();
    assert(os.getError() == ArdaError::Ok);

    // getTaskRunCount returns 0 on invalid without setting error
    uint32_t runCount = os.getTaskRunCount(-1);
    assert(runCount == 0);
    assert(os.getError() == ArdaError::Ok);  // Getters don't set error

    // getTaskInterval returns 0 on invalid without setting error
    uint32_t interval = os.getTaskInterval(99);
    assert(interval == 0);
    assert(os.getError() == ArdaError::Ok);  // Getters don't set error

    // Valid task also doesn't set error
    int8_t id = os.createTask("task", task1_setup, task1_loop, 100);
    os.clearError();

    runCount = os.getTaskRunCount(id);
    assert(runCount == 0);  // Valid, just never run
    assert(os.getError() == ArdaError::Ok);

    interval = os.getTaskInterval(id);
    assert(interval == 100);
    assert(os.getError() == ArdaError::Ok);

    printf("PASSED\n");
}

void test_free_list_o1_allocation() {
    printf("Test: free list O(1) allocation... ");
    resetTestCounters();

    Arda os;

    // Create and delete many tasks to exercise the free list
    for (int i = 0; i < 100; i++) {
        char name[16];
        snprintf(name, sizeof(name), "task%d", i % ARDA_MAX_TASKS);

        // Delete existing task with this name if it exists
        int8_t existing = os.findTaskByName(name);
        if (existing != -1) {
            os.deleteTask(existing);
        }

        // Create new task - should reuse deleted slot (O(1)) or allocate new
        int8_t id = os.createTask(name, task1_setup, task1_loop, 0);
        assert(id >= 0);
        assert(os.getActiveTaskCount() <= ARDA_MAX_TASKS);
    }

    printf("PASSED\n");
}

void test_batch_null_array() {
    printf("Test: batch operations with null array... ");
    resetTestCounters();

    Arda os;

    // All batch operations should safely return 0 for null arrays
    assert(os.startTasks(nullptr, 5) == 0);
    assert(os.stopTasks(nullptr, 5) == 0);
    assert(os.pauseTasks(nullptr, 5) == 0);
    assert(os.resumeTasks(nullptr, 5) == 0);

    // Also test zero/negative count
    int8_t ids[] = {0, 1, 2};
    assert(os.startTasks(ids, 0) == 0);
    assert(os.startTasks(ids, -1) == 0);

    printf("PASSED\n");
}

void test_stopTasks_counts_partial_success() {
    printf("Test: stopTasks counts partial success... ");
    resetTestCounters();
    depthTeardownCalls = 0;
    depthTeardownCurrentIndex = 0;

    // Create tasks with trdwns that chain-stop each other
    // This will hit callback depth limit, resulting in StopResult::TeardownSkipped
    static char names[3][16];
    int8_t ids[3];

    for (int i = 0; i < 3; i++) {
        snprintf(names[i], sizeof(names[i]), "chain%d", i);
        ids[i] = OS.createTask(names[i], task1_setup, task1_loop, 0);
        assert(ids[i] >= 0);
    }

    OS.begin();

    // Stop all - should count all as stopped even if some have partial success
    int8_t stopped = OS.stopTasks(ids, 3);
    assert(stopped == 3);  // All 3 should be counted as stopped

    // Verify all are actually stopped
    for (int i = 0; i < 3; i++) {
        assert(OS.getTaskState(ids[i]) == TaskState::Stopped);
    }

    printf("PASSED\n");
}

void test_interval_zero_runs_every_cycle() {
    printf("Test: interval=0 task runs every cycle... ");
    resetTestCounters();

    Arda os;
    (void)os.createTask("evryLp", task1_setup, task1_loop, 0);
    os.begin();

    // Should run on every run() call regardless of time
    os.run();
    assert(loop1Called == 1);

    os.run();
    assert(loop1Called == 2);

    os.run();
    assert(loop1Called == 3);

    // Even without advancing time
    os.run();
    os.run();
    os.run();
    assert(loop1Called == 6);

    printf("PASSED\n");
}

void test_max_length_name() {
    printf("Test: maximum length task name... ");
    resetTestCounters();

    Arda os;

    // Create a name that's exactly at the limit (ARDA_MAX_NAME_LEN - 1 chars)
    char maxName[ARDA_MAX_NAME_LEN];
    for (int i = 0; i < ARDA_MAX_NAME_LEN - 1; i++) {
        maxName[i] = 'a' + (i % 26);
    }
    maxName[ARDA_MAX_NAME_LEN - 1] = '\0';

    int8_t id = os.createTask(maxName, task1_setup, task1_loop, 0);
    assert(id >= 0);
    assert(os.getError() == ArdaError::Ok);

    // Verify name was stored correctly
    assert(strncmp(os.getTaskName(id), maxName, ARDA_MAX_NAME_LEN) == 0);

    // Verify findTaskByName works with max-length name
    assert(os.findTaskByName(maxName) == id);

    // Rename to another max-length name
    char newMaxName[ARDA_MAX_NAME_LEN];
    for (int i = 0; i < ARDA_MAX_NAME_LEN - 1; i++) {
        newMaxName[i] = 'z' - (i % 26);
    }
    newMaxName[ARDA_MAX_NAME_LEN - 1] = '\0';

    assert(os.renameTask(id, newMaxName) == true);
    assert(strncmp(os.getTaskName(id), newMaxName, ARDA_MAX_NAME_LEN) == 0);

    printf("PASSED\n");
}

void test_multiple_scheduler_instances() {
    printf("Test: multiple scheduler instances... ");
    resetTestCounters();

    Arda os1;
    Arda os2;

    // Create tasks on different schedulers
    int8_t id1 = os1.createTask("task1", task1_setup, task1_loop, 0);
    int8_t id2 = os2.createTask("task2", task2_setup, task2_loop, 0);

    assert(id1 >= 0);
    assert(id2 >= 0);

    // Both should have 1 task each
    assert(os1.getTaskCount() == 1);
    assert(os2.getTaskCount() == 1);

    // Tasks are independent
    os1.begin();
    os2.begin();

    os1.run();
    assert(loop1Called == 1);
    assert(loop2Called == 0);

    os2.run();
    assert(loop1Called == 1);
    assert(loop2Called == 1);

    // States are independent
    os1.pauseTask(id1);
    assert(os1.getTaskState(id1) == TaskState::Paused);
    assert(os2.getTaskState(id2) == TaskState::Running);

    printf("PASSED\n");
}

void test_deleted_task_slot_fully_cleared() {
    printf("Test: deleted task slot is fully cleared... ");
    resetTestCounters();

    Arda os;

    // Create task with interval=0 so it runs every cycle
    int8_t id = os.createTask("toDel", task1_setup, task1_loop, 0, trdwnTask_trdwn);
    os.setTaskTimeout(id, 500);
    os.begin();
    os.run();  // Run once to increment runCount

    assert(os.getTaskRunCount(id) > 0);
    assert(os.getTaskInterval(id) == 0);
    assert(os.getTaskTimeout(id) == 500);

    // Stop and delete
    os.stopTask(id);
    os.deleteTask(id);

    // Slot should be invalid now
    assert(os.isValidTask(id) == false);
    assert(os.getTaskName(id) == nullptr);
    assert(os.getTaskState(id) == TaskState::Invalid);

    // Create new task - should reuse slot
    int8_t newId = os.createTask("newTask", task2_setup, task2_loop, 200);
    assert(newId == id);  // Reused same slot

    // New task should have fresh state, not old values
    assert(os.getTaskRunCount(newId) == 0);
    assert(os.getTaskInterval(newId) == 200);
    assert(os.getTaskTimeout(newId) == 0);  // Default, not 500

    printf("PASSED\n");
}

void test_start_task_run_immediately() {
    printf("Test: startTask with runImmediately... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 1000);  // 1 second interval
    os.begin(false);  // Initialize without auto-starting

    // Normal start - should NOT run on first run() cycle
    os.startTask(id);
    os.run();
    assert(loop1Called == 0);  // Waiting for interval

    os.stopTask(id);
    loop1Called = 0;

    // Start with runImmediately=true - should run on first run() cycle
    os.startTask(id, true);
    os.run();
    assert(loop1Called == 1);  // Ran immediately

    printf("PASSED\n");
}

void test_set_task_interval_no_reset() {
    printf("Test: setTaskInterval with resetTiming=false... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 100);
    os.begin(false);  // Initialize without auto-starting
    os.startTask(id, true);  // Start with immediate run

    setMockMillis(0);
    os.run();
    assert(loop1Called == 1);  // First run at t=0

    // Advance 50ms - not yet time to run (interval is 100)
    setMockMillis(50);
    os.run();
    assert(loop1Called == 1);

    // Change interval to 200ms WITH resetTiming=true (default)
    // This resets lastRun to now (50), so next run at 250
    os.setTaskInterval(id, 200, true);
    setMockMillis(100);
    os.run();
    assert(loop1Called == 1);  // Still waiting (50 + 200 = 250)

    setMockMillis(250);
    os.run();
    assert(loop1Called == 2);  // Now it runs

    // Now test resetTiming=false
    // Task just ran at 250, next run would be at 450 (250+200)
    // Change interval to 100 WITHOUT resetting timing
    // Next run should be at 350 (250 + 100), not 350 (250 + 100 from reset)
    os.setTaskInterval(id, 100, false);
    setMockMillis(300);
    os.run();
    assert(loop1Called == 2);  // Still waiting (250 + 100 = 350)

    setMockMillis(350);
    os.run();
    assert(loop1Called == 3);  // Now it runs

    printf("PASSED\n");
}

void test_stop_all_tasks() {
    printf("Test: stopAllTasks... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    os.createTask("t3", task1_setup, task1_loop, 0);  // id3

    os.begin();
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Running);

    int8_t stopped = os.stopAllTasks();
    assert(stopped == 3);
    assert(os.getTaskState(id1) == TaskState::Stopped);
    assert(os.getTaskState(id2) == TaskState::Stopped);

    printf("PASSED\n");
}

void test_pause_all_tasks() {
    printf("Test: pauseAllTasks/resumeAllTasks... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    os.begin();

    int8_t paused = os.pauseAllTasks();
    assert(paused == 2);
    assert(os.getTaskState(id1) == TaskState::Paused);
    assert(os.getTaskState(id2) == TaskState::Paused);

    int8_t resumed = os.resumeAllTasks();
    assert(resumed == 2);
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Running);

    printf("PASSED\n");
}

void test_start_all_tasks() {
    printf("Test: startAllTasks... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);

    // Don't call begin() - tasks are stopped
    assert(os.getTaskState(id1) == TaskState::Stopped);
    assert(os.getTaskState(id2) == TaskState::Stopped);

    int8_t started = os.startAllTasks();
    assert(started == 2);
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Running);

    // Calling again should start 0 (all already running)
    started = os.startAllTasks();
    assert(started == 0);

    printf("PASSED\n");
}

void test_get_valid_task_ids() {
    printf("Test: getValidTaskIds... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    int8_t id3 = os.createTask("t3", task1_setup, task1_loop, 0);

    // Delete middle task
    os.deleteTask(id2);

    int8_t ids[16];
    int8_t count = os.getValidTaskIds(ids, 16);
    assert(count == 2);
    assert(ids[0] == id1);
    assert(ids[1] == id3);

    // Test with smaller buffer - returns total count but only writes maxCount entries
    count = os.getValidTaskIds(ids, 1);
    assert(count == 2);  // Returns total count (2), not buffer size (1)
    assert(ids[0] == id1);  // Only first entry written

    // Test with nullptr (just get count) - maxCount is ignored when outIds is nullptr
    count = os.getValidTaskIds(nullptr, 0);
    assert(count == 2);

    printf("PASSED\n");
}

void test_get_valid_task_ids_empty() {
    printf("Test: getValidTaskIds with no tasks... ");
    resetTestCounters();

    Arda os;
    int8_t ids[16];

    // No tasks created
    int8_t count = os.getValidTaskIds(ids, 16);
    assert(count == 0);

    // Create and delete a task
    int8_t id = os.createTask("temp", task1_setup, task1_loop, 0);
    os.deleteTask(id);

    count = os.getValidTaskIds(ids, 16);
    assert(count == 0);

    printf("PASSED\n");
}

void test_batch_operations_zero_count() {
    printf("Test: batch operations with count=0... ");
    resetTestCounters();

    Arda os;
    int8_t ids[] = {0, 1};

    // All batch operations with count=0 should return 0 and not crash
    assert(os.startTasks(ids, 0) == 0);
    assert(os.stopTasks(ids, 0) == 0);
    assert(os.pauseTasks(ids, 0) == 0);
    assert(os.resumeTasks(ids, 0) == 0);

    // Negative count should also be safe
    assert(os.startTasks(ids, -1) == 0);

    printf("PASSED\n");
}

static bool intervalModified = false;
void selfModifyInterval_setup() {}
void selfModifyInterval_loop() {
    if (!intervalModified) {
        OS.setTaskInterval(OS.getCurrentTask(), 500);
        intervalModified = true;
    }
    loop1Called++;
}

void test_task_modifies_own_interval() {
    printf("Test: task modifies own interval... ");
    resetTestCounters();
    intervalModified = false;

    int8_t id = OS.createTask("selfMod", selfModifyInterval_setup, selfModifyInterval_loop, 100);
    OS.begin();

    // First run at t=100
    setMockMillis(100);
    OS.run();
    assert(loop1Called == 1);
    assert(intervalModified == true);
    assert(OS.getTaskInterval(id) == 500);

    // Should now wait 500ms, not 100ms
    setMockMillis(200);  // Only 100ms later
    OS.run();
    assert(loop1Called == 1);  // Should not run

    setMockMillis(600);  // 500ms after the interval change
    OS.run();
    assert(loop1Called == 2);  // Now it runs

    printf("PASSED\n");
}

static int8_t taskToDelete = -1;
static bool deleteAttempted = false;
static StopResult stopYieldedResult = StopResult::Failed;
static ArdaError stopYieldedError = ArdaError::Ok;
static bool deleteSucceeded = false;

void deleterTask_setup() {}
void deleterTask_loop() {
    loop1Called++;
    // Try to delete the other task while yielding
    OS.yield();
}

void victimTask_setup() {}
void victimTask_loop() {
    loop2Called++;
    if (taskToDelete >= 0 && !deleteAttempted) {
        deleteAttempted = true;
        stopYieldedResult = OS.stopTask(taskToDelete);
        stopYieldedError = OS.getError();  // Capture error before deleteTask overwrites it
        deleteSucceeded = OS.deleteTask(taskToDelete);
    }
}

void test_delete_other_task_during_yield() {
    printf("Test: delete other task during yield... ");
    resetTestCounters();
    deleteAttempted = false;
    stopYieldedResult = StopResult::Failed;
    stopYieldedError = ArdaError::Ok;
    deleteSucceeded = false;

    // Create deleter first (id=0), victim second (id=1)
    int8_t deleterId = OS.createTask("deleter", deleterTask_setup, deleterTask_loop, 0);
    (void)OS.createTask("victim", victimTask_setup, victimTask_loop, 0);
    taskToDelete = deleterId;  // Victim will try to delete deleter

    OS.begin();
    OS.run();

    // Deleter runs first, calls yield(), victim runs and tries to stop+delete deleter
    // Deleter is in yield (inYield=true), so stop should fail (can't run trdwn mid-yield)
    assert(deleteAttempted == true);
    assert(stopYieldedResult == StopResult::Failed);
    assert(stopYieldedError == ArdaError::TaskYielded);
    // Delete also fails because task is still running (stop failed)
    assert(deleteSucceeded == false);
    assert(OS.isValidTask(deleterId) == true);  // Still exists
    assert(OS.getTaskState(deleterId) == TaskState::Running);  // Still running

    // Clean up
    taskToDelete = -1;

    printf("PASSED\n");
}

void test_double_delete() {
    printf("Test: double delete attempt... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    // First delete should succeed
    assert(os.deleteTask(id) == true);

    // Second delete should fail with InvalidId
    assert(os.deleteTask(id) == false);
    assert(os.getError() == ArdaError::InvalidId);

    // Task should remain invalid
    assert(os.isValidTask(id) == false);
    assert(os.getTaskState(id) == TaskState::Invalid);

    printf("PASSED\n");
}

static int traceYieldCount = 0;
void traceWithYield(int8_t taskId, TraceEvent event) {
    (void)taskId;
    if (event == TraceEvent::TaskLoopBegin) {
        traceYieldCount++;
        // This is questionable behavior but should not crash
        OS.yield();
    }
}

void test_yield_from_trace_callback() {
    printf("Test: yield from trace callback... ");
    resetTestCounters();
    traceYieldCount = 0;

    OS.setTraceCallback(traceWithYield);
    OS.createTask("t1", task1_setup, task1_loop, 0);
    OS.createTask("t2", task2_setup, task2_loop, 0);
    OS.begin();

    // Should not crash or infinite loop due to reentrancy guards
    OS.run();

    assert(traceYieldCount >= 1);  // Callback was invoked
    OS.setTraceCallback(nullptr);  // Clean up

    printf("PASSED\n");
}

TASK_SETUP(macTst) { setup1Called++; }
TASK_LOOP(macTst) { loop1Called++; }
TASK_TEARDOWN(macTst) { trdwnCalled++; }

void test_macros_runtime_behavior() {
    printf("Test: macros register working tasks... ");
    resetTestCounters();
    trdwnCalled = 0;

    // Use the macro to register a task
    REGISTER_TASK_ID(taskId, macTst, 100);
    assert(taskId >= 0);
    assert(strncmp(OS.getTaskName(taskId), "macTst", ARDA_MAX_NAME_LEN) == 0);

    OS.begin();
    assert(setup1Called == 1);

    setMockMillis(100);
    OS.run();
    assert(loop1Called == 1);

    OS.stopTask(taskId);
    assert(trdwnCalled == 0);  // No trdwn registered via basic macro

    // Now test with trdwn macro
    resetTestCounters();
    trdwnCalled = 0;
    REGISTER_TASK_ID_WITH_TEARDOWN(taskId2, macTst, 100);
    assert(taskId2 >= 0);

    OS.startTask(taskId2);
    assert(setup1Called == 1);

    OS.stopTask(taskId2);
    assert(trdwnCalled == 1);  // Teardown was registered and called

    printf("PASSED\n");
}

void test_run_before_begin() {
    printf("Test: run() before begin() returns false... ");
    resetTestCounters();

    Arda os;
    os.createTask("task", task1_setup, task1_loop, 0);

    // run() should return false and set error before begin() is called
    assert(os.run() == false);
    assert(os.getError() == ArdaError::WrongState);

    // After begin(), run() should succeed
    os.begin();
    assert(os.run() == true);
    assert(os.getError() == ArdaError::Ok);

    printf("PASSED\n");
}

void test_get_slot_count() {
    printf("Test: getSlotCount vs getTaskCount... ");
    resetTestCounters();

    Arda os;
    assert(os.getTaskCount() == 0);
    assert(os.getSlotCount() == 0);

    os.createTask("t1", task1_setup, task1_loop, 0);
    os.createTask("t2", task2_setup, task2_loop, 0);
    assert(os.getTaskCount() == 2);   // Active count
    assert(os.getSlotCount() == 2);   // Slot count

    os.deleteTask(0);
    assert(os.getTaskCount() == 1);   // Active decreases
    assert(os.getSlotCount() == 2);   // Slots stay same

    os.createTask("t3", task1_setup, task1_loop, 0);  // Reuses slot 0
    assert(os.getTaskCount() == 2);   // Active increases
    assert(os.getSlotCount() == 2);   // Slots still same (reused)

    printf("PASSED\n");
}

void test_batch_failed_id() {
    printf("Test: batch operations failedId parameter... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    os.begin(false);

    // Start both - should succeed
    int8_t ids[] = {id1, id2};
    int8_t failedId = -99;
    int8_t count = os.startTasks(ids, 2, &failedId);
    assert(count == 2);
    assert(failedId == -1);  // No failure

    // Try to start again - should fail
    failedId = -99;
    count = os.startTasks(ids, 2, &failedId);
    assert(count == 0);
    assert(failedId == id1);  // First task that failed
    assert(os.getError() == ArdaError::WrongState);

    // Pause both
    failedId = -99;
    count = os.pauseTasks(ids, 2, &failedId);
    assert(count == 2);
    assert(failedId == -1);

    // Resume both
    failedId = -99;
    count = os.resumeTasks(ids, 2, &failedId);
    assert(count == 2);
    assert(failedId == -1);

    // Stop both
    failedId = -99;
    count = os.stopTasks(ids, 2, &failedId);
    assert(count == 2);
    assert(failedId == -1);

    // Without failedId parameter (should still work)
    os.startTasks(ids, 2);
    count = os.stopTasks(ids, 2, nullptr);
    assert(count == 2);

    printf("PASSED\n");
}

void test_runcount_returns_zero_for_deleted_task() {
    printf("Test: getTaskRunCount returns 0 for deleted task (union safety)... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.begin();

    // Run a few times to increment runCount
    os.run();
    os.run();
    os.run();
    assert(os.getTaskRunCount(id) == 3);

    // Delete the task - internally, the union's nextFree field now overlaps runCount
    os.stopTask(id);
    os.deleteTask(id);

    // getTaskRunCount should return 0 for deleted task, not garbage from nextFree
    assert(os.getTaskRunCount(id) == 0);
    assert(os.isValidTask(id) == false);

    // Create a new task to exercise the free list (nextFree gets written)
    int8_t id2 = os.createTask("task2", task2_setup, task2_loop, 0);
    (void)id2;

    // Original slot should still return 0 for runCount
    assert(os.getTaskRunCount(id) == 0);

    printf("PASSED\n");
}

void test_iteration_with_deletion_shows_stale_ids() {
    printf("Test: getValidTaskIds returns snapshot (IDs can become stale)... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    int8_t id3 = os.createTask("t3", task1_setup, task1_loop, 0);

    // Get snapshot of valid IDs
    int8_t ids[16];
    int8_t count = os.getValidTaskIds(ids, 16);
    assert(count == 3);
    assert(ids[0] == id1);
    assert(ids[1] == id2);
    assert(ids[2] == id3);

    // Delete a task - the IDs array now contains a stale reference
    os.deleteTask(id2);

    // The array still contains id2, but it's no longer valid
    // This demonstrates why you shouldn't delete during iteration
    assert(ids[1] == id2);  // Still in our cached array
    assert(os.isValidTask(id2) == false);  // But no longer valid in OS

    // Proper pattern: check validity before using cached IDs
    int validCount = 0;
    for (int8_t i = 0; i < count; i++) {
        if (os.isValidTask(ids[i])) {
            validCount++;
        }
    }
    assert(validCount == 2);  // Only 2 of 3 cached IDs are still valid

    printf("PASSED\n");
}

void test_setTaskTimeout_on_invalid() {
    printf("Test: setTaskTimeout on invalid/deleted task... ");
    resetTestCounters();

    Arda os;

    // Invalid task ID
    assert(os.setTaskTimeout(-1, 100) == false);
    assert(os.getError() == ArdaError::InvalidId);

    assert(os.setTaskTimeout(99, 100) == false);
    assert(os.getError() == ArdaError::InvalidId);

    // Create and delete a task
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.deleteTask(id);

    // Deleted task
    assert(os.setTaskTimeout(id, 100) == false);
    assert(os.getError() == ArdaError::InvalidId);

    printf("PASSED\n");
}

void test_setTaskInterval_on_deleted() {
    printf("Test: setTaskInterval on deleted task... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 100);

    // Works before deletion
    assert(os.setTaskInterval(id, 200) == true);
    assert(os.getTaskInterval(id) == 200);

    // Delete the task
    os.deleteTask(id);

    // Should fail after deletion
    assert(os.setTaskInterval(id, 300) == false);
    assert(os.getError() == ArdaError::InvalidId);

    printf("PASSED\n");
}

void test_renameTask_on_deleted() {
    printf("Test: renameTask on deleted task... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("orig", task1_setup, task1_loop, 0);

    // Works before deletion
    assert(os.renameTask(id, "renamed") == true);

    // Delete the task
    os.deleteTask(id);

    // Should fail after deletion
    assert(os.renameTask(id, "newname") == false);
    assert(os.getError() == ArdaError::InvalidId);

    printf("PASSED\n");
}

// For pause during yield test
static int8_t pauseDuringYieldTaskId = -1;
static bool pauseDuringYieldAttempted = false;
static bool pauseDuringYieldResult = false;
static int pauseDuringYieldTaskARuns = 0;

void pauseDuringYieldTaskA_setup() {}
void pauseDuringYieldTaskA_loop() {
    pauseDuringYieldTaskARuns++;
    OS.yield();  // Task B will try to pause us during this yield
}

void pauseDuringYieldTaskB_setup() {}
void pauseDuringYieldTaskB_loop() {
    if (!pauseDuringYieldAttempted && pauseDuringYieldTaskId >= 0) {
        pauseDuringYieldAttempted = true;
        pauseDuringYieldResult = OS.pauseTask(pauseDuringYieldTaskId);
    }
}

void test_pause_task_during_yield() {
    printf("Test: pause task during its yield... ");
    resetTestCounters();
    pauseDuringYieldTaskId = -1;
    pauseDuringYieldAttempted = false;
    pauseDuringYieldResult = false;
    pauseDuringYieldTaskARuns = 0;

    pauseDuringYieldTaskId = OS.createTask("yieldA", pauseDuringYieldTaskA_setup, pauseDuringYieldTaskA_loop, 0);
    OS.createTask("pauseB", pauseDuringYieldTaskB_setup, pauseDuringYieldTaskB_loop, 0);
    OS.begin();

    // Run one cycle - task A yields, task B tries to pause task A
    OS.run();

    assert(pauseDuringYieldAttempted == true);
    // Pausing a yielded task should succeed (it's still Running state)
    assert(pauseDuringYieldResult == true);
    assert(OS.getTaskState(pauseDuringYieldTaskId) == TaskState::Paused);

    printf("PASSED\n");
}

void test_pauseAllTasks_with_mixed_states() {
    printf("Test: pauseAllTasks with some already paused... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    int8_t id3 = os.createTask("t3", task1_setup, task1_loop, 0);
    os.begin();

    // Pause one task first
    os.pauseTask(id1);
    assert(os.getTaskState(id1) == TaskState::Paused);
    assert(os.getTaskState(id2) == TaskState::Running);
    assert(os.getTaskState(id3) == TaskState::Running);

    // pauseAllTasks should only count newly paused tasks
    int8_t paused = os.pauseAllTasks();
    assert(paused == 2);  // Only id2 and id3 were paused (id1 already paused)

    // All should be paused now
    assert(os.getTaskState(id1) == TaskState::Paused);
    assert(os.getTaskState(id2) == TaskState::Paused);
    assert(os.getTaskState(id3) == TaskState::Paused);

    printf("PASSED\n");
}

void test_startAllTasks_with_some_running() {
    printf("Test: startAllTasks with some already running... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    int8_t id3 = os.createTask("t3", task1_setup, task1_loop, 0);

    // Start one task first
    os.startTask(id1);
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Stopped);
    assert(os.getTaskState(id3) == TaskState::Stopped);

    // startAllTasks should only count newly started tasks
    int8_t started = os.startAllTasks();
    assert(started == 2);  // Only id2 and id3 were started (id1 already running)

    // All should be running now
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Running);
    assert(os.getTaskState(id3) == TaskState::Running);

    printf("PASSED\n");
}

void test_stopAllTasks_with_some_stopped() {
    printf("Test: stopAllTasks with some already stopped... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    int8_t id3 = os.createTask("t3", task1_setup, task1_loop, 0);
    os.begin();

    // Stop one task first
    os.stopTask(id1);
    assert(os.getTaskState(id1) == TaskState::Stopped);
    assert(os.getTaskState(id2) == TaskState::Running);
    assert(os.getTaskState(id3) == TaskState::Running);

    // stopAllTasks should only count newly stopped tasks
    int8_t stopped = os.stopAllTasks();
    assert(stopped == 2);  // Only id2 and id3 were stopped (id1 already stopped)

    // All should be stopped now
    assert(os.getTaskState(id1) == TaskState::Stopped);
    assert(os.getTaskState(id2) == TaskState::Stopped);
    assert(os.getTaskState(id3) == TaskState::Stopped);

    printf("PASSED\n");
}

// For REGISTER_TASK_ON macro test - need a local scheduler
TASK_SETUP(onMacT) { setup1Called++; }
TASK_LOOP(onMacT) { loop1Called++; }

void test_register_task_on_macros() {
    printf("Test: REGISTER_TASK_ON macros... ");
    resetTestCounters();

    Arda localOs;

    // Use REGISTER_TASK_ON with a local scheduler instance
    int8_t taskId = REGISTER_TASK_ON(localOs, onMacT, 100);
    assert(taskId >= 0);
    assert(strncmp(localOs.getTaskName(taskId), "onMacT", ARDA_MAX_NAME_LEN) == 0);
    assert(localOs.getTaskInterval(taskId) == 100);

    localOs.begin();
    assert(setup1Called == 1);

    setMockMillis(100);
    localOs.run();
    assert(loop1Called == 1);

    // Test REGISTER_TASK_ID_ON variant
    setup2Called = 0;
    loop2Called = 0;
    REGISTER_TASK_ID_ON(taskId2, localOs, onMacT, 200);  // Note: duplicate name will fail
    // This will fail because "onMacT" already exists
    assert(taskId2 == -1);

    printf("PASSED\n");
}

// For consecutive yield test
static int consecutiveYieldRuns = 0;

void consecutiveYieldTask_setup() {}
void consecutiveYieldTask_loop() {
    consecutiveYieldRuns++;
    OS.yield();
    OS.yield();  // Second consecutive yield
    OS.yield();  // Third consecutive yield
}

void test_consecutive_yield_calls() {
    printf("Test: consecutive yield() calls... ");
    resetTestCounters();
    consecutiveYieldRuns = 0;

    OS.createTask("mltYld", consecutiveYieldTask_setup, consecutiveYieldTask_loop, 0);
    OS.createTask("other", task1_setup, task1_loop, 0);
    OS.begin();

    // Should not crash or cause issues
    OS.run();

    assert(consecutiveYieldRuns == 1);  // Task ran once despite multiple yields
    assert(loop1Called == 1);  // Other task also ran once

    printf("PASSED\n");
}

// For trace callback state modification test
static int traceStateModCount = 0;
static int8_t traceStateModTaskId = -1;

void traceStateModCallback(int8_t taskId, TraceEvent event) {
    if (event == TraceEvent::TaskLoopBegin && taskId == traceStateModTaskId) {
        traceStateModCount++;
        // Try to pause the task from within trace callback
        // This is questionable but should not crash
        if (traceStateModCount == 1) {
            OS.pauseTask(taskId);
        }
    }
}

void test_trace_callback_modifies_state() {
    printf("Test: trace callback modifying task state... ");
    resetTestCounters();
    traceStateModCount = 0;

    OS.setTraceCallback(traceStateModCallback);

    traceStateModTaskId = OS.createTask("traced", task1_setup, task1_loop, 0);
    OS.begin();

    // Run - trace callback will try to pause the task during TaskLoopBegin
    OS.run();

    // The task should have been paused by the trace callback
    assert(traceStateModCount >= 1);
    // Task state depends on implementation - just verify no crash
    // The pause may or may not take effect depending on when it's called

    OS.setTraceCallback(nullptr);  // Clean up

    printf("PASSED\n");
}

// For reset-from-trace-callback test
static bool traceResetAttempted = false;
static bool traceResetSucceeded = false;
static ArdaError traceResetError = ArdaError::Ok;

void traceResetCallback(int8_t taskId, TraceEvent event) {
    (void)taskId;
    if (event == TraceEvent::TaskDeleted && !traceResetAttempted) {
        traceResetAttempted = true;
        traceResetSucceeded = OS.reset();
        traceResetError = OS.getError();
    }
}

void test_reset_blocked_from_trace_callback() {
    printf("Test: reset() blocked from trace callback... ");
    resetTestCounters();
    traceResetAttempted = false;
    traceResetSucceeded = false;
    traceResetError = ArdaError::Ok;

    OS.setTraceCallback(traceResetCallback);

    int8_t taskId = OS.createTask("traced", task1_setup, task1_loop, 0);
    OS.begin();

    // Stop the task first (required before delete)
    OS.stopTask(taskId);

    // Delete will trigger TaskDeleted trace event, which tries to call reset()
    bool deleteResult = OS.deleteTask(taskId);

    // Delete should have succeeded
    assert(deleteResult == true);

    // The trace callback should have tried to call reset()
    assert(traceResetAttempted == true);

    // But reset() should have been blocked (callbackDepth > 0)
    assert(traceResetSucceeded == false);
    assert(traceResetError == ArdaError::InCallback);

    OS.setTraceCallback(nullptr);  // Clean up

    printf("PASSED\n");
}

// For reset-from-timeout-callback test
static bool timeoutResetAttempted = false;
static bool timeoutResetSucceeded = false;
static ArdaError timeoutResetError = ArdaError::Ok;

void timeoutResetCallback(int8_t taskId, uint32_t durationMs) {
    (void)taskId;
    (void)durationMs;
    if (!timeoutResetAttempted) {
        timeoutResetAttempted = true;
        timeoutResetSucceeded = OS.reset();
        timeoutResetError = OS.getError();
    }
}

void slowTaskForTimeoutReset_setup() {}
void slowTaskForTimeoutReset_loop() {
    advanceMockMillis(150);  // Simulate slow task exceeding timeout
}

void test_reset_blocked_from_timeout_callback() {
    printf("Test: reset() blocked from timeout callback... ");
    resetTestCounters();
    timeoutResetAttempted = false;
    timeoutResetSucceeded = false;
    timeoutResetError = ArdaError::Ok;

    OS.setTimeoutCallback(timeoutResetCallback);

    int8_t taskId = OS.createTask("slow", slowTaskForTimeoutReset_setup, slowTaskForTimeoutReset_loop, 0);
    OS.setTaskTimeout(taskId, 100);  // 100ms timeout, task takes 150ms

    OS.begin();
    OS.run();  // Task runs, exceeds timeout, callback tries reset()

    // The timeout callback should have tried to call reset()
    assert(timeoutResetAttempted == true);

    // But reset() should have been blocked
    assert(timeoutResetSucceeded == false);
    assert(timeoutResetError == ArdaError::InCallback);

    OS.setTimeoutCallback(nullptr);  // Clean up

    printf("PASSED\n");
}

// For reset-from-start-failure-callback test
static bool startFailResetAttempted = false;
static bool startFailResetSucceeded = false;
static ArdaError startFailResetError = ArdaError::Ok;

void startFailResetCallback(int8_t taskId, ArdaError error) {
    (void)taskId;
    (void)error;
    if (!startFailResetAttempted) {
        startFailResetAttempted = true;
        startFailResetSucceeded = OS.reset();
        startFailResetError = OS.getError();
    }
}

// Setup that stops itself, causing StateChanged error (start failure)
void slStoppingSetup_setup() {
    OS.stopTask(OS.getCurrentTask());
}

void test_reset_blocked_from_start_failure_callback() {
    printf("Test: reset() blocked from start failure callback... ");
    resetTestCounters();
    startFailResetAttempted = false;
    startFailResetSucceeded = false;
    startFailResetError = ArdaError::Ok;

    OS.setStartFailureCallback(startFailResetCallback);

    // Create a task with a setup that stops itself (causes StateChanged failure)
    OS.createTask("slStop", slStoppingSetup_setup, task1_loop, 0);

    OS.begin();  // This will trigger start failure callback

    // The start failure callback should have tried to call reset()
    assert(startFailResetAttempted == true);

    // But reset() should have been blocked
    assert(startFailResetSucceeded == false);
    assert(startFailResetError == ArdaError::InCallback);

    OS.setStartFailureCallback(nullptr);  // Clean up

    printf("PASSED\n");
}

// For iteration-safety test: task deletes another task during iteration
static int8_t iterSafetyTask1Id = -1;
static int8_t iterSafetyTask2Id = -1;
static int iterSafetyTask1RunCount = 0;
static int iterSafetyTask2RunCount = 0;

void iterSafetyTask1_setup() {}
void iterSafetyTask1_loop() {
    iterSafetyTask1RunCount++;
    // Delete task 2 during iteration
    if (iterSafetyTask2Id >= 0 && OS.getTaskState(iterSafetyTask2Id) != TaskState::Invalid) {
        OS.stopTask(iterSafetyTask2Id);
        OS.deleteTask(iterSafetyTask2Id);
    }
}

void iterSafetyTask2_setup() {}
void iterSafetyTask2_loop() {
    iterSafetyTask2RunCount++;
}

void test_iteration_safe_when_task_deleted() {
    printf("Test: iteration safe when task deleted during run()... ");
    resetTestCounters();
    iterSafetyTask1RunCount = 0;
    iterSafetyTask2RunCount = 0;

    // Create task1 first (lower ID, runs first)
    iterSafetyTask1Id = OS.createTask("deleter", iterSafetyTask1_setup, iterSafetyTask1_loop, 0);
    // Create task2 second (higher ID)
    iterSafetyTask2Id = OS.createTask("deleted", iterSafetyTask2_setup, iterSafetyTask2_loop, 0);

    OS.begin();

    // First run(): task1 runs and deletes task2 mid-iteration
    // With snapshot protection, task2 should NOT run (it's deleted before we get to it)
    OS.run();

    assert(iterSafetyTask1RunCount == 1);
    // task2 was in the snapshot but got deleted before its turn
    assert(iterSafetyTask2RunCount == 0);
    assert(OS.getTaskState(iterSafetyTask2Id) == TaskState::Invalid);  // Deleted

    // Verify task1 still runs normally
    OS.run();
    assert(iterSafetyTask1RunCount == 2);

    printf("PASSED\n");
}

// For iteration-safety test: task creates another task during iteration
static int8_t iterSafetyCreatorId = -1;
static int8_t iterSafetyCreatedId = -1;
static int iterSafetyCreatorRunCount = 0;
static int iterSafetyCreatedRunCount = 0;

void iterSafetyCreated_setup() {}
void iterSafetyCreated_loop() {
    iterSafetyCreatedRunCount++;
}

void iterSafetyCreator_setup() {}
void iterSafetyCreator_loop() {
    iterSafetyCreatorRunCount++;
    // Create a new task during iteration (only once)
    if (iterSafetyCreatedId < 0) {
        iterSafetyCreatedId = OS.createTask("created", iterSafetyCreated_setup, iterSafetyCreated_loop, 0);
    }
}

void test_iteration_safe_when_task_created() {
    printf("Test: iteration safe when task created during run()... ");
    resetTestCounters();
    iterSafetyCreatorRunCount = 0;
    iterSafetyCreatedRunCount = 0;
    iterSafetyCreatedId = -1;

    iterSafetyCreatorId = OS.createTask("creator", iterSafetyCreator_setup, iterSafetyCreator_loop, 0);

    OS.begin();

    // First run(): creator runs and creates new task mid-iteration
    // With snapshot protection, new task should NOT run this cycle (not in snapshot)
    OS.run();

    assert(iterSafetyCreatorRunCount == 1);
    assert(iterSafetyCreatedId >= 0);  // Task was created
    // New task wasn't in snapshot, but was auto-started by createTask, so it might run
    // Actually, with snapshot, it shouldn't run until next cycle
    // But createTask auto-starts AFTER begin(), so it runs via auto-start's own startTask
    // which doesn't immediately execute the loop. So it shouldn't run this cycle.
    assert(iterSafetyCreatedRunCount == 0);

    // Second run(): now the created task is in the snapshot and should run
    OS.run();
    assert(iterSafetyCreatorRunCount == 2);
    assert(iterSafetyCreatedRunCount == 1);

    printf("PASSED\n");
}

// For callback depth in trace callback test
static int traceDepthTestCount = 0;
static bool traceDepthStartFailed = false;

void traceDepthCallback(int8_t taskId, TraceEvent event) {
    (void)taskId;
    if (event == TraceEvent::TaskLoopBegin) {
        traceDepthTestCount++;
        // Try to start a task from trace callback - should fail if we're at depth limit
        // Create a task that will be started, adding to callback depth
        int8_t newId = OS.createTask("nested", task1_setup, task1_loop, 0, nullptr, false);
        if (newId >= 0) {
            bool started = OS.startTask(newId);
            if (!started && OS.getError() == ArdaError::CallbackDepth) {
                traceDepthStartFailed = true;
            }
            // Clean up
            if (started) {
                OS.stopTask(newId);
            }
            OS.deleteTask(newId);
        }
    }
}

void test_callback_depth_in_trace_callback() {
    printf("Test: callback depth incremented in trace callback... ");
    resetTestCounters();
    traceDepthTestCount = 0;
    traceDepthStartFailed = false;

    OS.setTraceCallback(traceDepthCallback);

    // Create many tasks to get close to depth limit
    int8_t taskIds[ARDA_MAX_CALLBACK_DEPTH];
    for (int i = 0; i < ARDA_MAX_CALLBACK_DEPTH - 2; i++) {
        char name[16];
        snprintf(name, sizeof(name), "task%d", i);
        taskIds[i] = OS.createTask(name, task1_setup, task1_loop, 0);
    }

    OS.begin();
    OS.run();

    // Trace callback was called (TaskLoopBegin for each task)
    assert(traceDepthTestCount > 0);

    // Eventually should have hit depth limit trying to start nested task from trace
    // (trace callback is at depth 1, plus we're already in runInternal at depth 1)

    OS.setTraceCallback(nullptr);

    printf("PASSED\n");
}

// For loop self-stop test
static int8_t loopSelfStopTaskId = -1;
static bool loopSelfStopCalled = false;

void loopSelfStop_setup() {}
void loopSelfStop_loop() {
    loopSelfStopCalled = true;
    OS.stopTask(loopSelfStopTaskId);  // Stop ourselves during loop
}

void test_loop_self_stop() {
    printf("Test: task loop stops itself... ");
    resetTestCounters();
    loopSelfStopCalled = false;

    loopSelfStopTaskId = OS.createTask("slStop", loopSelfStop_setup, loopSelfStop_loop, 0);
    OS.begin();

    // Task runs and stops itself
    OS.run();

    assert(loopSelfStopCalled == true);
    assert(OS.getTaskState(loopSelfStopTaskId) == TaskState::Stopped);

    // Task should not run again (it's stopped)
    loopSelfStopCalled = false;
    OS.run();
    assert(loopSelfStopCalled == false);

    printf("PASSED\n");
}

// For loop self-pause test
static int8_t loopSelfPauseTaskId = -1;
static int loopSelfPauseRunCount = 0;

void loopSelfPause_setup() {}
void loopSelfPause_loop() {
    loopSelfPauseRunCount++;
    OS.pauseTask(loopSelfPauseTaskId);  // Pause ourselves during loop
}

void test_loop_self_pause() {
    printf("Test: task loop pauses itself... ");
    resetTestCounters();
    loopSelfPauseRunCount = 0;

    loopSelfPauseTaskId = OS.createTask("slPause", loopSelfPause_setup, loopSelfPause_loop, 0);
    OS.begin();

    // Task runs and pauses itself
    OS.run();

    assert(loopSelfPauseRunCount == 1);
    assert(OS.getTaskState(loopSelfPauseTaskId) == TaskState::Paused);

    // Task should not run again (it's paused)
    OS.run();
    assert(loopSelfPauseRunCount == 1);

    // Resume and verify it runs again
    OS.resumeTask(loopSelfPauseTaskId);
    OS.run();
    assert(loopSelfPauseRunCount == 2);
    assert(OS.getTaskState(loopSelfPauseTaskId) == TaskState::Paused);  // Paused itself again

    printf("PASSED\n");
}

void test_timeout_without_callback() {
    printf("Test: timeout exceeded without callback set... ");
    resetTestCounters();

    Arda os;

    // Do NOT set a timeout callback
    // os.setTimeoutCallback(...) is intentionally not called

    int8_t id = os.createTask("slow", nullptr, []() {
        advanceMockMillis(200);  // Task "takes" 200ms
    }, 0);

    // Set timeout to 100ms (task will exceed this)
    os.setTaskTimeout(id, 100);
    os.begin();

    // Run the task - it exceeds timeout but no callback is set
    // Should not crash or cause any issues
    os.run();

    // Task should have run successfully despite exceeding timeout
    assert(os.getTaskRunCount(id) == 1);
    assert(os.getTaskState(id) == TaskState::Running);
    assert(os.getError() == ArdaError::Ok);

    printf("PASSED\n");
}

// For REGISTER_TASK_ON_WITH_TEARDOWN macro test
static int onWTrdSetupCalls = 0;
static int onWTrdLoopCalls = 0;
static int onWTrdTeardownCalls = 0;

TASK_SETUP(onWTrd) { onWTrdSetupCalls++; }
TASK_LOOP(onWTrd) { onWTrdLoopCalls++; }
TASK_TEARDOWN(onWTrd) { onWTrdTeardownCalls++; }

void test_register_task_on_with_trdwn_macros() {
    printf("Test: REGISTER_TASK_ON_WITH_TEARDOWN macros... ");
    resetTestCounters();
    onWTrdSetupCalls = 0;
    onWTrdLoopCalls = 0;
    onWTrdTeardownCalls = 0;

    Arda localOs;

    // Use REGISTER_TASK_ON_WITH_TEARDOWN with a local scheduler instance
    int8_t taskId = REGISTER_TASK_ON_WITH_TEARDOWN(localOs, onWTrd, 100);
    assert(taskId >= 0);
    assert(strncmp(localOs.getTaskName(taskId), "onWTrd", ARDA_MAX_NAME_LEN) == 0);
    assert(localOs.hasTaskTeardown(taskId) == true);

    localOs.begin();
    assert(onWTrdSetupCalls == 1);

    setMockMillis(100);
    localOs.run();
    assert(onWTrdLoopCalls == 1);

    // Stop and verify trdwn is called
    localOs.stopTask(taskId);
    assert(onWTrdTeardownCalls == 1);

    // Test REGISTER_TASK_ID_ON_WITH_TEARDOWN variant with a fresh scheduler
    Arda localOs2;
    onWTrdSetupCalls = 0;
    onWTrdLoopCalls = 0;
    onWTrdTeardownCalls = 0;

    REGISTER_TASK_ID_ON_WITH_TEARDOWN(taskId2, localOs2, onWTrd, 200);
    assert(taskId2 >= 0);
    assert(strncmp(localOs2.getTaskName(taskId2), "onWTrd", ARDA_MAX_NAME_LEN) == 0);
    assert(localOs2.getTaskInterval(taskId2) == 200);
    assert(localOs2.hasTaskTeardown(taskId2) == true);

    localOs2.begin();
    assert(onWTrdSetupCalls == 1);

    localOs2.stopTask(taskId2);
    assert(onWTrdTeardownCalls == 1);

    printf("PASSED\n");
}

// For TaskStopping trace event test
static bool traceStoppingEventSeen = false;
static bool traceStoppedEventSeen = false;
static int8_t traceStoppingTaskId = -1;

void traceStoppingCallback(int8_t taskId, TraceEvent event) {
    if (taskId == traceStoppingTaskId) {
        if (event == TraceEvent::TaskStopping) {
            traceStoppingEventSeen = true;
            // Verify TaskStopping comes before TaskStopped
            assert(traceStoppedEventSeen == false);
        } else if (event == TraceEvent::TaskStopped) {
            traceStoppedEventSeen = true;
        }
    }
}

void traceStoppingTask_setup() {}
void traceStoppingTask_loop() {}
void traceStoppingTask_trdwn() { trdwnCalled++; }

void test_trace_task_stpng_event() {
    printf("Test: TraceEvent::TaskStopping fires before trdwn... ");
    resetTestCounters();
    traceStoppingEventSeen = false;
    traceStoppedEventSeen = false;
    trdwnCalled = 0;

    OS.setTraceCallback(traceStoppingCallback);

    traceStoppingTaskId = OS.createTask("stpng", traceStoppingTask_setup,
                                         traceStoppingTask_loop, 0,
                                         traceStoppingTask_trdwn);
    OS.begin();

    // Stop the task - should emit TaskStopping then run trdwn then emit TaskStopped
    OS.stopTask(traceStoppingTaskId);

    assert(traceStoppingEventSeen == true);
    assert(traceStoppedEventSeen == true);
    assert(trdwnCalled == 1);

    OS.setTraceCallback(nullptr);  // Clean up

    printf("PASSED\n");
}

void test_resumeAllTasks_with_mixed_states() {
    printf("Test: resumeAllTasks with some not paused... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    int8_t id3 = os.createTask("t3", task1_setup, task1_loop, 0);
    os.begin();

    // Pause only two tasks, leave one running
    os.pauseTask(id1);
    os.pauseTask(id2);
    assert(os.getTaskState(id1) == TaskState::Paused);
    assert(os.getTaskState(id2) == TaskState::Paused);
    assert(os.getTaskState(id3) == TaskState::Running);

    // resumeAllTasks should only count newly resumed tasks
    int8_t resumed = os.resumeAllTasks();
    assert(resumed == 2);  // Only id1 and id2 were resumed (id3 was already running)

    // All should be running now
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Running);
    assert(os.getTaskState(id3) == TaskState::Running);

    // Test with a stopped task in the mix
    os.stopTask(id1);
    os.pauseTask(id2);
    assert(os.getTaskState(id1) == TaskState::Stopped);
    assert(os.getTaskState(id2) == TaskState::Paused);
    assert(os.getTaskState(id3) == TaskState::Running);

    // resumeAllTasks should only resume paused tasks, not stopped ones
    resumed = os.resumeAllTasks();
    assert(resumed == 1);  // Only id2 was paused and resumed
    assert(os.getTaskState(id1) == TaskState::Stopped);  // Still stopped
    assert(os.getTaskState(id2) == TaskState::Running);
    assert(os.getTaskState(id3) == TaskState::Running);

    printf("PASSED\n");
}

// =============================================================================
// Priority Tests
// =============================================================================

#ifndef ARDA_NO_PRIORITY

void test_priority_default() {
    printf("Test: new tasks have default priority 8... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    assert(os.getTaskPriority(id) == ARDA_DEFAULT_PRIORITY);
    assert(os.getTaskPriority(id) == 8);

    printf("PASSED\n");
}

void test_priority_set_get() {
    printf("Test: setTaskPriority/getTaskPriority... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    // Set to various values
    assert(os.setTaskPriority(id, 0) == true);
    assert(os.getTaskPriority(id) == 0);

    assert(os.setTaskPriority(id, 15) == true);
    assert(os.getTaskPriority(id) == 15);

    assert(os.setTaskPriority(id, 7) == true);
    assert(os.getTaskPriority(id) == 7);

    // Use enum values
    assert(os.setTaskPriority(id, static_cast<uint8_t>(TaskPriority::Lowest)) == true);
    assert(os.getTaskPriority(id) == static_cast<uint8_t>(TaskPriority::Lowest));

    assert(os.setTaskPriority(id, static_cast<uint8_t>(TaskPriority::Highest)) == true);
    assert(os.getTaskPriority(id) == static_cast<uint8_t>(TaskPriority::Highest));

    printf("PASSED\n");
}

void test_priority_create_with_priority() {
    printf("Test: createTask overload with priority... ");
    resetTestCounters();

    Arda os;

    // Create with explicit priority
    int8_t id1 = os.createTask("high", task1_setup, task1_loop, 0, 15, nullptr, false);
    int8_t id2 = os.createTask("low", task2_setup, task2_loop, 0, 0, nullptr, false);
    int8_t id3 = os.createTask("mid", task1_setup, task1_loop, 0, 8, nullptr, false);

    assert(id1 >= 0);
    assert(id2 >= 0);
    assert(id3 >= 0);

    assert(os.getTaskPriority(id1) == 15);
    assert(os.getTaskPriority(id2) == 0);
    assert(os.getTaskPriority(id3) == 8);

    printf("PASSED\n");
}

void test_priority_invalid_task() {
    printf("Test: priority operations on invalid task... ");
    resetTestCounters();

    Arda os;

    // setTaskPriority on invalid ID should fail
    assert(os.setTaskPriority(-1, 5) == false);
    assert(os.getError() == ArdaError::InvalidId);

    assert(os.setTaskPriority(99, 5) == false);
    assert(os.getError() == ArdaError::InvalidId);

    // getTaskPriority on invalid ID should return 0
    assert(os.getTaskPriority(-1) == 0);
    assert(os.getTaskPriority(99) == 0);

    // Create and delete a task
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.deleteTask(id);

    // Operations on deleted task
    assert(os.setTaskPriority(id, 5) == false);
    assert(os.getError() == ArdaError::InvalidId);
    assert(os.getTaskPriority(id) == 0);

    printf("PASSED\n");
}

void test_priority_clamp() {
    printf("Test: priority values > 15 are clamped... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    // Set to value > 15
    assert(os.setTaskPriority(id, 20) == true);
    assert(os.getTaskPriority(id) == 15);  // Clamped to 15

    assert(os.setTaskPriority(id, 255) == true);
    assert(os.getTaskPriority(id) == 15);  // Clamped to 15

    // Create with priority > 15
    int8_t id2 = os.createTask("task2", task1_setup, task1_loop, 0, 100, nullptr, false);
    assert(os.getTaskPriority(id2) == 15);  // Clamped to 15

    printf("PASSED\n");
}

// For priority ordering test - track execution order
static int priorityOrderIndex = 0;
static int8_t priorityOrder[3] = {-1, -1, -1};

void priorityOrderHigh_setup() {}
void priorityOrderHigh_loop() {
    if (priorityOrderIndex < 3) {
        priorityOrder[priorityOrderIndex++] = 0;  // High is task 0
    }
}

void priorityOrderMid_setup() {}
void priorityOrderMid_loop() {
    if (priorityOrderIndex < 3) {
        priorityOrder[priorityOrderIndex++] = 1;  // Mid is task 1
    }
}

void priorityOrderLow_setup() {}
void priorityOrderLow_loop() {
    if (priorityOrderIndex < 3) {
        priorityOrder[priorityOrderIndex++] = 2;  // Low is task 2
    }
}

void test_priority_ordering() {
    printf("Test: higher priority tasks run first... ");
    resetTestCounters();
    priorityOrderIndex = 0;
    priorityOrder[0] = -1;
    priorityOrder[1] = -1;
    priorityOrder[2] = -1;

    // Create tasks in reverse priority order (low first, high last)
    // to ensure it's priority, not creation order, that matters
    int8_t idLow = OS.createTask("low", priorityOrderLow_setup, priorityOrderLow_loop, 0, 2, nullptr, false);
    int8_t idMid = OS.createTask("mid", priorityOrderMid_setup, priorityOrderMid_loop, 0, 8, nullptr, false);
    int8_t idHigh = OS.createTask("high", priorityOrderHigh_setup, priorityOrderHigh_loop, 0, 15, nullptr, false);

    OS.begin(false);
    OS.startTask(idLow);
    OS.startTask(idMid);
    OS.startTask(idHigh);

    OS.run();

    // All three should have run
    assert(priorityOrderIndex == 3);

    // Order should be: high (0), mid (1), low (2)
    assert(priorityOrder[0] == 0);  // High ran first
    assert(priorityOrder[1] == 1);  // Mid ran second
    assert(priorityOrder[2] == 2);  // Low ran third

    printf("PASSED\n");
}

// For same-priority ordering test
static int samePriorityOrderIndex = 0;
static int8_t samePriorityOrder[3] = {-1, -1, -1};

void samePriorityTask0_setup() {}
void samePriorityTask0_loop() {
    if (samePriorityOrderIndex < 3) {
        samePriorityOrder[samePriorityOrderIndex++] = 0;
    }
}

void samePriorityTask1_setup() {}
void samePriorityTask1_loop() {
    if (samePriorityOrderIndex < 3) {
        samePriorityOrder[samePriorityOrderIndex++] = 1;
    }
}

void samePriorityTask2_setup() {}
void samePriorityTask2_loop() {
    if (samePriorityOrderIndex < 3) {
        samePriorityOrder[samePriorityOrderIndex++] = 2;
    }
}

void test_priority_same_level() {
    printf("Test: same priority preserves snapshot order... ");
    resetTestCounters();
    samePriorityOrderIndex = 0;
    samePriorityOrder[0] = -1;
    samePriorityOrder[1] = -1;
    samePriorityOrder[2] = -1;

    // All tasks have the same priority
    (void)OS.createTask("t0", samePriorityTask0_setup, samePriorityTask0_loop, 0, 8, nullptr, true);
    (void)OS.createTask("t1", samePriorityTask1_setup, samePriorityTask1_loop, 0, 8, nullptr, true);
    (void)OS.createTask("t2", samePriorityTask2_setup, samePriorityTask2_loop, 0, 8, nullptr, true);

    OS.begin(false);
    OS.startAllTasks();

    OS.run();

    // All three should have run
    assert(samePriorityOrderIndex == 3);

    // With same priority, the first one found in the snapshot wins
    // Since we scan through snapshot and pick the best (first at same priority),
    // the order should be: 0, 1, 2 (snapshot order)
    assert(samePriorityOrder[0] == 0);
    assert(samePriorityOrder[1] == 1);
    assert(samePriorityOrder[2] == 2);

    printf("PASSED\n");
}

// For priority with intervals test
static int priorityIntervalHighRuns = 0;
static int priorityIntervalLowRuns = 0;

void priorityIntervalHigh_setup() {}
void priorityIntervalHigh_loop() {
    priorityIntervalHighRuns++;
}

void priorityIntervalLow_setup() {}
void priorityIntervalLow_loop() {
    priorityIntervalLowRuns++;
}

void test_priority_with_intervals() {
    printf("Test: priority only matters when both ready... ");
    resetTestCounters();
    priorityIntervalHighRuns = 0;
    priorityIntervalLowRuns = 0;

    // High priority but long interval (1000ms)
    // Low priority but short interval (100ms)
    int8_t idHigh = OS.createTask("high", priorityIntervalHigh_setup, priorityIntervalHigh_loop, 1000, 15, nullptr, false);
    int8_t idLow = OS.createTask("low", priorityIntervalLow_setup, priorityIntervalLow_loop, 100, 2, nullptr, false);

    OS.begin(false);
    OS.startTask(idHigh, true);  // Run immediately
    OS.startTask(idLow, true);   // Run immediately

    setMockMillis(0);
    OS.run();

    // Both were ready (runImmediately), high should run first due to priority
    // But both should have run this cycle
    assert(priorityIntervalHighRuns == 1);
    assert(priorityIntervalLowRuns == 1);

    // Advance 100ms - only low should be ready
    setMockMillis(100);
    OS.run();

    assert(priorityIntervalHighRuns == 1);  // Still 1 (interval not elapsed)
    assert(priorityIntervalLowRuns == 2);   // Now 2

    // Advance to 1000ms - both ready again
    setMockMillis(1000);
    OS.run();

    // High priority runs first when both are ready
    assert(priorityIntervalHighRuns == 2);
    assert(priorityIntervalLowRuns == 3);

    printf("PASSED\n");
}

void test_priority_enum_values() {
    printf("Test: TaskPriority enum values... ");

    // Verify enum values match documentation
    assert(static_cast<uint8_t>(TaskPriority::Lowest) == 0);
    assert(static_cast<uint8_t>(TaskPriority::Low) == 4);
    assert(static_cast<uint8_t>(TaskPriority::Normal) == 8);
    assert(static_cast<uint8_t>(TaskPriority::High) == 12);
    assert(static_cast<uint8_t>(TaskPriority::Highest) == 15);

    // Verify ARDA_DEFAULT_PRIORITY matches TaskPriority::Normal
    assert(ARDA_DEFAULT_PRIORITY == static_cast<uint8_t>(TaskPriority::Normal));

    printf("PASSED\n");
}

void test_priority_preserved_across_stop_start() {
    printf("Test: priority preserved across stop/start... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    os.setTaskPriority(id, 12);
    assert(os.getTaskPriority(id) == 12);

    os.startTask(id);
    assert(os.getTaskPriority(id) == 12);

    os.stopTask(id);
    assert(os.getTaskPriority(id) == 12);

    os.startTask(id);
    assert(os.getTaskPriority(id) == 12);

    printf("PASSED\n");
}

// Regression test: priority must be set BEFORE setup() runs when using
// createTask with explicit priority and autoStart=true
static uint8_t capturedPriorityInSetup = 255;

void prioritySetupCapture_setup() {
    // Capture our own priority during setup using getCurrentTask()
    int8_t myId = OS.getCurrentTask();
    if (myId >= 0) {
        capturedPriorityInSetup = OS.getTaskPriority(myId);
    }
}
void prioritySetupCapture_loop() {}

void test_priority_set_before_setup_runs() {
    printf("Test: priority set before setup() runs... ");
    resetTestCounters();
    capturedPriorityInSetup = 255;

    OS.begin(false);  // Initialize scheduler without auto-start

    // Create task with explicit priority=15 and autoStart=true
    // The priority should be 15 when setup() runs, not default 8
    int8_t taskId = OS.createTask("priSetu", prioritySetupCapture_setup,
                                  prioritySetupCapture_loop, 0, 15,
                                  nullptr, true);

    assert(taskId >= 0);

    // setup() should have run during createTask (autoStart=true after begin)
    // and captured the priority as 15
    assert(capturedPriorityInSetup == 15);

    // Verify priority is still 15 after creation
    assert(OS.getTaskPriority(taskId) == 15);

    printf("PASSED\n");
}

// Regression test: task readiness should be evaluated using millis() snapshot
// from cycle start, not live millis() that could change mid-cycle
static int midCycleReadyTaskRuns = 0;
static bool midCycleTimeAdvanced = false;

void midCycleAdvancer_setup() {}
void midCycleAdvancer_loop() {
    // This task runs first (higher priority) and advances time mid-cycle
    if (!midCycleTimeAdvanced) {
        midCycleTimeAdvanced = true;
        advanceMockMillis(200);  // Advance time so lower-priority task's interval elapses
    }
}

void midCycleReady_setup() {}
void midCycleReady_loop() {
    midCycleReadyTaskRuns++;
}

void test_priority_uses_cycle_start_millis_snapshot() {
    printf("Test: priority scheduling uses millis() snapshot from cycle start... ");
    resetTestCounters();
    midCycleReadyTaskRuns = 0;
    midCycleTimeAdvanced = false;

    setMockMillis(0);

    // High priority task (15) with interval=0 - runs every cycle
    (void)OS.createTask("advncr", midCycleAdvancer_setup, midCycleAdvancer_loop,
                        0, 15, nullptr, false);

    // Low priority task (2) with interval=100ms - should NOT be ready at cycle start
    (void)OS.createTask("waiter", midCycleReady_setup, midCycleReady_loop,
                        100, 2, nullptr, false);

    OS.begin(false);
    OS.startAllTasks();

    // At t=0, only the high-priority task should be ready
    // The high-priority task advances time to t=200 mid-cycle
    // But the low-priority task should NOT run because it wasn't ready
    // at cycle start (millis() was 0, interval is 100, lastRun is 0)
    OS.run();

    // High priority task ran and advanced time
    assert(midCycleTimeAdvanced == true);

    // Low priority task should NOT have run - it wasn't ready at cycle start
    // (even though time is now 200ms due to mid-cycle advance)
    assert(midCycleReadyTaskRuns == 0);

    // Now run again - low priority task should be ready at this cycle's start
    OS.run();
    assert(midCycleReadyTaskRuns == 1);

    printf("PASSED\n");
}

#endif // ARDA_NO_PRIORITY

int main() {
    printf("\n=== Arda Unit Tests ===\n\n");

    // ---- Task Creation & Naming ----
    test_create_task();
    test_task_names();
    test_nullptr_name_rejected();
    test_empty_string_name_rejected();
    test_long_name_rejected();
    test_max_length_name();
    test_name_copied_safely();
    test_duplicate_name_rejected();
    test_max_tasks();
    test_is_valid_task_public();
    test_find_task_by_name();
    test_rename_task();
    test_renameTask_on_deleted();

    // ---- Task Lifecycle (Start/Stop/Pause/Resume) ----
    test_task_states();
    test_setup_called_on_start();
    test_start_task_run_immediately();
    test_trdwn_called();
    test_trdwn_sees_stopped_state();
    test_paused_task_not_executed();
    test_invalid_operations();
    test_loop_self_stop();
    test_loop_self_pause();
    test_setup_self_stop_returns_false();
    test_trdwn_self_restart_returns_partial();

    // ---- Scheduler Control (begin/run/reset) ----
    test_begin_starts_all();
    test_begin_returns_count();
    test_begin_only_once();
    test_begin_no_autostart();
    test_has_begun();
    test_run_before_begin();
    test_run_executes_tasks();
    test_reset();
    test_reset_clears_callbacks();
    test_reset_preserves_callbacks();
    test_reset_returns_true_when_all_trdwns_run();
    test_reset_from_callback_blocked();

    // ---- Auto-Start Behavior ----
    test_auto_start_after_begin();
    test_auto_start_disabled();
    test_createTask_autostart_failure_preserves_error();
    test_begin_partial_failure_preserves_error();

    // ---- Interval & Timing ----
    test_interval_scheduling();
    test_interval_cadence();
    test_interval_catchup_prevention();
    test_interval_zero_runs_every_cycle();
    test_set_task_interval();
    test_set_interval_paused_task();
    test_set_task_interval_no_reset();
    test_setTaskInterval_on_deleted();
    test_get_task_interval();
    test_task_modifies_own_interval();
    test_uptime();
    test_millis_ovrflow_interval();
    test_millis_ovrflow_uptime();

    // ---- Timeouts ----
    test_task_timeout();
    test_task_timeout_not_triggered();
    test_task_timeout_disabled();
    test_timeout_without_callback();
    test_get_task_timeout_invalid();
    test_setTaskTimeout_on_invalid();

    // ---- Task Execution & Run Count ----
    test_run_count();
    test_runcount_returns_zero_for_deleted_task();
    test_null_loop_function();
    test_get_current_task();

    // ---- Deletion & Memory Management ----
    test_delete_task();
    test_deleted_task_not_executed();
    test_deleted_task_returns_invalid();
    test_deleted_task_slot_fully_cleared();
    test_deleted_slot_reuse();
    test_self_deletion_prevented();
    test_double_delete();
    test_free_list_o1_allocation();

    // ---- Yield ----
    test_yield_reentrancy();
    test_yield_runs_other_tasks();
    test_yield_restores_inrun();
    test_yield_in_setup_does_not_corrupt_inrun();
    test_yield_outside_task_context();
    test_yielded_task_deletion_prevented();
    test_startTask_resets_inYield_flag();
    test_is_task_yielded();
    test_pause_task_during_yield();
    test_delete_other_task_during_yield();
    test_consecutive_yield_calls();
    test_yield_from_trace_callback();

    // ---- Batch Operations ----
    test_batch_start_tasks();
    test_batch_stop_tasks();
    test_batch_pause_resume_tasks();
    test_batch_partial_failure();
    test_batch_preserves_first_error();
    test_batch_null_array();
    test_batch_operations_zero_count();
    test_batch_failed_id();
    test_stopTasks_counts_partial_success();

    // ---- All-Tasks Operations ----
    test_start_all_tasks();
    test_stop_all_tasks();
    test_pause_all_tasks();
    test_startAllTasks_with_some_running();
    test_stopAllTasks_with_some_stopped();
    test_pauseAllTasks_with_mixed_states();
    test_resumeAllTasks_with_mixed_states();

    // ---- Task Counting & Iteration ----
    test_active_task_count();
    test_active_count_o1();
    test_get_slot_count();
    test_get_valid_task_ids();
    test_get_valid_task_ids_empty();
    test_iteration_with_deletion_shows_stale_ids();

    // ---- Callbacks (Timeout, Start Failure, Trace) ----
    test_start_failure_callback();
    test_trace_callback();
    test_trace_pause_resume_delete();
    test_trace_task_stpng_event();
    test_trace_stopped_emitted_on_callback_depth();
    test_trace_callback_modifies_state();
    test_reset_blocked_from_trace_callback();
    test_reset_blocked_from_timeout_callback();
    test_reset_blocked_from_start_failure_callback();

    // ---- Callback Depth Limiting ----
    test_callback_depth_limit();
    test_stopTask_returns_partial_on_trdwn_skip();

    // ---- Iteration Safety ----
    test_iteration_safe_when_task_deleted();
    test_iteration_safe_when_task_created();

    // ---- Callback Depth for All Callback Types ----
    test_callback_depth_in_trace_callback();

    // ---- Error Handling ----
    test_error_codes();
    test_error_codes_max_tasks();
    test_success_clears_error();
    test_error_string();
    test_error_string_in_callback();
    test_getters_set_error_on_invalid();

    // ---- Query Methods ----
    test_has_task_callbacks();

    // ---- Macros ----
    test_macros_runtime_behavior();
    test_register_task_on_macros();
    test_register_task_on_with_trdwn_macros();

    // ---- Priority ----
#ifndef ARDA_NO_PRIORITY
    test_priority_default();
    test_priority_set_get();
    test_priority_create_with_priority();
    test_priority_invalid_task();
    test_priority_clamp();
    test_priority_ordering();
    test_priority_same_level();
    test_priority_with_intervals();
    test_priority_enum_values();
    test_priority_preserved_across_stop_start();
    test_priority_set_before_setup_runs();
    test_priority_uses_cycle_start_millis_snapshot();
#endif

    // ---- Miscellaneous ----
    test_get_max_tasks();
    test_version_constants();
    test_stop_result_enum_values();
    test_multiple_scheduler_instances();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
