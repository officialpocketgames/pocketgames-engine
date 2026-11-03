// SPDX-License-Identifier: LGPL-2.1-or-later
// From steamac's DXMT patch 0010 (SteveTanSaMa, tests/regression/scheduler_lifetime.cpp of
// SteveTanSaMa/DXMT def87758), fitted for MYIOSDECK spec 05 clarification 10: this tree's CHECK
// helpers instead of assert, and a summary line like the other host tests.
//
// task_scheduler::shutdown (dxmt/src/dxmt/dxmt_tasks.hpp) joins a running task before its owner
// destroys what the task uses (PipelineCache declares the scheduler before its task storage);
// sleeping workers, a repeated shutdown and a submission after it are harmless.

#include "test_common.hpp"

#include "dxmt_tasks.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>

// Native Win32 compatibility helpers only need this logging entry point.
void
dxmt::Logger::warn(const std::string &) {}

namespace {

std::atomic<unsigned> g_ran_after_destroy{0};

struct Task {
  std::promise<void> entered;
  std::shared_future<void> release;
  std::atomic_bool done = false;
  std::atomic_bool &completed;
  std::atomic_bool &destroyed;
  ~Task() { destroyed = true; }
};

} // namespace

template <> struct dxmt::task_trait<Task *> {
  Task *
  run_task(Task *task) {
    task->entered.set_value();
    task->release.wait();
    if (task->destroyed.load())
      g_ran_after_destroy++;
    task->completed = true;
    return task;
  }
  bool
  get_done(Task *task) {
    return task->done;
  }
  void
  set_done(Task *task) {
    task->done = true;
  }
};

namespace {

void
TestShutdownJoinsRunningTask() {
  for (int iteration = 0; iteration < 50; ++iteration) {
    std::promise<void> release;
    std::atomic_bool completed = false, destroyed = false;
    // Match PipelineCache: the scheduler precedes its task-owning storage.
    struct Owner {
      dxmt::task_scheduler<Task *> scheduler;
      std::unique_ptr<Task> task;
      ~Owner() { scheduler.shutdown(); }
    };
    auto owner = std::make_unique<Owner>();
    owner->task = std::unique_ptr<Task>(new Task{{}, release.get_future().share(), false, completed, destroyed});
    auto entered = owner->task->entered.get_future();
    owner->scheduler.submit(owner->task.get());
    CHECK(entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto destroying = std::async(std::launch::async, [&] { owner.reset(); });
    // the owner's destructor waits for the running task
    CHECK(destroying.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    CHECK(!destroyed);
    release.set_value();
    CHECK(destroying.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    destroying.get();
    CHECK(completed && destroyed);
  }
  CHECK_EQ(g_ran_after_destroy.load(), 0);
}

void
TestIdleRepeatedAndLateSubmit() {
  // sleeping workers, repeated shutdown, and a late submission (dropped, never run)
  dxmt::task_scheduler<Task *> idle;
  idle.shutdown();
  idle.shutdown();
  idle.submit(nullptr);
  CHECK(true);
}

} // namespace

int
main() {
  TestShutdownJoinsRunningTask();
  TestIdleRepeatedAndLateSubmit();
  return TestSummary("test_scheduler_lifetime");
}
