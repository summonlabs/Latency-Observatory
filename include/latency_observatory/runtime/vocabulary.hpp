// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Re-exports the shared domain vocabulary into the runtime namespace.
#include "latency_observatory/model/identities.hpp"

namespace latobs::runtime {

using core::Nanos;
using core::Timestamp;
using core::MonoTime;
using core::SourceId;
using core::PathId;
using core::HopId;
using core::HopIndex;
using core::LinkId;
using core::QueueId;
using core::EndpointId;
using core::GenerationId;
using core::ClockDomainId;
using core::EpochId;
using core::IncarnationId;
using core::BaselineId;
using core::MeasurementId;
using core::SnapshotId;
using core::SessionId;
using core::Sequence;
using core::Revision;
using core::Name;
using core::Error;
using core::ErrorCode;
using core::Result;
using core::Status;

}  // namespace latobs::runtime
