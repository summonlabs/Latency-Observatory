// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>

#include "latency_observatory/runtime/cancellation.hpp"
#include "latency_observatory/runtime/vocabulary.hpp"

namespace latobs::runtime {

/// A bounded worker pool with real cancellation and real shutdown.
///
/// Deadlock and reentrancy rules (audited in docs/concurrency.md):
///   * submitting from inside a worker of the same pool is refused with a typed
///     error: a task can never wait on the pool that is running it;
///   * the queue is bounded, so a saturated runtime refuses work instead of
///     growing without limit;
///   * shutdown wakes every worker, cancels the token, and either drains or
///     cancels the queued work; every queued task settles with a value or an
///     error, so no waiter can block forever;
///   * the pool owns no lock while running a task.
class WorkerPool {
 public:
  using Outcome = core::Result<std::string>;
  using Function = std::function<Outcome(const CancellationToken&)>;

  struct Config {
    std::size_t workers = 4;
    std::size_t queue_depth = 256;
  };

  struct Stats {
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t refused = 0;
    std::uint64_t reentrant_refusals = 0;
    std::uint64_t peak_pending = 0;
    std::size_t pending = 0;
    bool shutting_down = false;
  };

  /// Handle to a submitted task. The task is always settled: with a value, with
  /// an error, or with a cancellation.
  class Handle {
   public:
    Handle() = default;
    explicit Handle(std::shared_future<Outcome> future) : future_(std::move(future)) {}
    [[nodiscard]] bool valid() const noexcept { return future_.valid(); }
    /// Never throws: a task that could not run is reported as an error value.
    [[nodiscard]] Outcome get() const {
      if (!future_.valid()) {
        return core::Error(core::ErrorCode::InvalidArgument, "the handle has no task");
      }
      try {
        return future_.get();
      } catch (const std::future_error& error) {
        return core::Error(core::ErrorCode::Cancelled, "the task never produced a result",
                           error.what());
      }
    }

   private:
    std::shared_future<Outcome> future_;
  };

  WorkerPool() = default;
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  WorkerPool(WorkerPool&&) noexcept = default;
  WorkerPool& operator=(WorkerPool&&) noexcept = default;

  [[nodiscard]] static Result<WorkerPool> create(const Config& config);

  /// Submits work. Fails with CapacityExceeded when the queue is full, with
  /// ShuttingDown after shutdown started, and with InvalidArgument when called
  /// from one of this pool's own workers.
  [[nodiscard]] Result<Handle> submit(Function function);

  /// Requests cancellation of the token shared by the pending work.
  void cancel_pending(std::string reason);

  /// Stops the pool. With \p drain the queued work is completed first; without
  /// it the queued work is cancelled. Idempotent.
  [[nodiscard]] Status shutdown(bool drain);

  [[nodiscard]] Stats stats() const;

  [[nodiscard]] bool shutting_down() const noexcept;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace latobs::runtime
