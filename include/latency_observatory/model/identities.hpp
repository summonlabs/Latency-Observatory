// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Domain vocabulary shared by every layer above core. The typed identities,
/// time quantities and error/result vocabulary live in latobs::core; this
/// header re-exports them into latobs::model so that domain types can be
/// written without a namespace prefix while remaining strongly typed.
#include "latency_observatory/core/error.hpp"
#include "latency_observatory/core/ids.hpp"
#include "latency_observatory/core/time.hpp"

namespace latobs::model {

using core::BaselineId;
using core::ClockDomainId;
using core::EndpointId;
using core::EpochId;
using core::Error;
using core::ErrorCode;
using core::GenerationId;
using core::HopId;
using core::HopIndex;
using core::IncarnationId;
using core::LinkId;
using core::MeasurementId;
using core::MonoTime;
using core::Name;
using core::Nanos;
using core::PathId;
using core::QueueId;
using core::Result;
using core::Revision;
using core::Sequence;
using core::SessionId;
using core::SnapshotId;
using core::SourceId;
using core::Status;
using core::SubscriptionId;
using core::Timestamp;

}  // namespace latobs::model
