# Testing

## Building and running

```console
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Every test is a registered CTest test with `TIMEOUT 0`: no test may pass or fail
because of a clock. The suite uses no sleeps, no polling and no retry loops; the
one blocking handshake (the transport startup) is a pipe read.

## Suites

| Test | Kind | What it proves |
| --- | --- | --- |
| `t_unit_core` | unit | name and identity validation, checked arithmetic and 128 bit helpers, SHA-256/CRC-32C/FNV known vectors, UTC rendering before and after the epoch, canonical JSON and the strict parser, the evidence lattice, policy validation and digest |
| `t_unit_stats` | unit | exact rational probes, nearest rank definition, exact sums and floor means with remainders, overflow reporting, histogram bucketing, order independence, limits, distribution codec round trip |
| `t_model_clocks` | unit | catalog definition, revision and dangling reference rules, generation lineage and branching, clock sync revision fences, the full comparability matrix, age estimation, entity and measurement codecs |
| `t_ingest_fences` | integration | acceptance of fresh evidence, duplicate sequences, gaps, reordering, epoch/incarnation/generation/revision fences, staleness, structural rejection, batch bounds, bounded verdicts |
| `t_missing_hops` | proof | a hop that was never reported is unknown: no mean, no quantile, empty CSV fields, explicit coverage, refusal of a non tiling decomposition |
| `t_incomparable_clocks` | proof | incomparable clocks cannot produce a fabricated attribution: no dwell, no accounted time, no shares, with a comparable control run |
| `t_baselines` | integration | baseline creation rules, every mismatch kind reported separately, generation bound lookup, anomaly classification, no causal claim, codec round trip |
| `t_attribution` | integration | complete decomposition, residual reporting (positive and negative), semantics gate, refusal with no evidence, staleness, queue residency kept separate, anomaly attachment, unknown baseline |
| `t_determinism` | property/randomized | the same data set submitted in six orders and in different batchings produces identical aggregation and identical exported row sets; histogram invariants and the sum identity over 25 random populations; two independent runtimes produce the same digest |
| `t_store_restart` | restart/recovery | round trip through disk, restart does not make history current, replays still refused, conservative truncation, corruption detection, manifest integrity and version enforcement, rotation and capacity limits |
| `t_concurrency` | concurrency/race | worker pool completion, overload and reentrancy refusal, real cancellation, settling shutdown, parallel readers agreeing with a serial reader, concurrent ingestion storing every record once |
| `t_service_protocol` | integration | envelope shape, typed errors instead of exceptions, request size bounds, the full pipeline through the protocol, deterministic responses, export truncation accounting |
| `t_transport_e2e` | end to end | real sockets in process, malformed input handling, oversized line refusal, and the decisive case: a server process, a client process started by the test, a second client process, and a fourth process reading the store the server wrote |
| `t_cli_e2e` | end to end | the deployed binary: version, operations, capability matrix, inline and file requests, exit codes for failed operations and usage errors, the synthetic demo, and a store shared across processes |
| `package_install`, `downstream_find_package` | packaging | installation into a scratch prefix, and an independent CMake project that calls `find_package(LatencyObservatory)`, links `latobs::runtime`, builds and runs |

## Adversarial cases

The adversarial work is distributed across the suites rather than isolated in one
file:

* malformed, duplicate key, non integral, over deep and over long JSON;
* oversized transport lines, oversized request documents, oversized batches,
  oversized payloads, oversized stores;
* unknown operations, unknown names, wrong field types, missing fields;
* duplicate sequences, reordered sequences, sequence gaps, epoch and incarnation
  replays, revision rollbacks, superseded generations;
* cross domain hops without synchronization, in a holdover state, with an expired
  report, with a generation mismatch, with uncertainty above the ceiling, and
  with a reference mismatch;
* corrupted manifests, corrupted segment headers, corrupted record CRCs,
  truncated segments, empty unclosed segments;
* hops that contradict the path, hops that are not strictly increasing, hops
  present without a usable dwell, hops missing entirely.

## Determinism

Determinism is asserted by comparing canonical JSON bytes, not by comparing
samples: `t_determinism.shuffled_ingest_batches_produce_identical_exports`
compares the full aggregation content and the sorted set of exported CSV rows
across five different batchings and orderings of the same data set. The evidence
reason list is deliberately excluded from that comparison, because fences record
gaps and reordering that genuinely differ between orderings.

## Sanitizers

`-DLATOBS_ENABLE_ASAN=ON` adds AddressSanitizer to every first party target
(`/fsanitize=address` on MSVC, `-fsanitize=address` on GCC and Clang);
`-DLATOBS_ENABLE_UBSAN=ON` adds UndefinedBehaviorSanitizer where the compiler
supports it (GCC and Clang only: MSVC has no equivalent, which the build message
states rather than hides). The runtime reports sanitizer availability through
`capabilities`.

## Warnings

First party code is compiled with `/W4 /WX` on MSVC and with a strict GCC/Clang
warning set plus `-Werror` elsewhere. The project has no third party
dependencies, so there is no third party warning surface to suppress. The warning
count is zero in Debug, Release and the sanitizer build.
