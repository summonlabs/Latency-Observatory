// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace latobs::runtime {

/// Runs the synthetic end to end scenario. The scenario is generated in process
/// from a seeded generator and is labelled SYNTHETIC everywhere it appears: no
/// observed hardware, fabric or switch data is involved.
[[nodiscard]] int run_demo(const std::string& store_directory, std::size_t samples,
                           std::uint64_t seed, bool pretty);

/// Builds the synthetic scenario requests. Exposed so tests can exercise the
/// same scenario the tool prints.
[[nodiscard]] std::string build_demo_request(std::uint64_t seed, std::size_t samples);

}  // namespace latobs::runtime
