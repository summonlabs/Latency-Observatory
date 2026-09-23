// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "latency_observatory/baseline/baseline.hpp"
#include "latency_observatory/core/json.hpp"

namespace latobs::baseline {

[[nodiscard]] Result<Baseline> decode_baseline(const core::JsonValue& value);
[[nodiscard]] Result<Baseline> decode_baseline_text(std::string_view text,
                                                    const core::Limits& limits);

}  // namespace latobs::baseline
