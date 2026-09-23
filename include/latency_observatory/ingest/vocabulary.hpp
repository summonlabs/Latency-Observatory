// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Re-exports the shared domain vocabulary into the ingest namespace.
#include "latency_observatory/model/identities.hpp"

namespace latobs::ingest {

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
using core::Timestamp;

}  // namespace latobs::ingest
