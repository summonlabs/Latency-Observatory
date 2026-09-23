// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "latency_observatory/core/error.hpp"

namespace latobs::runtime {

/// Cooperative cancellation state. Cancellation is always real: a cancelled
/// operation stops at its next checkpoint and reports the cancellation instead
/// of producing a partial result that looks complete.
class CancellationState {
 public:
  void cancel(std::string reason) {
    reason_ = std::move(reason);
    cancelled_.store(true, std::memory_order_release);
  }
  [[nodiscard]] bool cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
  }
  [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

 private:
  std::atomic<bool> cancelled_{false};
  std::string reason_;
};

/// A token handed to work items. Tokens share the state of the operation they
/// belong to; copying a token does not create a new cancellation domain.
class CancellationToken {
 public:
  CancellationToken() : state_(std::make_shared<CancellationState>()) {}

  [[nodiscard]] bool cancelled() const noexcept { return state_->cancelled(); }
  void cancel(std::string reason) const { state_->cancel(std::move(reason)); }
  [[nodiscard]] const std::string& reason() const noexcept { return state_->reason(); }
  [[nodiscard]] core::Error error() const {
    return core::Error(core::ErrorCode::Cancelled,
                       reason().empty() ? "operation cancelled" : reason());
  }

 private:
  std::shared_ptr<CancellationState> state_;
};

/// Throws nothing and allocates nothing on the happy path: called at loop
/// checkpoints.
inline core::Status check_cancelled(const CancellationToken& token) {
  if (token.cancelled()) return token.error();
  return core::ok_status();
}

}  // namespace latobs::runtime
