// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/runtime/worker_pool.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace latobs::runtime {
namespace {

/// Identifies the pool whose worker is running on this thread, so that
/// reentrant submission can be refused instead of deadlocking.
thread_local const void* current_worker_pool = nullptr;

using Task = WorkerPool::Function;
using Outcome = WorkerPool::Outcome;

struct QueuedTask {
  Task function;
  std::shared_ptr<std::promise<Outcome>> promise;
  CancellationToken token;
};

}  // namespace

struct WorkerPool::State {
  Config config;
  std::vector<std::thread> threads;
  std::deque<QueuedTask> queue;
  mutable std::mutex mutex;
  std::condition_variable not_empty;
  std::condition_variable not_full;
  std::condition_variable idle;
  Stats stats;
  bool stopping = false;
  bool draining = false;
  std::size_t active = 0;

  /// Destruction joins every worker and settles every queued task, so no
  /// waiter can block forever and no joinable thread is ever destroyed.
  ~State() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      draining = false;
      while (!queue.empty()) {
        QueuedTask task = std::move(queue.front());
        queue.pop_front();
        try {
          task.promise->set_value(
              Error(ErrorCode::ShuttingDown, "the worker pool was destroyed"));
        } catch (const std::future_error&) {
          // Already settled by another path.
        }
      }
    }
    not_empty.notify_all();
    not_full.notify_all();
    for (std::thread& thread : threads) {
      if (thread.joinable()) thread.join();
    }
    threads.clear();
  }
};

Result<WorkerPool> WorkerPool::create(const Config& config) {
  if (config.workers == 0) {
    return Error(ErrorCode::InvalidArgument, "a worker pool needs at least one worker");
  }
  if (config.queue_depth == 0) {
    return Error(ErrorCode::InvalidArgument, "a worker pool needs a non-zero queue depth");
  }
  WorkerPool pool;
  pool.state_ = std::make_shared<State>();
  pool.state_->config = config;

  const std::shared_ptr<State> state = pool.state_;
  state->threads.reserve(config.workers);
  for (std::size_t index = 0; index < config.workers; ++index) {
    state->threads.emplace_back([state]() {
      current_worker_pool = state.get();
      while (true) {
        QueuedTask task;
        {
          std::unique_lock<std::mutex> lock(state->mutex);
          state->not_empty.wait(lock, [&state]() {
            return !state->queue.empty() || state->stopping;
          });
          if (state->queue.empty()) {
            if (state->stopping) break;
            continue;
          }
          task = std::move(state->queue.front());
          state->queue.pop_front();
          ++state->active;
          state->not_full.notify_one();
        }

        Outcome outcome = [&task]() -> Outcome {
          if (task.token.cancelled()) return task.token.error();
          try {
            return task.function(task.token);
          } catch (const std::exception& error) {
            return Error(ErrorCode::Internal, "task threw an exception", error.what());
          } catch (...) {
            return Error(ErrorCode::Internal, "task threw an unknown exception");
          }
        }();

        {
          std::unique_lock<std::mutex> lock(state->mutex);
          --state->active;
          if (outcome.has_value()) {
            ++state->stats.completed;
          } else if (outcome.error().code() == ErrorCode::Cancelled) {
            ++state->stats.cancelled;
          } else {
            ++state->stats.failed;
          }
          // A cancelled outcome may be delivered to either the task or the
          // shutdown path; setting a promise twice throws, so guard it.
          try {
            task.promise->set_value(std::move(outcome));
          } catch (const std::future_error&) {
            // The shutdown path already settled this task.
          }
          if (state->queue.empty() && state->active == 0) state->idle.notify_all();
        }
      }
      current_worker_pool = nullptr;
    });
  }
  return pool;
}

Result<WorkerPool::Handle> WorkerPool::submit(Function function) {
  if (state_ == nullptr) {
    return Error(ErrorCode::InvalidArgument, "the worker pool is not initialised");
  }
  if (!function) {
    return Error(ErrorCode::InvalidArgument, "cannot submit an empty task");
  }
  if (current_worker_pool == state_.get()) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->stats.reentrant_refusals;
    return Error(ErrorCode::InvalidArgument,
                 "re-entrant submission from a worker of the same pool is refused");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->stopping) {
    return Error(ErrorCode::ShuttingDown, "the worker pool is shutting down");
  }
  if (state_->queue.size() >= state_->config.queue_depth) {
    ++state_->stats.refused;
    return Error(ErrorCode::CapacityExceeded, "the worker pool queue is full",
                 std::to_string(state_->config.queue_depth));
  }
  auto promise = std::make_shared<std::promise<Outcome>>();
  std::shared_future<Outcome> future = promise->get_future().share();
  QueuedTask task;
  task.function = std::move(function);
  task.promise = promise;
  task.token = CancellationToken{};
  state_->queue.push_back(std::move(task));
  ++state_->stats.submitted;
  if (state_->queue.size() > state_->stats.peak_pending) {
  state_->stats.peak_pending = state_->queue.size();
  }
  state_->not_empty.notify_one();
  return Handle(std::move(future));
}

void WorkerPool::cancel_pending(std::string reason) {
  if (state_ == nullptr) return;
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (QueuedTask& task : state_->queue) {
    task.token.cancel(reason);
  }
}

Status WorkerPool::shutdown(bool drain) {
  if (state_ == nullptr) return core::ok_status();
  std::deque<QueuedTask> abandoned;
  {
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->stopping && state_->threads.empty()) return core::ok_status();
    state_->stopping = true;
    state_->draining = drain;
    if (drain) {
      state_->idle.wait(lock, [this]() {
        return state_->queue.empty() && state_->active == 0;
      });
    } else {
      while (!state_->queue.empty()) {
        abandoned.push_back(std::move(state_->queue.front()));
        state_->queue.pop_front();
      }
    }
    lock.unlock();
    state_->not_empty.notify_all();
    state_->not_full.notify_all();
  }
  for (QueuedTask& task : abandoned) {
    task.token.cancel("the runtime shut down before the task started");
    try {
      task.promise->set_value(Error(ErrorCode::Cancelled,
                                    "the runtime shut down before the task started"));
      std::unique_lock<std::mutex> lock(state_->mutex);
      ++state_->stats.cancelled;
    } catch (const std::future_error&) {
      // Already settled elsewhere.
    }
  }
  for (std::thread& thread : state_->threads) {
    if (thread.joinable()) thread.join();
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->threads.clear();
  }
  return core::ok_status();
}

WorkerPool::Stats WorkerPool::stats() const {
  Stats copy;
  if (state_ == nullptr) return copy;
  std::lock_guard<std::mutex> lock(state_->mutex);
  copy = state_->stats;
  copy.pending = state_->queue.size();
  copy.shutting_down = state_->stopping;
  return copy;
}

bool WorkerPool::shutting_down() const noexcept {
  if (state_ == nullptr) return true;
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->stopping;
}

}  // namespace latobs::runtime
