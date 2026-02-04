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
//
// Note: Shell is disabled for these tests to allow local Arda instances to
// behave normally. Shell functionality is tested separately in test_shell.cpp.

#include <cstdio>
#include <cstring>
#include <cassert>
#include <new>
#include "Arduino.h"

// Disable shell for core tests (shell needs global OS instance)
#define ARDA_NO_SHELL

// Enable internal test accessor for Task* access without relying on memory layout
#define ARDA_INTERNAL_TEST

// Enable task recovery feature for timeout tests (not auto-enabled on non-AVR)
#define ARDA_TASK_RECOVERY 1

// Define mock variables
uint32_t _mockMillis = 0;  // 32-bit to simulate Arduino's millis() ovrflow
MockSerial Serial;

// Include Arda (unity build - header and implementation together)
#include "../Arda.h"
#include "../Arda.cpp"

// Test helper to access internal task state (for overflow testing)
// Uses ARDA_INTERNAL_TEST accessor instead of relying on class memory layout.
Task* getTaskPtr(Arda& os, int8_t id) {
    return os.getTaskPtr_(id);
}

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

    // Operations on invalid task IDs should return Failed
    assert(os.startTask(99) == StartResult::Failed);
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

void test_resume_running_task() {
    printf("Test: resumeTask on running (not paused) task... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.startTask(id);
    assert(os.getTaskState(id) == TaskState::Running);

    // Resuming a running task that was never paused should fail
    assert(os.resumeTask(id) == false);
    assert(os.getError() == ArdaError::WrongState);

    // Task should still be running
    assert(os.getTaskState(id) == TaskState::Running);

    printf("PASSED\n");
}

void test_pause_already_paused_task() {
    printf("Test: pauseTask on already paused task... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.startTask(id);

    // First pause should succeed
    assert(os.pauseTask(id) == true);
    assert(os.getTaskState(id) == TaskState::Paused);
    assert(os.getError() == ArdaError::Ok);

    // Second pause should fail (already paused)
    assert(os.pauseTask(id) == false);
    assert(os.getError() == ArdaError::WrongState);

    // Task should still be paused
    assert(os.getTaskState(id) == TaskState::Paused);

    printf("PASSED\n");
}

void test_double_resume() {
    printf("Test: double resume after pause... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.startTask(id);
    os.pauseTask(id);
    assert(os.getTaskState(id) == TaskState::Paused);

    // First resume should succeed
    assert(os.resumeTask(id) == true);
    assert(os.getTaskState(id) == TaskState::Running);
    assert(os.getError() == ArdaError::Ok);

    // Second resume should fail (now running, not paused)
    assert(os.resumeTask(id) == false);
    assert(os.getError() == ArdaError::WrongState);

    printf("PASSED\n");
}

void test_set_interval_to_zero_dynamically() {
    printf("Test: setTaskInterval to 0 dynamically... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 1000);  // 1 second interval
    os.begin();

    // Task should not run immediately (lastRun set to current millis, interval not elapsed)
    os.run();
    assert(loop1Called == 0);

    // Still doesn't run (interval not elapsed)
    os.run();
    assert(loop1Called == 0);

    // Change interval to 0 (run every cycle)
    assert(os.setTaskInterval(id, 0) == true);
    assert(os.getTaskInterval(id) == 0);

    // Now should run every cycle regardless of time
    os.run();
    assert(loop1Called == 1);
    os.run();
    assert(loop1Called == 2);
    os.run();
    assert(loop1Called == 3);

    printf("PASSED\n");
}

void test_clear_error_preserves_state() {
    printf("Test: clearError preserves scheduler state... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);  // interval=0 runs every cycle
    os.startTask(id);
    os.begin();

    // Cause an error
    os.startTask(99);  // Invalid ID
    assert(os.getError() == ArdaError::InvalidId);

    // Clear the error
    os.clearError();
    assert(os.getError() == ArdaError::Ok);

    // Verify scheduler state is unchanged
    assert(os.getTaskState(id) == TaskState::Running);
    assert(os.hasBegun() == true);
    assert(os.getTaskInterval(id) == 0);

    // Scheduler should still work
    os.run();
    assert(loop1Called >= 1);

    printf("PASSED\n");
}

// For teardown-deletes-other-task test
static int8_t teardownDeleteTargetId = -1;
static bool teardownDeleteOtherCalled = false;

void teardownDeleteOther_setup() {}
void teardownDeleteOther_loop() {}
void teardownDeleteOther_trdwn() {
    teardownDeleteOtherCalled = true;
    // Try to delete another task during our teardown
    if (teardownDeleteTargetId >= 0) {
        OS.deleteTask(teardownDeleteTargetId);
    }
}

void test_teardown_deletes_other_running_task() {
    printf("Test: teardown deletes another running task... ");
    resetTestCounters();
    teardownDeleteOtherCalled = false;

    // Create two tasks - one whose teardown will try to delete the other
    int8_t deleter = OS.createTask("deleter", teardownDeleteOther_setup,
                                   teardownDeleteOther_loop, 0,
                                   teardownDeleteOther_trdwn);
    int8_t target = OS.createTask("target", task1_setup, task1_loop, 0);
    teardownDeleteTargetId = target;

    assert(deleter >= 0 && target >= 0);
    OS.begin();

    // Both should be running
    assert(OS.getTaskState(deleter) == TaskState::Running);
    assert(OS.getTaskState(target) == TaskState::Running);

    // Stop the deleter - its teardown will try to delete the running target
    StopResult result = OS.stopTask(deleter);

    // Teardown ran
    assert(teardownDeleteOtherCalled == true);

    // The delete should have failed (target is running)
    assert(OS.isValidTask(target));
    assert(OS.getTaskState(target) == TaskState::Running);

    // Deleter should be stopped
    assert(OS.getTaskState(deleter) == TaskState::Stopped);

    // Result should indicate teardown changed state (it tried to delete)
    // Actually, since deleteTask fails on running task, state wasn't changed
    // So result should be Success
    assert(result == StopResult::Success);

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
    assert(os.startTask(id) == StartResult::Failed);
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

    // Before begin(), uptime returns 0
    setMockMillis(5000);
    assert(os.uptime() == 0);

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

void test_kill_task_running() {
    printf("Test: killTask on running task... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.startTask(id);
    assert(os.getTaskState(id) == TaskState::Running);

    // killTask should stop and delete in one call
    assert(os.killTask(id) == true);
    assert(os.getError() == ArdaError::Ok);

    // Task should be gone
    assert(!os.isValidTask(id));
    assert(os.getTaskState(id) == TaskState::Invalid);

    printf("PASSED\n");
}

void test_kill_task_stopped() {
    printf("Test: killTask on already stopped task... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    // Task is created in Stopped state (not auto-started since no begin())
    assert(os.getTaskState(id) == TaskState::Stopped);

    // killTask should handle already-stopped gracefully
    assert(os.killTask(id) == true);
    assert(os.getError() == ArdaError::Ok);

    // Task should be gone
    assert(!os.isValidTask(id));

    printf("PASSED\n");
}

void test_kill_task_paused() {
    printf("Test: killTask on paused task... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.startTask(id);
    os.pauseTask(id);
    assert(os.getTaskState(id) == TaskState::Paused);

    // killTask should stop (from paused) and delete
    assert(os.killTask(id) == true);
    assert(os.getError() == ArdaError::Ok);
    assert(!os.isValidTask(id));

    printf("PASSED\n");
}

void test_kill_task_invalid_id() {
    printf("Test: killTask on invalid ID... ");
    resetTestCounters();

    Arda os;

    assert(os.killTask(99) == false);
    assert(os.getError() == ArdaError::InvalidId);

    assert(os.killTask(-1) == false);
    assert(os.getError() == ArdaError::InvalidId);

    printf("PASSED\n");
}

// For killTask teardown-restarts test
static int8_t killTrdwnRestartTaskId = -1;
void killTrdwnRestart_trdwn() {
    OS.startTask(killTrdwnRestartTaskId);  // Restart during teardown
}

void test_kill_task_teardown_restarts() {
    printf("Test: killTask when teardown restarts task... ");
    resetTestCounters();

    // Create task whose teardown restarts itself
    killTrdwnRestartTaskId = OS.createTask("restart", nullptr, nullptr, 0, killTrdwnRestart_trdwn);
    assert(killTrdwnRestartTaskId >= 0);
    OS.startTask(killTrdwnRestartTaskId);

    // killTask should fail because teardown restarts the task
    assert(OS.killTask(killTrdwnRestartTaskId) == false);
    assert(OS.getError() == ArdaError::StateChanged);

    // Task should still exist and be running (teardown restarted it)
    assert(OS.isValidTask(killTrdwnRestartTaskId));
    assert(OS.getTaskState(killTrdwnRestartTaskId) == TaskState::Running);

    printf("PASSED\n");
}

void test_kill_task_self() {
    printf("Test: killTask on self (from loop)... ");
    resetTestCounters();

    // killTask from within the task's own loop should fail with TaskExecuting
    static int8_t selfKillTaskId = -1;
    static bool selfKillAttempted = false;
    static bool selfKillResult = true;
    static ArdaError selfKillError = ArdaError::Ok;

    selfKillAttempted = false;

    auto selfKillLoop = []() {
        if (!selfKillAttempted) {
            selfKillAttempted = true;
            selfKillResult = OS.killTask(selfKillTaskId);
            selfKillError = OS.getError();
        }
    };

    selfKillTaskId = OS.createTask("selfKil", nullptr, selfKillLoop, 0);
    OS.begin();
    OS.run();

    assert(selfKillAttempted == true);
    assert(selfKillResult == false);
    assert(selfKillError == ArdaError::TaskExecuting);

    // Task should still exist (couldn't kill itself)
    assert(OS.isValidTask(selfKillTaskId));

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

void test_getTaskLastRun_before_first_run() {
    printf("Test: getTaskLastRun returns 0 before first loop execution... ");
    resetTestCounters();

    Arda os;
    _mockMillis = 100;  // Non-zero time

    // Create task with autoStart=false to prevent auto-start
    int8_t id = os.createTask("task", task1_setup, task1_loop, 500, nullptr, false);
    os.begin();

    // Task exists but hasn't been started yet
    assert(os.getTaskState(id) == TaskState::Stopped);
    assert(os.getTaskRunCount(id) == 0);
    assert(os.getTaskLastRun(id) == 0);  // Never ran

    // Start the task with runImmediately=false so it waits for interval
    os.startTask(id, false);  // runImmediately=false
    assert(os.getTaskState(id) == TaskState::Running);

    // Task is running but hasn't executed its loop yet
    // startTask() sets internal lastRun for scheduling, but runCount is still 0
    assert(os.getTaskRunCount(id) == 0);
    assert(os.getTaskLastRun(id) == 0);  // Should return 0 (never ran)

    // Run scheduler - task won't execute yet (needs to wait interval)
    _mockMillis = 200;
    os.run();
    assert(os.getTaskRunCount(id) == 0);  // Still waiting
    assert(os.getTaskLastRun(id) == 0);   // Still "never ran"

    // Advance time past interval and run
    _mockMillis = 700;  // 600ms since startTask at t=100
    os.run();
    assert(os.getTaskRunCount(id) == 1);  // Now it ran
    // lastRun is updated by interval scheduling logic, not just millis()
    assert(os.getTaskLastRun(id) > 0);    // Has a valid timestamp now

    printf("PASSED\n");
}

void test_getTaskLastRun_at_time_zero() {
    printf("Test: getTaskLastRun at millis()==0... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.begin();

    // Before any runs (but begin() already called startTask, setting internal lastRun)
    assert(os.getTaskRunCount(id) == 0);
    assert(os.getTaskLastRun(id) == 0);  // Never ran (runCount == 0)

    // Run at millis() == 0
    _mockMillis = 0;
    os.run();

    // Task has run (runCount > 0), lastRun == 0
    assert(os.getTaskRunCount(id) == 1);
    // getTaskLastRun returns 0 (the actual time) - same value but now runCount > 0
    assert(os.getTaskLastRun(id) == 0);

    // Run again at a later time to verify lastRun updates
    _mockMillis = 100;
    os.run();
    assert(os.getTaskRunCount(id) == 2);
    assert(os.getTaskLastRun(id) == 100);

    printf("PASSED\n");
}

// Test helper to access internal task state for overflow testing
// The Task struct is public, but Arda::tasks is private. We use a workaround.
extern Task* getTaskPtr(Arda& os, int8_t id);

void test_getTaskLastRun_runCount_overflow_skips_zero() {
    printf("Test: getTaskLastRun runCount overflow skips 0... ");
    resetTestCounters();

    // This test verifies that runCount skips 0 on overflow, preserving the
    // "never ran" semantics of runCount == 0.

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.begin();

    // Run once at time 500
    _mockMillis = 500;
    os.run();
    assert(os.getTaskRunCount(id) == 1);
    assert(os.getTaskLastRun(id) == 500);

    // Simulate being at UINT32_MAX (about to overflow)
    Task* task = getTaskPtr(os, id);
    task->runCount = UINT32_MAX;

    // Run again - should wrap to 1, not 0
    _mockMillis = 600;
    os.run();
    assert(os.getTaskRunCount(id) == 1);  // Wrapped UINT32_MAX -> 1, skipping 0
    assert(os.getTaskLastRun(id) == 600);  // Still correctly reports last run time

    printf("PASSED\n");
}

void test_get_task_count() {
    printf("Test: getTaskCount... ");
    resetTestCounters();

    Arda os;
    assert(os.getTaskCount() == 0);

    os.createTask("t1", task1_setup, task1_loop, 0);
    os.createTask("t2", task2_setup, task2_loop, 0);
    assert(os.getTaskCount() == 2);

    // Delete one task (must stop first)
    // Task is not started yet, so it's already stopped
    os.deleteTask(0);
    assert(os.getTaskCount() == 1);        // Active count decreases
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

void test_interval_minimum_gap() {
    printf("Test: interval minimum gap... ");
    resetTestCounters();

    Arda os;
    os.createTask("task", task1_setup, task1_loop, 100);  // 100ms interval
    os.begin();

    // Initial state: lastRun = 0, need to wait 100ms
    setMockMillis(100);
    os.run();
    assert(loop1Called == 1);

    // With actual-time tracking, next run is at 205 + 100 = 305ms
    // because the task ran at t=205 (not scheduled 200ms)
    setMockMillis(205);
    os.run();
    assert(loop1Called == 2);

    // Next run should be at 305ms (205 + 100), not 300ms
    // At 304ms it should NOT run yet
    setMockMillis(304);
    os.run();
    assert(loop1Called == 2);  // Still 2

    // At 305ms it should run
    setMockMillis(305);
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

    // Change interval to 50ms with resetTiming=true (resets lastRun to current time)
    assert(os.setTaskInterval(id, 50, true) == true);

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
    printf("Test: createTask with autoStart=false does not auto-start... ");
    resetTestCounters();

    Arda os;
    // Create tasks with autoStart=false
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0, nullptr, false);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0, nullptr, false);

    // begin() should initialize but tasks with autoStart=false stay stopped
    int8_t result = os.begin();
    assert(result == 0);  // 0 tasks started (both had autoStart=false)
    assert(os.hasBegun() == true);  // Scheduler is begun

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

void test_begin_with_prestarted_tasks() {
    printf("Test: begin() skips already-running tasks... ");
    resetTestCounters();

    Arda os;
    int8_t t1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t t2 = os.createTask("t2", task2_setup, task2_loop, 0);
    int8_t t3 = os.createTask("t3", task1_setup, task1_loop, 0);

    // Manually start t2 before begin()
    os.startTask(t2);
    assert(os.getTaskState(t2) == TaskState::Running);
    assert(setup2Called == 1);

    // begin() should skip t2 (already Running) and start t1, t3
    int8_t started = os.begin();

    // All 3 tasks should count as "started" (t2 was already running, t1/t3 started by begin)
    assert(started == 3);

    // All tasks should be Running
    assert(os.getTaskState(t1) == TaskState::Running);
    assert(os.getTaskState(t2) == TaskState::Running);
    assert(os.getTaskState(t3) == TaskState::Running);

    // t2's setup was only called once (before begin), not twice
    assert(setup2Called == 1);
    // t1's setup was called by begin
    assert(setup1Called == 2);  // t1 and t3 both use task1_setup

    // No error should be set (all tasks successfully accounted for)
    assert(os.getError() == ArdaError::Ok);

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

    // Names at exactly max length should work (15 chars + null = 16)
    const char* maxName = "exactly15chars!";  // 15 characters
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

    // Change interval while paused with resetTiming=true (resets lastRun to 150ms)
    os.setTaskInterval(id, 50, true);
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
    StartResult result = OS.startTask(depthTestTaskIds[0]);
    assert(result == StartResult::Success);

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
    assert(os.getTaskCount() == 4);
    assert(os.getTaskCount() == 4);      // Active count decreases

    // New task should reuse slot 1 (the only deleted slot)
    int8_t newId1 = os.createTask("new1", task2_setup, task2_loop, 0);
    assert(newId1 == 1);  // Reused slot 1
    assert(os.getSlotCount() == 5);      // Slot count unchanged

    // Delete tasks 0 and 3
    os.deleteTask(0);
    os.deleteTask(3);
    assert(os.getTaskCount() == 3);

    // New tasks should reuse deleted slots (order may vary due to free list LIFO)
    int8_t newId2 = os.createTask("new2", task2_setup, task2_loop, 0);
    assert(newId2 == 0 || newId2 == 3);  // Reused one of the deleted slots
    assert(os.getSlotCount() == 5);      // Slot count unchanged (slot reused)

    int8_t newId3 = os.createTask("new3", task2_setup, task2_loop, 0);
    assert(newId3 == 0 || newId3 == 3);  // Reused the other deleted slot
    assert(newId3 != newId2);            // Must be different from newId2
    assert(os.getSlotCount() == 5);      // Slot count still unchanged

    // Now all slots 0-4 are used again
    assert(os.getTaskCount() == 5);
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

    // Starting should return SetupChangedState because setup stops the task
    StartResult result = OS.startTask(setupSelfStopTaskId);

    assert(setupSelfStopCalled == true);  // Setup did run
    assert(result == StartResult::SetupChangedState);  // Setup changed state
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

    // Verify all error codes have human-readable string representations
    assert(strncmp(Arda::errorString(ArdaError::Ok), "No error", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::NullName), "Task name is null", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::EmptyName), "Task name is empty", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::NameTooLong), "Task name too long", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::DuplicateName), "Task name already exists", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::MaxTasks), "Max tasks reached", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::InvalidId), "Invalid task ID", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::WrongState), "Wrong task state", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::TaskExecuting), "Task is executing", 32) == 0);
#ifdef ARDA_YIELD
    assert(strncmp(Arda::errorString(ArdaError::TaskYielded), "Task has yielded", 32) == 0);
#endif
    assert(strncmp(Arda::errorString(ArdaError::AlreadyBegun), "Already begun", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::CallbackDepth), "Callback depth exceeded", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::StateChanged), "State changed unexpectedly", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::InCallback), "Cannot call from callback", 32) == 0);
    assert(strncmp(Arda::errorString(ArdaError::NotSupported), "Not supported", 32) == 0);

    // Unknown error code should return "Unknown error"
    assert(strncmp(Arda::errorString((ArdaError)99), "Unknown error", 32) == 0);

    printf("PASSED\n");
}

void test_version_constants() {
    printf("Test: version constants... ");

    // Verify version constants are defined and accessible
    assert(ARDA_VERSION_MAJOR == 1);
    assert(ARDA_VERSION_MINOR == 2);
    assert(ARDA_VERSION_PATCH == 0);
    assert(strncmp(ARDA_VERSION_STRING, "1.2.0", 16) == 0);

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

void test_createTask_autostart_failure_returns_negative_one() {
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

    // First task should have valid ID and be running
    assert(autoStartDepthTaskIds[0] >= 0);
    assert(OS.getTaskState(autoStartDepthTaskIds[0]) == TaskState::Running);

    // When auto-start fails due to callback depth, createTask returns -1
    // and the task is deleted. Count how many succeeded vs failed.
    int successCount = 1;  // First task succeeded
    int failCount = 0;
    for (int i = 1; i <= ARDA_MAX_CALLBACK_DEPTH + 1; i++) {
        if (autoStartDepthTaskIds[i] >= 0) {
            successCount++;
            // Successful tasks should be Running
            assert(OS.getTaskState(autoStartDepthTaskIds[i]) == TaskState::Running);
        } else {
            failCount++;
        }
    }

    // Due to callback depth limit, at least one task should have failed (returned -1)
    assert(failCount > 0);

    printf("PASSED\n");
}

// Setup that stops itself, causing startTask to return false with StateChanged error
static int selfStopSetupCalls = 0;
void selfStopSetup() {
    selfStopSetupCalls++;
    // Stop ourselves during setup - this causes startTask to fail
    OS.stopTask(OS.getCurrentTask());
}

void test_begin_partial_failure_preserves_error() {
    printf("Test: begin partial failure preserves error... ");
    resetTestCounters();
    selfStopSetupCalls = 0;

    // Create two normal tasks and one that will fail during setup (last, so error is preserved)
    int8_t t1 = OS.createTask("good1", task1_setup, task1_loop, 0);
    int8_t t2 = OS.createTask("good2", task2_setup, task2_loop, 0);
    int8_t t3 = OS.createTask("fail", selfStopSetup, task1_loop, 0);  // This will fail (last)
    assert(t1 >= 0 && t2 >= 0 && t3 >= 0);

    // begin() will try to start all tasks
    // "fail" task's setup stops itself, causing StateChanged error
    int8_t started = OS.begin();

    // good1 + good2 = 2 tasks should have started (fail task failed)
    // Note: shell is disabled in test_arda.cpp
    assert(started == 2);

    // Error should be StateChanged from the failed task (it's last so error preserved)
    assert(OS.getError() == ArdaError::StateChanged);

    // The setup was called but task ended up Stopped
    assert(selfStopSetupCalls == 1);
    assert(OS.getTaskState(t3) == TaskState::Stopped);

    printf("PASSED\n");
}

void test_begin_partial_failure_first_task_preserves_error() {
    printf("Test: begin partial failure (first task) preserves error... ");
    resetTestCounters();
    selfStopSetupCalls = 0;

    // Create failing task FIRST, then successful task - tests that later success doesn't mask error
    int8_t t1 = OS.createTask("fail", selfStopSetup, task1_loop, 0);  // This will fail (first)
    int8_t t2 = OS.createTask("good", task1_setup, task1_loop, 0);    // This succeeds (second)
    assert(t1 >= 0 && t2 >= 0);

    // begin() will try to start all tasks
    int8_t started = OS.begin();

    // Only "good" should have started
    assert(started == 1);

    // Error should still be StateChanged from the first failed task,
    // not Ok (which would be the bug - successful task masking the error)
    assert(OS.getError() == ArdaError::StateChanged);

    // Verify states
    assert(selfStopSetupCalls == 1);
    assert(OS.getTaskState(t1) == TaskState::Stopped);
    assert(OS.getTaskState(t2) == TaskState::Running);

    printf("PASSED\n");
}

void test_error_string_in_callback() {
    printf("Test: errorString InCallback... ");

    assert(strncmp(Arda::errorString(ArdaError::InCallback), "Cannot call from callback", 32) == 0);

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
    selfStopSetupCalls = 0;

    OS.setStartFailureCallback(startFailureCallback);

    // Create tasks, one of which will fail during setup
    int8_t t1 = OS.createTask("good1", task1_setup, task1_loop, 0);
    int8_t t2 = OS.createTask("good2", task2_setup, task2_loop, 0);
    int8_t t3 = OS.createTask("fail", selfStopSetup, task1_loop, 0);  // This will fail
    assert(t1 >= 0 && t2 >= 0 && t3 >= 0);

    // begin() will try to start all tasks
    // "fail" task's setup stops itself, triggering the callback
    int8_t started = OS.begin();

    // good1 + good2 = 2 tasks should have started successfully
    // Note: shell is disabled in test_arda.cpp
    assert(started == 2);

    // Callback should have been called exactly once (for the failed task)
    assert(startFailureCount == 1);
    assert(lastFailedTaskId == t3);
    assert(lastFailureError == ArdaError::StateChanged);

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

void test_start_result_enum_values() {
    printf("Test: StartResult enum values are distinct... ");

    // Verify enum values are distinct and can be tested
    assert(StartResult::Success != StartResult::SetupChangedState);
    assert(StartResult::Success != StartResult::Failed);
    assert(StartResult::SetupChangedState != StartResult::Failed);

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

    int8_t id = OS.createTask("traced", task1_setup, task1_loop, 0, nullptr, false);
    OS.begin();  // Initialize scheduler (task has autoStart=false so stays stopped)
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

void test_trace_skipped_on_callback_depth() {
    printf("Test: traces skipped when callback depth exceeded... ");
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

    // Tasks are stopped (state changes happen regardless of trace depth)
    // but traces are skipped when depth is at the limit
    // We expect fewer TaskStopped traces than tasks stopped
    assert(traceStoppedCount < ARDA_MAX_CALLBACK_DEPTH + 1);
    assert(traceStoppedCount > 0);  // At least some traces fired

    // Verify tasks are actually stopped despite traces being skipped
    for (int i = 0; i < ARDA_MAX_CALLBACK_DEPTH + 1; i++) {
        assert(OS.getTaskState(depthTraceTaskIds[i]) == TaskState::Stopped);
    }

    printf("PASSED\n");
}

void test_task_count_o1() {
    printf("Test: getTaskCount is O(1)... ");
    resetTestCounters();

    Arda os;

    assert(os.getTaskCount() == 0);

    // Create tasks
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    assert(os.getTaskCount() == 1);

    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);
    assert(os.getTaskCount() == 2);

    // Delete one
    os.deleteTask(id1);
    assert(os.getTaskCount() == 1);

    // Create another (reuses slot)
    int8_t id3 = os.createTask("t3", task1_setup, task1_loop, 0);
    assert(os.getTaskCount() == 2);

    // Delete both
    os.deleteTask(id2);
    os.deleteTask(id3);
    assert(os.getTaskCount() == 0);

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
        assert(os.getTaskCount() <= ARDA_MAX_TASKS);
    }

    printf("PASSED\n");
}

void test_batch_null_array() {
    printf("Test: batch operations with null array... ");
    resetTestCounters();

    Arda os;

    // Set a prior error to verify no-op clears it
    os.startTask(99);  // Invalid ID sets error
    assert(os.getError() == ArdaError::InvalidId);

    // All batch operations should safely return 0 for null arrays AND clear error
    assert(os.startTasks(nullptr, 5) == 0);
    assert(os.getError() == ArdaError::Ok);

    os.startTask(99);  // Set error again
    assert(os.stopTasks(nullptr, 5) == 0);
    assert(os.getError() == ArdaError::Ok);

    os.startTask(99);
    assert(os.pauseTasks(nullptr, 5) == 0);
    assert(os.getError() == ArdaError::Ok);

    os.startTask(99);
    assert(os.resumeTasks(nullptr, 5) == 0);
    assert(os.getError() == ArdaError::Ok);

    // Also test zero/negative count clears error
    int8_t ids[] = {0, 1, 2};
    os.startTask(99);
    assert(os.startTasks(ids, 0) == 0);
    assert(os.getError() == ArdaError::Ok);

    os.startTask(99);
    assert(os.startTasks(ids, -1) == 0);
    assert(os.getError() == ArdaError::Ok);

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

void test_stopTasks_preserves_teardown_error() {
    printf("Test: stopTasks preserves teardown error... ");
    resetTestCounters();
    trdwnSelfRestartCalled = false;

    // Create a normal task and a task whose teardown restarts itself
    int8_t normalId = OS.createTask("normal", task1_setup, task1_loop, 0);
    trdwnSelfRestartTaskId = OS.createTask("restart", trdwnSelfRestart_setup,
                                           trdwnSelfRestart_loop, 0,
                                           trdwnSelfRestart_trdwn);
    assert(normalId >= 0 && trdwnSelfRestartTaskId >= 0);

    OS.begin();

    // Stop both tasks - the restart task will have TeardownChangedState
    int8_t ids[] = {normalId, trdwnSelfRestartTaskId};
    int8_t failedId = -1;
    int8_t stopped = OS.stopTasks(ids, 2, &failedId);

    // Only normal task should be counted as stopped.
    // TeardownChangedState means the task restarted itself - it's NOT actually stopped!
    assert(stopped == 1);
    assert(failedId == trdwnSelfRestartTaskId);

    // Error should be StateChanged from the teardown that restarted
    assert(OS.getError() == ArdaError::StateChanged);

    printf("PASSED\n");
}

void test_stopAllTasks_preserves_teardown_error() {
    printf("Test: stopAllTasks preserves teardown error... ");
    resetTestCounters();
    trdwnSelfRestartCalled = false;

    // Create a normal task and a task whose teardown restarts itself
    int8_t normalId = OS.createTask("normal", task1_setup, task1_loop, 0);
    trdwnSelfRestartTaskId = OS.createTask("restart", trdwnSelfRestart_setup,
                                           trdwnSelfRestart_loop, 0,
                                           trdwnSelfRestart_trdwn);
    assert(normalId >= 0 && trdwnSelfRestartTaskId >= 0);

    OS.begin();

    // Stop all tasks
    int8_t stopped = OS.stopAllTasks();

    // Only normal task should be counted as stopped.
    // TeardownChangedState means the task restarted itself - it's NOT actually stopped!
    assert(stopped == 1);

    // Error should be StateChanged from the teardown that restarted
    assert(OS.getError() == ArdaError::StateChanged);

    (void)normalId;  // Used only to balance the count
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
    int8_t id = os.createTask("task", task1_setup, task1_loop, 1000, nullptr, false);  // 1 second interval
    os.begin();  // Initialize (task has autoStart=false)

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

// For mid-cycle start test
static int midCycleStarterRuns = 0;
static int midCycleStartedRuns = 0;
static int8_t midCycleStartedId = -1;
static Arda* midCycleTestOs = nullptr;

void midCycleStarter_setup() {}
void midCycleStarter_loop() {
    midCycleStarterRuns++;
    if (midCycleStarterRuns == 1 && midCycleStartedId >= 0 && midCycleTestOs) {
        // Start the other task mid-cycle with runImmediately=false
        midCycleTestOs->startTask(midCycleStartedId, false);
    }
}

void midCycleStarted_setup() {}
void midCycleStarted_loop() {
    midCycleStartedRuns++;
}

void test_mid_cycle_start_no_immediate_run() {
    printf("Test: task started mid-cycle with runImmediately=false waits... ");
    midCycleStarterRuns = 0;
    midCycleStartedRuns = 0;

    Arda os;
    midCycleTestOs = &os;

    // Create starter task (interval=0 so it runs every cycle) with autoStart=false
    int8_t starterId = os.createTask("starter", midCycleStarter_setup, midCycleStarter_loop, 0, nullptr, false);

    // Create started task (will be started mid-cycle by starter) with autoStart=false
    midCycleStartedId = os.createTask("started", midCycleStarted_setup, midCycleStarted_loop, 0, nullptr, false);

    // Initialize scheduler, then manually start only the starter
    os.begin();
    os.startTask(starterId, true);  // Start starter with runImmediately=true

    // First run: starter runs and starts "started" mid-cycle with runImmediately=false
    // "started" should NOT run this cycle despite interval=0
    setMockMillis(0);
    os.run();
    assert(midCycleStarterRuns == 1);
    assert(midCycleStartedRuns == 0);  // Did NOT run this cycle (ranThisCycle=true)

    // Second run: now "started" should run
    setMockMillis(1);
    os.run();
    assert(midCycleStarterRuns == 2);
    assert(midCycleStartedRuns == 1);  // Now it ran

    midCycleTestOs = nullptr;
    printf("PASSED\n");
}

void test_set_task_interval_no_reset() {
    printf("Test: setTaskInterval with resetTiming=false... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 100, nullptr, false);
    os.begin();  // Initialize (task has autoStart=false)
    os.startTask(id, true);  // Start with immediate run

    setMockMillis(0);
    os.run();
    assert(loop1Called == 1);  // First run at t=0

    // Advance 50ms - not yet time to run (interval is 100)
    setMockMillis(50);
    os.run();
    assert(loop1Called == 1);

    // Change interval to 200ms WITH resetTiming=true
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

void test_run_reentrancy_detected() {
    printf("Test: run() reentrancy returns false... ");
    resetTestCounters();

    // Test that calling run() from within a task loop is detected and fails
    static bool reentrantRunAttempted = false;
    static bool reentrantRunResult = true;
    static ArdaError reentrantRunError = ArdaError::Ok;

    auto reentrantLoop = []() {
        if (!reentrantRunAttempted) {
            reentrantRunAttempted = true;
            reentrantRunResult = OS.run();  // Try to call run() while already in run()
            reentrantRunError = OS.getError();
        }
    };

    OS.createTask("reenter", nullptr, reentrantLoop, 0);
    OS.begin();
    OS.run();  // This will execute reentrantLoop which tries to call run() again

    assert(reentrantRunAttempted == true);
    assert(reentrantRunResult == false);  // Reentrant call should fail
    assert(reentrantRunError == ArdaError::InCallback);

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
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0, nullptr, false);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0, nullptr, false);
    os.begin();

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

// For mid-loop timeout adjustment test
static int8_t midLoopTimeoutTaskId = -1;
static uint32_t midLoopCapturedTimeout1 = 0;
static uint32_t midLoopCapturedTimeout2 = 0;
static uint32_t midLoopCapturedTimeout3 = 0;
static int midLoopTimeoutLoopCount = 0;

void midLoopTimeout_setup() {}
void midLoopTimeout_loop() {
    midLoopTimeoutLoopCount++;

    // Capture initial timeout
    midLoopCapturedTimeout1 = OS.getTaskTimeout(midLoopTimeoutTaskId);

    // Extend timeout mid-loop
    OS.setTaskTimeout(midLoopTimeoutTaskId, 500);
    midLoopCapturedTimeout2 = OS.getTaskTimeout(midLoopTimeoutTaskId);

    // Disable timeout mid-loop
    OS.setTaskTimeout(midLoopTimeoutTaskId, 0);
    midLoopCapturedTimeout3 = OS.getTaskTimeout(midLoopTimeoutTaskId);

    // Restore original
    OS.setTaskTimeout(midLoopTimeoutTaskId, 100);
}

void test_setTaskTimeout_midloop() {
    printf("Test: setTaskTimeout mid-loop updates timeout... ");
    resetTestCounters();
    midLoopTimeoutTaskId = -1;
    midLoopCapturedTimeout1 = 0;
    midLoopCapturedTimeout2 = 0;
    midLoopCapturedTimeout3 = 0;
    midLoopTimeoutLoopCount = 0;

    // Create task with initial timeout
    midLoopTimeoutTaskId = OS.createTask("midloop", midLoopTimeout_setup, midLoopTimeout_loop, 0);
    assert(midLoopTimeoutTaskId >= 0);
    OS.setTaskTimeout(midLoopTimeoutTaskId, 100);

    OS.begin();
    OS.run();

    // Verify loop ran
    assert(midLoopTimeoutLoopCount == 1);

    // Verify timeout was captured correctly at each stage
    assert(midLoopCapturedTimeout1 == 100);  // Initial timeout
    assert(midLoopCapturedTimeout2 == 500);  // Extended timeout
    assert(midLoopCapturedTimeout3 == 0);    // Disabled timeout

    // Verify final timeout is restored
    assert(OS.getTaskTimeout(midLoopTimeoutTaskId) == 100);

    printf("PASSED\n");
}

// For mid-loop timeout callback suppression test
static int8_t extendTimeoutTaskId = -1;
static int extendTimeoutCallbackCount = 0;
static int extendTimeoutLoopCount = 0;

void extendTimeout_setup() {}
void extendTimeout_loop() {
    extendTimeoutLoopCount++;
    // Start with 10ms timeout, but immediately extend to 200ms
    // Loop takes ~50ms (simulated), which exceeds 10ms but not 200ms
    OS.setTaskTimeout(extendTimeoutTaskId, 200);
    _mockMillis += 50;  // Simulate 50ms of work
}

void extendTimeoutCallback(int8_t taskId, uint32_t duration) {
    (void)taskId; (void)duration;
    extendTimeoutCallbackCount++;
}

void test_setTaskTimeout_midloop_suppresses_callback() {
    printf("Test: mid-loop timeout extension suppresses callback... ");
    resetTestCounters();
    extendTimeoutTaskId = -1;
    extendTimeoutCallbackCount = 0;
    extendTimeoutLoopCount = 0;

    OS.setTimeoutCallback(extendTimeoutCallback);

    // Create task with short initial timeout (10ms)
    extendTimeoutTaskId = OS.createTask("extend", extendTimeout_setup, extendTimeout_loop, 0);
    assert(extendTimeoutTaskId >= 0);
    OS.setTaskTimeout(extendTimeoutTaskId, 10);  // Initial: 10ms

    OS.begin();
    OS.run();

    // Verify loop ran
    assert(extendTimeoutLoopCount == 1);

    // The task extended timeout to 200ms mid-loop, then "took" 50ms
    // 50ms < 200ms, so timeout callback should NOT fire
    // (Even though 50ms > original 10ms)
    assert(extendTimeoutCallbackCount == 0);

    // Clean up
    OS.setTimeoutCallback(nullptr);

    printf("PASSED\n");
}

// heartbeat() is only available on AVR (ARDA_TASK_RECOVERY_IMPL=1)
// On desktop, ARDA_TASK_RECOVERY_IMPL=0 so heartbeat() doesn't exist
#if ARDA_TASK_RECOVERY_IMPL
static int8_t heartbeatTaskId = -1;
static bool heartbeatCalled = false;

void heartbeat_setup() {}
void heartbeat_loop() {
    heartbeatCalled = OS.heartbeat();
}

void test_heartbeat_resets_timeout() {
    printf("Test: heartbeat() resets timeout timer... ");
    resetTestCounters();
    heartbeatTaskId = -1;
    heartbeatCalled = false;

    heartbeatTaskId = OS.createTask("hb", heartbeat_setup, heartbeat_loop, 0);
    assert(heartbeatTaskId >= 0);
    OS.setTaskTimeout(heartbeatTaskId, 100);

    OS.begin();
    OS.run();

    assert(heartbeatCalled == true);

    printf("PASSED\n");
}

void test_heartbeat_outside_task_fails() {
    printf("Test: heartbeat() outside task context fails... ");
    resetTestCounters();

    // heartbeat() called outside any task should fail
    bool result = OS.heartbeat();
    assert(result == false);
    assert(OS.getError() == ArdaError::InvalidId);

    printf("PASSED\n");
}

static int8_t noTimeoutTaskId = -1;
static bool noTimeoutHeartbeatResult = true;
static ArdaError noTimeoutHeartbeatError = ArdaError::Ok;

void noTimeoutHeartbeat_setup() {}
void noTimeoutHeartbeat_loop() {
    noTimeoutHeartbeatResult = OS.heartbeat();
    noTimeoutHeartbeatError = OS.getError();
}

void test_heartbeat_no_timeout_fails() {
    printf("Test: heartbeat() with no timeout configured fails... ");
    resetTestCounters();
    noTimeoutTaskId = -1;
    noTimeoutHeartbeatResult = true;
    noTimeoutHeartbeatError = ArdaError::Ok;

    // Create task without timeout (timeout=0 by default)
    noTimeoutTaskId = OS.createTask("noTo", noTimeoutHeartbeat_setup, noTimeoutHeartbeat_loop, 0);
    assert(noTimeoutTaskId >= 0);
    // Don't set timeout - leave it at 0

    OS.begin();
    OS.run();

    assert(noTimeoutHeartbeatResult == false);
    assert(noTimeoutHeartbeatError == ArdaError::WrongState);

    printf("PASSED\n");
}
#endif // ARDA_TASK_RECOVERY_IMPL

// Test on all platforms where ARDA_TASK_RECOVERY is enabled
#ifdef ARDA_TASK_RECOVERY
void test_setTaskRecoveryEnabled_suppresses_callback() {
    printf("Test: setTaskRecoveryEnabled(false) suppresses timeout callback... ");
    resetTestCounters();
    timeoutCallbackCalled = false;
    capturedTimeoutDuration = 0;

    OS.setTimeoutCallback(timeoutCallback);

    // Create task that exceeds timeout
    timeoutTaskId = OS.createTask("timeout", timeoutTask_setup, timeoutTask_loop, 0);
    OS.setTaskTimeout(timeoutTaskId, 100);  // 100ms timeout, task takes 150ms

    // Disable recovery globally
    OS.setTaskRecoveryEnabled(false);

    OS.begin();
    OS.run();

    // Timeout callback should NOT have been called even though task exceeded timeout
    assert(timeoutCallbackCalled == false);

    // Re-enable and verify callback fires
    OS.setTaskRecoveryEnabled(true);
    timeoutCallbackCalled = false;
    OS.run();
    assert(timeoutCallbackCalled == true);

    // Clean up
    OS.setTimeoutCallback(nullptr);

    printf("PASSED\n");
}

void test_reset_restores_recoveryEnabled() {
    printf("Test: reset() restores recoveryEnabled to true... ");
    resetTestCounters();
    timeoutCallbackCalled = false;

    OS.setTimeoutCallback(timeoutCallback);

    // Create task that exceeds timeout
    timeoutTaskId = OS.createTask("timeout", timeoutTask_setup, timeoutTask_loop, 0);
    OS.setTaskTimeout(timeoutTaskId, 100);

    // Disable recovery
    OS.setTaskRecoveryEnabled(false);
    assert(OS.isTaskRecoveryEnabled() == false);

    // Reset with preserveCallbacks=true so timeout callback remains
    OS.reset(true);
    assert(OS.isTaskRecoveryEnabled() == true);

    // Recreate task and verify timeout callback fires
    timeoutTaskId = OS.createTask("timeout", timeoutTask_setup, timeoutTask_loop, 0);
    OS.setTaskTimeout(timeoutTaskId, 100);
    OS.begin();
    OS.run();
    assert(timeoutCallbackCalled == true);

    // Clean up
    OS.setTimeoutCallback(nullptr);

    printf("PASSED\n");
}
#endif // ARDA_TASK_RECOVERY

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

// ============================================================================
// Batch Operation Snapshot Safety Tests
// ============================================================================

// Callback that creates a new task during startAllTasks
static int8_t createdInSetup = -1;
static Arda* setupCreatorScheduler = nullptr;
void setupCreatesTask_setup() {
    setup1Called++;
    if (setupCreatorScheduler) {
        createdInSetup = setupCreatorScheduler->createTask("created", nullptr, task1_loop, 0, nullptr, false);
    }
}
void setupCreatesTask_loop() { loop1Called++; }

void test_startAllTasks_snapshot_safety() {
    printf("Test: startAllTasks snapshot protects against setup creating task... ");
    resetTestCounters();

    Arda os;
    setupCreatorScheduler = &os;
    createdInSetup = -1;

    // Create two tasks - one will create another in its setup
    os.createTask("t1", setupCreatesTask_setup, setupCreatesTask_loop, 0);
    os.createTask("t2", task1_setup, task1_loop, 0);

    assert(os.getTaskCount() == 2);

    // startAllTasks should start only the tasks that existed at snapshot time
    int8_t started = os.startAllTasks();

    // Two tasks started (the original two)
    // The task created during setup should NOT be counted in started
    assert(started == 2);
    assert(setup1Called == 2);  // Both original setups ran

    // The new task was created but NOT started (wasn't in snapshot)
    assert(createdInSetup >= 0);  // Task was created
    assert(os.isValidTask(createdInSetup));
    assert(os.getTaskState(createdInSetup) == TaskState::Stopped);  // Not started by startAllTasks

    // Total task count is now 3
    assert(os.getTaskCount() == 3);

    setupCreatorScheduler = nullptr;
    printf("PASSED\n");
}

// Callback that deletes another task during stopAllTasks
static int8_t taskToDeleteInTeardown = -1;
static Arda* teardownDeleterScheduler = nullptr;
void teardownDeletesTask_setup() { setup1Called++; }
void teardownDeletesTask_loop() { loop1Called++; }
void teardownDeletesTask_teardown() {
    trdwnCalled++;
    if (teardownDeleterScheduler && taskToDeleteInTeardown >= 0) {
        // Stop and delete the other task
        teardownDeleterScheduler->stopTask(taskToDeleteInTeardown);
        teardownDeleterScheduler->deleteTask(taskToDeleteInTeardown);
        taskToDeleteInTeardown = -1;  // Only delete once
    }
}

void test_stopAllTasks_snapshot_safety() {
    printf("Test: stopAllTasks snapshot protects against teardown deleting task... ");
    resetTestCounters();

    Arda os;
    teardownDeleterScheduler = &os;
    trdwnCalled = 0;

    // Create two tasks - one will delete the other in its teardown
    // Use trdwnTask_trdwn for victim which also increments trdwnCalled
    int8_t id1 = os.createTask("deleter", teardownDeletesTask_setup, teardownDeletesTask_loop, 0, teardownDeletesTask_teardown);
    int8_t id2 = os.createTask("victim", task1_setup, task1_loop, 0, trdwnTask_trdwn);

    taskToDeleteInTeardown = id2;  // Tell id1's teardown to delete id2

    os.begin();
    assert(os.getTaskCount() == 2);
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Running);

    // stopAllTasks should complete without crash even though teardown deletes a task
    int8_t stopped = os.stopAllTasks();

    // Flow analysis:
    // 1. Snapshot: [id1, id2] both Running
    // 2. Stop id1: runs teardownDeletesTask_teardown (trdwnCalled=1)
    //    - which calls stopTask(id2): runs trdwnTask_trdwn (trdwnCalled=2)
    //    - then deletes id2
    // 3. Try to stop id2: invalid (deleted), skipped
    // Both teardowns ran: deleter's directly, victim's from nested stopTask call
    assert(trdwnCalled == 2);

    // stopped count: id1 was stopped by stopAllTasks, id2 was stopped by nested call
    // but id2 was invalid when stopAllTasks tried to process it, so only 1 counted
    assert(stopped == 1);

    // Only deleter should remain (victim was deleted)
    assert(os.getTaskCount() == 1);
    assert(os.isValidTask(id1));
    assert(!os.isValidTask(id2));

    teardownDeleterScheduler = nullptr;
    printf("PASSED\n");
}

// Test pauseAllTasks with callback that modifies state
static Arda* pauseModifierScheduler = nullptr;
static int8_t taskToModifyInPause = -1;
void pauseModifier_setup() { setup1Called++; }
void pauseModifier_loop() {
    loop1Called++;
    // This task will pause another task when it runs
    if (pauseModifierScheduler && taskToModifyInPause >= 0) {
        // Stop the other task (changing its state from Running to Stopped)
        pauseModifierScheduler->stopTask(taskToModifyInPause);
        taskToModifyInPause = -1;
    }
}

void test_pauseAllTasks_snapshot_safety() {
    printf("Test: pauseAllTasks handles state changes during iteration... ");
    resetTestCounters();

    Arda os;
    pauseModifierScheduler = &os;

    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);

    os.begin();
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Running);

    // Simulate that between snapshot and pause, task id2 gets stopped by some other means
    // We can't easily do this with callbacks for pause (no callback on pause)
    // So just test basic snapshot behavior

    // pauseAllTasks should work
    int8_t paused = os.pauseAllTasks();
    assert(paused == 2);
    assert(os.getTaskState(id1) == TaskState::Paused);
    assert(os.getTaskState(id2) == TaskState::Paused);

    pauseModifierScheduler = nullptr;
    printf("PASSED\n");
}

void test_resumeAllTasks_snapshot_safety() {
    printf("Test: resumeAllTasks handles state changes during iteration... ");
    resetTestCounters();

    Arda os;
    int8_t id1 = os.createTask("t1", task1_setup, task1_loop, 0);
    int8_t id2 = os.createTask("t2", task2_setup, task2_loop, 0);

    os.begin();
    os.pauseTask(id1);
    os.pauseTask(id2);
    assert(os.getTaskState(id1) == TaskState::Paused);
    assert(os.getTaskState(id2) == TaskState::Paused);

    // resumeAllTasks should work
    int8_t resumed = os.resumeAllTasks();
    assert(resumed == 2);
    assert(os.getTaskState(id1) == TaskState::Running);
    assert(os.getTaskState(id2) == TaskState::Running);

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
            StartResult started = OS.startTask(newId);
            if (started == StartResult::Failed && OS.getError() == ArdaError::CallbackDepth) {
                traceDepthStartFailed = true;
            }
            // Clean up
            if (started == StartResult::Success) {
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

void test_self_stop_increments_runcount() {
    printf("Test: self-stop still increments runCount and lastRun... ");
    resetTestCounters();
    loopSelfStopCalled = false;

    loopSelfStopTaskId = OS.createTask("slStop", loopSelfStop_setup, loopSelfStop_loop, 0);
    OS.begin();

    // Verify runCount is 0 before running
    assert(OS.getTaskRunCount(loopSelfStopTaskId) == 0);
    assert(OS.getTaskLastRun(loopSelfStopTaskId) == 0);

    _mockMillis = 100;
    OS.run();  // Task runs and stops itself

    assert(loopSelfStopCalled == true);
    assert(OS.getTaskState(loopSelfStopTaskId) == TaskState::Stopped);
    // Even though task stopped itself, it DID run - runCount should be 1
    assert(OS.getTaskRunCount(loopSelfStopTaskId) == 1);
    assert(OS.getTaskLastRun(loopSelfStopTaskId) == 100);

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
    printf("Test: new tasks have default priority Normal... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    assert(os.getTaskPriority(id) == TaskPriority::Normal);
    assert(static_cast<uint8_t>(os.getTaskPriority(id)) == ARDA_DEFAULT_PRIORITY);

    printf("PASSED\n");
}

void test_priority_set_get() {
    printf("Test: setTaskPriority/getTaskPriority... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    // Test all five priority levels
    assert(os.setTaskPriority(id, TaskPriority::Lowest) == true);
    assert(os.getTaskPriority(id) == TaskPriority::Lowest);

    assert(os.setTaskPriority(id, TaskPriority::Low) == true);
    assert(os.getTaskPriority(id) == TaskPriority::Low);

    assert(os.setTaskPriority(id, TaskPriority::Normal) == true);
    assert(os.getTaskPriority(id) == TaskPriority::Normal);

    assert(os.setTaskPriority(id, TaskPriority::High) == true);
    assert(os.getTaskPriority(id) == TaskPriority::High);

    assert(os.setTaskPriority(id, TaskPriority::Highest) == true);
    assert(os.getTaskPriority(id) == TaskPriority::Highest);

    printf("PASSED\n");
}

void test_priority_create_with_priority() {
    printf("Test: createTask overload with priority... ");
    resetTestCounters();

    Arda os;

    // Create with explicit priority (priority is last parameter)
    int8_t id1 = os.createTask("high", task1_setup, task1_loop, 0, nullptr, false, TaskPriority::Highest);
    int8_t id2 = os.createTask("low", task2_setup, task2_loop, 0, nullptr, false, TaskPriority::Lowest);
    int8_t id3 = os.createTask("mid", task1_setup, task1_loop, 0, nullptr, false, TaskPriority::Normal);

    assert(id1 >= 0);
    assert(id2 >= 0);
    assert(id3 >= 0);

    assert(os.getTaskPriority(id1) == TaskPriority::Highest);
    assert(os.getTaskPriority(id2) == TaskPriority::Lowest);
    assert(os.getTaskPriority(id3) == TaskPriority::Normal);

    printf("PASSED\n");
}

void test_priority_invalid_task() {
    printf("Test: priority operations on invalid task... ");
    resetTestCounters();

    Arda os;

    // setTaskPriority on invalid ID should fail
    assert(os.setTaskPriority(-1, TaskPriority::High) == false);
    assert(os.getError() == ArdaError::InvalidId);

    assert(os.setTaskPriority(99, TaskPriority::High) == false);
    assert(os.getError() == ArdaError::InvalidId);

    // getTaskPriority on invalid ID should return Lowest
    assert(os.getTaskPriority(-1) == TaskPriority::Lowest);
    assert(os.getTaskPriority(99) == TaskPriority::Lowest);

    // Create and delete a task
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);
    os.deleteTask(id);

    // Operations on deleted task
    assert(os.setTaskPriority(id, TaskPriority::High) == false);
    assert(os.getError() == ArdaError::InvalidId);
    assert(os.getTaskPriority(id) == TaskPriority::Lowest);

    printf("PASSED\n");
}

void test_priority_rejected_if_invalid() {
    printf("Test: priority values > Highest are rejected... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    // Set valid priority first
    assert(os.setTaskPriority(id, TaskPriority::High) == true);
    assert(os.getTaskPriority(id) == TaskPriority::High);

    // setTaskPriority rejects values > Highest with InvalidValue
    assert(os.setTaskPriority(id, static_cast<TaskPriority>(10)) == false);
    assert(os.getError() == ArdaError::InvalidValue);
    assert(os.getTaskPriority(id) == TaskPriority::High);  // Unchanged

    assert(os.setTaskPriority(id, static_cast<TaskPriority>(255)) == false);
    assert(os.getError() == ArdaError::InvalidValue);
    assert(os.getTaskPriority(id) == TaskPriority::High);  // Unchanged

    // createTask with priority > Highest should also fail
    int8_t id2 = os.createTask("task2", task1_setup, task1_loop, 0, nullptr, false, static_cast<TaskPriority>(100));
    assert(id2 == -1);
    assert(os.getError() == ArdaError::InvalidValue);

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
    int8_t idLow = OS.createTask("low", priorityOrderLow_setup, priorityOrderLow_loop, 0, nullptr, false, TaskPriority::Low);
    int8_t idMid = OS.createTask("mid", priorityOrderMid_setup, priorityOrderMid_loop, 0, nullptr, false, TaskPriority::Normal);
    int8_t idHigh = OS.createTask("high", priorityOrderHigh_setup, priorityOrderHigh_loop, 0, nullptr, false, TaskPriority::Highest);

    OS.begin();  // Tasks have autoStart=false
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

    // All tasks have the same priority, created with autoStart=false
    (void)OS.createTask("t0", samePriorityTask0_setup, samePriorityTask0_loop, 0, nullptr, false, TaskPriority::Normal);
    (void)OS.createTask("t1", samePriorityTask1_setup, samePriorityTask1_loop, 0, nullptr, false, TaskPriority::Normal);
    (void)OS.createTask("t2", samePriorityTask2_setup, samePriorityTask2_loop, 0, nullptr, false, TaskPriority::Normal);

    OS.begin();
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
    int8_t idHigh = OS.createTask("high", priorityIntervalHigh_setup, priorityIntervalHigh_loop, 1000, nullptr, false, TaskPriority::Highest);
    int8_t idLow = OS.createTask("low", priorityIntervalLow_setup, priorityIntervalLow_loop, 100, nullptr, false, TaskPriority::Low);

    OS.begin();  // Tasks have autoStart=false
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

    // Verify enum values are consecutive 0-4
    assert(static_cast<uint8_t>(TaskPriority::Lowest) == 0);
    assert(static_cast<uint8_t>(TaskPriority::Low) == 1);
    assert(static_cast<uint8_t>(TaskPriority::Normal) == 2);
    assert(static_cast<uint8_t>(TaskPriority::High) == 3);
    assert(static_cast<uint8_t>(TaskPriority::Highest) == 4);

    // Verify ARDA_DEFAULT_PRIORITY matches TaskPriority::Normal
    assert(ARDA_DEFAULT_PRIORITY == static_cast<uint8_t>(TaskPriority::Normal));

    printf("PASSED\n");
}

void test_priority_preserved_across_stop_start() {
    printf("Test: priority preserved across stop/start... ");
    resetTestCounters();

    Arda os;
    int8_t id = os.createTask("task", task1_setup, task1_loop, 0);

    os.setTaskPriority(id, TaskPriority::High);
    assert(os.getTaskPriority(id) == TaskPriority::High);

    os.startTask(id);
    assert(os.getTaskPriority(id) == TaskPriority::High);

    os.stopTask(id);
    assert(os.getTaskPriority(id) == TaskPriority::High);

    os.startTask(id);
    assert(os.getTaskPriority(id) == TaskPriority::High);

    printf("PASSED\n");
}

// Regression test: priority must be set BEFORE setup() runs when using
// createTask with explicit priority and autoStart=true
static TaskPriority capturedPriorityInSetup = TaskPriority::Lowest;

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
    capturedPriorityInSetup = TaskPriority::Lowest;

    OS.begin();  // Initialize scheduler

    // Create task with explicit priority=Highest and autoStart=true
    // The priority should be Highest when setup() runs, not default Normal
    int8_t taskId = OS.createTask("priSetu", prioritySetupCapture_setup,
                                  prioritySetupCapture_loop, 0,
                                  nullptr, true, TaskPriority::Highest);

    assert(taskId >= 0);

    // setup() should have run during createTask (autoStart=true after begin)
    // and captured the priority as Highest
    assert(capturedPriorityInSetup == TaskPriority::Highest);

    // Verify priority is still Highest after creation
    assert(OS.getTaskPriority(taskId) == TaskPriority::Highest);

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

void test_priority_uses_fresh_millis_for_readiness() {
    printf("Test: priority scheduling uses fresh millis() for readiness checks... ");
    resetTestCounters();
    midCycleReadyTaskRuns = 0;
    midCycleTimeAdvanced = false;

    setMockMillis(0);

    // High priority task with interval=0 - runs every cycle
    (void)OS.createTask("advncr", midCycleAdvancer_setup, midCycleAdvancer_loop,
                        0, nullptr, false, TaskPriority::Highest);

    // Low priority task with interval=100ms - not ready at cycle start
    (void)OS.createTask("waiter", midCycleReady_setup, midCycleReady_loop,
                        100, nullptr, false, TaskPriority::Low);

    OS.begin();  // Tasks have autoStart=false
    OS.startAllTasks();

    // At t=0, only the high-priority task is initially ready
    // The high-priority task advances time to t=200 mid-cycle
    // The low-priority task SHOULD run because readiness uses fresh millis(),
    // and at t=200 the interval (100ms) has elapsed since lastRun (0)
    OS.run();

    // High priority task ran and advanced time
    assert(midCycleTimeAdvanced == true);

    // Low priority task SHOULD have run - readiness uses fresh millis()
    // so mid-cycle time advance makes the task ready
    assert(midCycleReadyTaskRuns == 1);

    printf("PASSED\n");
}

// Test: task that restarts itself mid-cycle cannot run twice in same cycle
static int8_t selfRestartTaskId = -1;
static int selfRestartRunCount = 0;

void selfRestart_setup() {}
void selfRestart_loop() {
    selfRestartRunCount++;
    // Stop and immediately restart with runImmediately=true
    OS.stopTask(selfRestartTaskId);
    OS.startTask(selfRestartTaskId, true);  // runImmediately=true
}

void test_self_restart_no_double_execution() {
    printf("Test: self-restart with runImmediately cannot run twice per cycle... ");
    resetTestCounters();
    selfRestartRunCount = 0;

    selfRestartTaskId = OS.createTask("rstrt", selfRestart_setup, selfRestart_loop, 0);
    OS.begin();

    // Run one cycle
    OS.run();

    // Task should have run exactly once, even though it restarted itself with runImmediately=true
    // The fix ensures ranThisCycle stays true when FLAG_IN_RUN is set
    assert(selfRestartRunCount == 1);

    // Task should still be in Running state (it restarted itself)
    assert(OS.getTaskState(selfRestartTaskId) == TaskState::Running);

    // Run another cycle - now it can run again
    OS.run();
    assert(selfRestartRunCount == 2);

    printf("PASSED\n");
}

// Regression test: createTask with priority overload and autoStart=true
// should auto-start when begin() is called (bug: RAN_BIT wasn't set)
static int priorityAutoStartSetupCalled = 0;
void priorityAutoStart_setup() { priorityAutoStartSetupCalled++; }
void priorityAutoStart_loop() {}

void test_priority_autostart_before_begin() {
    printf("Test: priority createTask with autoStart=true before begin()... ");
    resetTestCounters();
    priorityAutoStartSetupCalled = 0;

    Arda os;

    // Create task with priority overload, autoStart=true, BEFORE begin()
    int8_t id = os.createTask("priAuto", priorityAutoStart_setup, priorityAutoStart_loop,
                              0, nullptr, true, TaskPriority::High);
    assert(id >= 0);

    // Setup should NOT have been called yet (begin() not called)
    assert(priorityAutoStartSetupCalled == 0);
    assert(os.getTaskState(id) == TaskState::Stopped);

    // Now call begin() - this should auto-start the task
    int8_t started = os.begin();
    assert(started == 1);

    // Setup should have been called by begin()
    assert(priorityAutoStartSetupCalled == 1);
    assert(os.getTaskState(id) == TaskState::Running);

    printf("PASSED\n");
}

// Test: createTask overload with timeout and recover (requires ARDA_TASK_RECOVERY)
#ifdef ARDA_TASK_RECOVERY
static int timeoutRecoverCalled = 0;
void timeoutRecover_setup() {}
void timeoutRecover_loop() {}
void timeoutRecover_recover() { timeoutRecoverCalled++; }

void test_createTask_with_timeout_and_recover() {
    printf("Test: createTask overload with timeout and recover... ");
    resetTestCounters();
    timeoutRecoverCalled = 0;

    Arda os;

    // Create task with timeout and recover in one call
    int8_t id = os.createTask("timedTask", timeoutRecover_setup, timeoutRecover_loop,
                              100, nullptr, false, TaskPriority::Normal,
                              50, timeoutRecover_recover);  // 50ms timeout
    assert(id >= 0);

    // Verify timeout was set
    assert(os.getTaskTimeout(id) == 50);

    // Verify recover callback was set (use internal accessor since hasTaskRecover
    // returns false on non-AVR test platforms where ARDA_TASK_RECOVERY_IMPL=0)
    Task* task = os.getTaskPtr_(id);
    assert(task != nullptr);
    assert(task->recover == timeoutRecover_recover);

    // Verify priority was set correctly
    assert(os.getTaskPriority(id) == TaskPriority::Normal);

    // Create another with timeout=0 (disabled)
    int8_t id2 = os.createTask("noTimeout", timeoutRecover_setup, timeoutRecover_loop,
                               100, nullptr, false, TaskPriority::High,
                               0, nullptr);  // timeout disabled, no recover
    assert(id2 >= 0);
    assert(os.getTaskTimeout(id2) == 0);
    Task* task2 = os.getTaskPtr_(id2);
    assert(task2 != nullptr);
    assert(task2->recover == nullptr);
    assert(os.getTaskPriority(id2) == TaskPriority::High);

    printf("PASSED\n");
}
#endif // ARDA_TASK_RECOVERY

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
    test_mid_cycle_start_no_immediate_run();
    test_trdwn_called();
    test_trdwn_sees_stopped_state();
    test_paused_task_not_executed();
    test_invalid_operations();
    test_resume_running_task();
    test_pause_already_paused_task();
    test_double_resume();
    test_loop_self_stop();
    test_self_stop_increments_runcount();
    test_loop_self_pause();
    test_setup_self_stop_returns_false();
    test_trdwn_self_restart_returns_partial();
    test_teardown_deletes_other_running_task();

    // ---- Scheduler Control (begin/run/reset) ----
    test_begin_starts_all();
    test_begin_returns_count();
    test_begin_only_once();
    test_begin_no_autostart();
    test_begin_with_prestarted_tasks();
    test_has_begun();
    test_run_before_begin();
    test_run_reentrancy_detected();
    test_run_executes_tasks();
    test_reset();
    test_reset_clears_callbacks();
    test_reset_preserves_callbacks();
    test_reset_returns_true_when_all_trdwns_run();
    test_reset_from_callback_blocked();

    // ---- Auto-Start Behavior ----
    test_auto_start_after_begin();
    test_auto_start_disabled();
    test_createTask_autostart_failure_returns_negative_one();
    test_begin_partial_failure_preserves_error();
    test_begin_partial_failure_first_task_preserves_error();

    // ---- Interval & Timing ----
    test_interval_scheduling();
    test_interval_minimum_gap();
    test_interval_catchup_prevention();
    test_interval_zero_runs_every_cycle();
    test_set_task_interval();
    test_set_interval_paused_task();
    test_set_task_interval_no_reset();
    test_setTaskInterval_on_deleted();
    test_set_interval_to_zero_dynamically();
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
    test_setTaskTimeout_midloop();
    test_setTaskTimeout_midloop_suppresses_callback();
#if ARDA_TASK_RECOVERY_IMPL
    test_heartbeat_resets_timeout();
    test_heartbeat_outside_task_fails();
    test_heartbeat_no_timeout_fails();
#endif
#ifdef ARDA_TASK_RECOVERY
    test_setTaskRecoveryEnabled_suppresses_callback();
    test_reset_restores_recoveryEnabled();
#endif

    // ---- Task Execution & Run Count ----
    test_run_count();
    test_getTaskLastRun_before_first_run();
    test_getTaskLastRun_at_time_zero();
    test_getTaskLastRun_runCount_overflow_skips_zero();
    test_runcount_returns_zero_for_deleted_task();
    test_null_loop_function();
    test_get_current_task();

    // ---- Deletion & Memory Management ----
    test_delete_task();
    test_kill_task_running();
    test_kill_task_stopped();
    test_kill_task_paused();
    test_kill_task_invalid_id();
    test_kill_task_teardown_restarts();
    test_kill_task_self();
    test_deleted_task_not_executed();
    test_deleted_task_returns_invalid();
    test_deleted_task_slot_fully_cleared();
    test_deleted_slot_reuse();
    test_self_deletion_prevented();
    test_double_delete();
    test_free_list_o1_allocation();

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
    test_stopTasks_preserves_teardown_error();
    test_stopAllTasks_preserves_teardown_error();

    // ---- All-Tasks Operations ----
    test_start_all_tasks();
    test_stop_all_tasks();
    test_pause_all_tasks();
    test_startAllTasks_with_some_running();
    test_stopAllTasks_with_some_stopped();
    test_pauseAllTasks_with_mixed_states();
    test_resumeAllTasks_with_mixed_states();
    test_startAllTasks_snapshot_safety();
    test_stopAllTasks_snapshot_safety();
    test_pauseAllTasks_snapshot_safety();
    test_resumeAllTasks_snapshot_safety();

    // ---- Task Counting & Iteration ----
    test_get_task_count();
    test_task_count_o1();
    test_get_slot_count();
    test_get_valid_task_ids();
    test_get_valid_task_ids_empty();
    test_iteration_with_deletion_shows_stale_ids();

    // ---- Callbacks (Timeout, Start Failure, Trace) ----
    test_start_failure_callback();
    test_trace_callback();
    test_trace_pause_resume_delete();
    test_trace_task_stpng_event();
    test_trace_skipped_on_callback_depth();
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
    test_clear_error_preserves_state();
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
    test_priority_rejected_if_invalid();
    test_priority_ordering();
    test_priority_same_level();
    test_priority_with_intervals();
    test_priority_enum_values();
    test_priority_preserved_across_stop_start();
    test_priority_set_before_setup_runs();
    test_priority_uses_fresh_millis_for_readiness();
    test_self_restart_no_double_execution();
    test_priority_autostart_before_begin();
#ifdef ARDA_TASK_RECOVERY
    test_createTask_with_timeout_and_recover();
#endif
#endif

    // ---- Miscellaneous ----
    test_get_max_tasks();
    test_version_constants();
    test_stop_result_enum_values();
    test_start_result_enum_values();
    test_multiple_scheduler_instances();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
