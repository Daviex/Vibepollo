// Exercise the production worker with synchronization barriers, without devices.
#include "src/input_executor.h"

#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <vector>

using namespace std::chrono_literals;
using input::serial_executor_t;

void wait(std::future<void> &future) {
  assert(future.wait_for(5s) == std::future_status::ready);
  future.get();
}

void barrier(serial_executor_t &worker) {
  std::promise<void> done;
  auto future = done.get_future();
  assert(worker.push([&] { done.set_value(); }));
  wait(future);
}

void fifo_and_independent_progress() {
  serial_executor_t background, input;
  background.start();
  input.start();
  std::promise<void> entered, unblock;
  auto entered_future = entered.get_future(), unblock_future = unblock.get_future();
  assert(background.push([&] { entered.set_value(); unblock_future.wait(); }));
  wait(entered_future);
  std::vector<int> delivered;
  for (int i = 0; i < 1000; ++i) assert(input.push([&, i] { delivered.push_back(i); }));
  barrier(input);
  assert(delivered.size() == 1000);
  for (int i = 0; i < 1000; ++i) assert(delivered[i] == i);
  unblock.set_value();
  background.stop();
  input.stop();
}

void cancellation_and_fair_due_timers() {
  serial_executor_t worker;
  worker.start();
  std::promise<void> entered, unblock;
  auto entered_future = entered.get_future(), unblock_future = unblock.get_future();
  assert(worker.push([&] { entered.set_value(); unblock_future.wait(); }));
  wait(entered_future);
  int cancelled_fired = 0;
  auto cancelled = worker.pushDelayed([&] { ++cancelled_fired; }, 0ms).task_id;
  assert(worker.cancel(cancelled));
  assert(!worker.cancel(cancelled));
  std::vector<int> order;
  for (int i = 0; i < 100; ++i) assert(worker.push([&] { order.push_back(1); }));
  auto press = worker.pushDelayed([&] { assert(false); }, 0ms).task_id;
  // An already queued Back/key-up must cancel its press-generating timer,
  // even when the worker wakes after that timer's nominal deadline.
  assert(worker.push([&] { assert(worker.cancel(press)); }));
  for (int i = 0; i < 100; ++i) assert(worker.pushDelayedRelease([&] { order.push_back(2); }, 0ms).task_id);
  unblock.set_value();
  // This immediate barrier alone need not follow all timers, so add a timer
  // barrier as well. Each FIFO is ordered; both producers are now quiescent.
  barrier(worker);
  std::promise<void> timer_done;
  auto timer_future = timer_done.get_future();
  assert(worker.pushDelayed([&] { timer_done.set_value(); }, 0ms).task_id);
  wait(timer_future);
  worker.stop();
  assert(cancelled_fired == 0 && order.size() == 200);
  assert(order[0] == 2 && order[1] == 1 && order[2] == 2 && order[3] == 1);
}

void stop_drains_and_rejects_recursion() {
  serial_executor_t worker;
  std::thread::id owner;
  worker.start([&] { owner = std::this_thread::get_id(); });
  std::promise<void> entered, unblock;
  auto entered_future = entered.get_future(), unblock_future = unblock.get_future();
  assert(worker.push([&] { entered.set_value(); unblock_future.wait(); }));
  wait(entered_future);
  std::atomic<int> accepted {0}, executed {0};
  for (int i = 0; i < 50; ++i) {
    assert(worker.push([&] {
      ++executed;
      assert(!worker.push([] {}));
      assert(!worker.pushDelayed([] {}, 0ms).task_id);
    }));
    ++accepted;
  }
  auto held = std::make_shared<int>(42);
  std::weak_ptr<int> weak = held;
  assert(worker.pushDelayed([held] { assert(false); }, 24h).task_id);
  held.reset();
  bool finalized = false;
  auto stopped = std::async(std::launch::async, [&] {
    worker.stop([&] {
      assert(std::this_thread::get_id() == owner);
      assert(executed == accepted && weak.expired());
      finalized = true;
    });
  });
  // Observe the real admission boundary, without depending on a sleep.
  while (worker.push([&] { ++executed; })) ++accepted;
  unblock.set_value();
  assert(stopped.wait_for(5s) == std::future_status::ready);
  stopped.get();
  assert(finalized && weak.expired() && executed == accepted);
  assert(!worker.push([] {}) && !worker.pushDelayed([] {}, 0ms).task_id);
  worker.stop();
}

void cancellation_destroys_captures_without_lock() {
  serial_executor_t worker;
  worker.start();
  std::atomic<int> destructed {0};
  struct captured_t {
    serial_executor_t &worker;
    std::atomic<int> &destructed;
    ~captured_t() {
      ++destructed;
      worker.push([] {}); // Must not deadlock the executor's queue mutex.
    }
  };
  auto held = std::make_shared<captured_t>(worker, destructed);
  auto id = worker.pushDelayed([held] {}, 24h).task_id;
  held.reset();
  assert(worker.cancel(id));
  assert(destructed == 1);
  barrier(worker);
  worker.stop();
  worker.start();
  barrier(worker);
  worker.stop();
}

int main() {
  fifo_and_independent_progress();
  cancellation_and_fair_due_timers();
  stop_drains_and_rejects_recursion();
  cancellation_destroys_captures_without_lock();
  std::cout << "PASS: input executor FIFO, isolated progress, fair timers, cancellation, shutdown and restart\n";
}
