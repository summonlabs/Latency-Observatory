// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <utility>

#include "latency_observatory/core/json.hpp"
#include "latency_observatory/ingest/ingest.hpp"

namespace latobs::ingest {

/// Fence state is persisted so that a restarted runtime keeps refusing
/// replays instead of accepting them again as if they were new evidence.
void write_json(core::JsonWriter& writer, SourceId source, const SourceFenceState& state);
[[nodiscard]] Result<std::pair<SourceId, SourceFenceState>> decode_fence_state(
    const core::JsonValue& value);

}  // namespace latobs::ingest
