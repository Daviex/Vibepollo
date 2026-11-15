/**
 * @file src/input_executor.h
 * @brief Single-owner input work and cancellable timers, independent of background work.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

namespace input {
  class serial_executor_t {
    struct timer_identity_t {};

  public:
    using clock = std::chrono::steady_clock;
    using task_id_t = std::shared_ptr<const timer_identity_t>;
    struct timer_task_t {
      task_id_t task_id;
    };

    serial_executor_t() = default;
    serial_executor_t(const serial_executor_t &) = delete;
    serial_executor_t &operator=(const serial_executor_t &) = delete;

    ~serial_executor_t() {
      stop();
    }

    // Lifecycle calls belong to the owner, never to the worker's callbacks.
    void start(std::function<void()> initialize = {}, std::function<void(std::exception_ptr)> on_error = {}) {
      std::lock_guard lifecycle_lock(lifecycle_mutex_);
      std::lock_guard lock(mutex_);
      if (started_) return;
      on_error_ = std::move(on_error);
      accepting_ = true;
      stopping_ = false;
      try {
        worker_ = std::thread([this, initialize = std::move(initialize)] {
          invoke(initialize);
          run();
        });
        started_ = true;
      } catch (...) {
        accepting_ = false;
        throw;
      }
    }

    template<class Function, class... Args>
    bool push(Function &&function, Args &&...args) {
      auto task = bind(std::forward<Function>(function), std::forward<Args>(args)...);
      {
        std::lock_guard lock(mutex_);
        if (!accepting_) return false;
        ready_.push_back(std::move(task));
      }
      wake_.notify_one();
      return true;
    }

    template<class Function, class Rep, class Period, class... Args>
    timer_task_t pushDelayed(Function &&function, std::chrono::duration<Rep, Period> delay, Args &&...args) {
      return schedule(false, std::forward<Function>(function), delay, std::forward<Args>(args)...);
    }

    // Only releases may overtake pending packets. A Back-to-Home or repeat
    // press must first let queued key/button-up packets cancel its timer.
    template<class Function, class Rep, class Period, class... Args>
    timer_task_t pushDelayedRelease(Function &&function, std::chrono::duration<Rep, Period> delay, Args &&...args) {
      return schedule(true, std::forward<Function>(function), delay, std::forward<Args>(args)...);
    }

    bool cancel(const task_id_t &id) {
      if (!id) return false;
      // Captures may own input contexts whose destructors submit cleanup.
      // Always destroy callbacks outside the executor mutex.
      std::function<void()> discarded;
      {
        std::lock_guard lock(mutex_);
        for (auto *timers : {&timers_, &release_timers_}) {
          for (auto it = timers->begin(); it != timers->end(); ++it) {
            if (it->second.id == id) {
              discarded = std::move(it->second.task);
              timers->erase(it);
              break;
            }
          }
          if (discarded) break;
        }
      }
      if (discarded) wake_.notify_one();
      return static_cast<bool>(discarded);
    }

    // Reject producers first, discard all timers, drain admitted immediate
    // callbacks, then run finalization on the same owner before joining.
    // Repeating callbacks cannot keep shutdown alive by resubmitting work.
    void stop(std::function<void()> finalize = {}) {
      std::lock_guard lifecycle_lock(lifecycle_mutex_);
      std::multimap<clock::time_point, timer_t> discarded;
      {
        std::lock_guard lock(mutex_);
        if (!started_ || stopping_) return;
        accepting_ = false;
        discarded.swap(timers_);
        discarded.merge(release_timers_);
      }
      // Finish destroying pending timer captures before the worker can enter
      // platform finalization. Their destructors may try (and fail) to post.
      discarded.clear();
      {
        std::lock_guard lock(mutex_);
        finalizer_ = std::move(finalize);
        stopping_ = true;
      }
      wake_.notify_one();
      worker_.join();
      std::lock_guard lock(mutex_);
      started_ = false;
    }

  private:
    struct timer_t {
      task_id_t id;
      std::function<void()> task;
    };

    template<class Function, class Rep, class Period, class... Args>
    timer_task_t schedule(bool release, Function &&function, std::chrono::duration<Rep, Period> delay, Args &&...args) {
      const auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(delay);
      auto task = bind(std::forward<Function>(function), std::forward<Args>(args)...);
      auto id = std::make_shared<timer_identity_t>();
      {
        std::lock_guard lock(mutex_);
        if (!accepting_) return {};
        auto &timers = release ? release_timers_ : timers_;
        timers.emplace(deadline, timer_t {id, std::move(task)});
      }
      wake_.notify_one();
      return {std::move(id)};
    }

    template<class Function, class... Args>
    static std::function<void()> bind(Function &&function, Args &&...args) {
      return [function = std::forward<Function>(function), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
        std::apply(function, std::move(args));
      };
    }

    void invoke(const std::function<void()> &task) {
      if (!task) return;
      try {
        task();
      } catch (...) {
        if (on_error_) on_error_(std::current_exception());
      }
    }

    void run() {
      bool last_was_timer = false;
      for (;;) {
        std::function<void()> task;
        bool finalizing = false;
        {
          std::unique_lock lock(mutex_);
          for (;;) {
            const bool due = !release_timers_.empty() && release_timers_.begin()->first <= clock::now();
            // Bound starvation in both directions: a due release runs after at
            // most one queued packet, and a busy timer cannot starve packets.
            if (due && (!last_was_timer || ready_.empty())) {
              task = std::move(release_timers_.begin()->second.task);
              release_timers_.erase(release_timers_.begin());
              last_was_timer = true;
              break;
            }
            if (!ready_.empty()) {
              task = std::move(ready_.front());
              ready_.pop_front();
              last_was_timer = false;
              break;
            }
            if (!timers_.empty() && timers_.begin()->first <= clock::now()) {
              task = std::move(timers_.begin()->second.task);
              timers_.erase(timers_.begin());
              last_was_timer = true;
              break;
            }
            if (stopping_) {
              task = std::move(finalizer_);
              finalizing = true;
              break;
            }
            if (timers_.empty() && release_timers_.empty()) {
              wake_.wait(lock);
            } else {
              auto deadline = timers_.empty() ? release_timers_.begin()->first : timers_.begin()->first;
              if (!release_timers_.empty() && release_timers_.begin()->first < deadline) deadline = release_timers_.begin()->first;
              wake_.wait_until(lock, deadline);
            }
          }
        }
        invoke(task);
        if (finalizing) return;
      }
    }

    std::mutex lifecycle_mutex_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> ready_;
    std::multimap<clock::time_point, timer_t> timers_;
    std::multimap<clock::time_point, timer_t> release_timers_;
    std::function<void()> finalizer_;
    std::function<void(std::exception_ptr)> on_error_;
    std::thread worker_;
    bool started_ = false;
    bool accepting_ = false;
    bool stopping_ = false;
  };
}  // namespace input
