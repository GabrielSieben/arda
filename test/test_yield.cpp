// Test for ARDA_YIELD feature
// Build: g++ -std=c++11 -I. -o test_yield test_yield.cpp && ./test_yield
//
// This verifies that:
// 1. yield() API is available when ARDA_YIELD is defined
// 2. isTaskYielded() API is available when ARDA_YIELD is defined
// 3. TaskYielded error code exists when ARDA_YIELD is defined
// 4. Yield functionality works correctly

#include <cstdio>
#include <cstring>
#include <cassert>
#include <new>

// Mock Arduino environment
#include "Arduino.h"
uint32_t _mockMillis = 0;
MockSerial Serial;

// Enable yield and disable shell BEFORE including Arda
#define ARDA_YIELD
#define ARDA_NO_SHELL
#include "../Arda.h"
#include "../Arda.cpp"

// Reset global OS instance to clean state between tests.
void resetGlobalOS() {
    OS.~Arda();
    new (&OS) Arda();
}

static int setup1Called = 0;
static int loop1Called = 0;
static int setup2Called = 0;
static int loop2Called = 0;

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

// For basic yield test - a task that calls yield
static int yieldLoopCalled = 0;
void yldTask_setup() {}
void yldTask_loop() {
    yieldLoopCalled++;
    // This would cause infinite recursion without reentrancy guard
    OS.yield();
}

void test_yield_api_available() {
    printf("Test: yield API is available... ");

    // Verify that ARDA_TASK_YIELD_BIT is defined
#ifndef ARDA_TASK_YIELD_BIT
    #error "ARDA_TASK_YIELD_BIT should be defined when ARDA_YIELD is set"
#endif

    // Verify TaskYielded error exists by using it
    ArdaError err = ArdaError::TaskYielded;
    assert(err == ArdaError::TaskYielded);

    printf("PASSED\n");
}

void test_yield_reentrancy() {
    printf("Test: yield reentrancy guard... ");
    resetTestCounters();
    yieldLoopCalled = 0;

    Arda os;
    os.createTask("yldTask", yldTask_setup, yldTask_loop, 0);
    os.begin();

    // This would stack overflow without reentrancy guard
    os.run();

    // Task should have run exactly once despite calling yield()
    assert(yieldLoopCalled == 1);

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
    // Stop should fail (task is mid-yield, teardown would corrupt stack)
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
static StopResult yieldRestartStopResult1 = StopResult::Failed;
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
    yieldRestartStopResult1 = StopResult::Failed;
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
    StartResult restartResult = OS.startTask(yieldRestartTargetId);
    assert(restartResult == StartResult::Success);
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
    // Deleter is in yield (inYield=true), so stop should fail (can't run teardown mid-yield)
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

void test_task_yielded_error_string() {
    printf("Test: TaskYielded error string... ");

    const char* str = Arda::errorString(ArdaError::TaskYielded);
    assert(str != nullptr);
    // Either short or long form should contain "Yielded"
    assert(strstr(str, "Yielded") != nullptr || strstr(str, "yielded") != nullptr);

    printf("PASSED\n");
}

int main() {
    printf("\n=== ARDA_YIELD Tests ===\n\n");

    test_yield_api_available();
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
    test_task_yielded_error_string();

    printf("\n=== All tests passed! ===\n\n");
    return 0;
}
