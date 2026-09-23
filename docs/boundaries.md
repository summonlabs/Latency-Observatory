# Boundaries: REAL, SYNTHETIC, UNSUPPORTED

The runtime publishes this boundary as data, not as prose:
`latobs capabilities` (or the `capabilities` operation) returns one entry per
claim with a status of `real`, `synthetic` or `unsupported`. The entries below
are the same list.

## REAL

These surfaces are implemented in this repository and are exercised by the test
suite with the mechanism they claim.

| Capability | Evidence |
| --- | --- |
| End to end and per hop latency observation | `t_model_clocks`, `t_missing_hops`, `t_attribution` |
| Deterministic aggregation (count, min, max, exact sum, floor mean with remainder, exact rational quantiles, explicit histograms) | `t_unit_stats`, `t_determinism` |
| Clock comparability gating, including refusal, staleness and uncertainty ceilings | `t_model_clocks`, `t_incomparable_clocks` |
| Generation bound baselines with explicit mismatch kinds | `t_baselines` |
| Fences: epoch, incarnation, generation, revision, authority, sequence replay, reordering, gaps | `t_ingest_fences`, `t_store_restart` |
| Versioned, integrity checked persistence with conservative recovery | `t_store_restart` |
| Restart semantics: history never becomes current | `t_store_restart`, `t_cli_e2e` |
| Bounded workers, queues, batches, payloads, history, results, persistence growth | `t_concurrency`, `t_unit_stats`, `t_store_restart`, `t_service_protocol` |
| A line oriented TCP ingest transport | `t_transport_e2e` (in process sockets and three independent processes) |
| Installable CMake package with an exported target set | `package_install`, `downstream_find_package` |
| Canonical, byte stable JSON and CSV export | `t_determinism`, `t_missing_hops`, `t_service_protocol` |

## SYNTHETIC

| Surface | What it means |
| --- | --- |
| `latobs demo` | Generates a scenario in process from a seeded SplitMix64 generator. It proves the pipeline end to end; it says nothing about any real fabric. |
| Test fixtures | Every measurement in `tests/` is constructed by the test itself with explicit timestamps. No test claims to have observed a real network. |
| Source declarations | The runtime records what a source *declares* about itself (kind, authority, semantics). A declaration is data, not a verification. |

The synthetic surface is labelled where it appears: the demo prints
`SYNTHETIC`, the generated records carry the `synthetic` flag, and every export
of them carries that flag too.

## UNSUPPORTED

These are refused by design, and the runtime says so instead of approximating.

| Not supported | Why |
| --- | --- |
| Switch, ASIC, RDMA, InfiniBand or NVLink telemetry | The runtime never claims hardware counters it did not receive. Hop kinds are semantic labels declared by a source, not hardware identifications. |
| Causality inference | A deviation from a baseline is a statistical statement. Every anomaly and attribution carries `no_causal_inference`. |
| Latency SLO enforcement, routing or queue control | Out of scope: this runtime observes and attributes only. It has no actuator. |
| Multi host fabric observation | Sources are declarations. The runtime does not verify a remote topology, and it does not claim to have discovered one. |
| Transitive clock comparability | A chain of synchronization reports is not evidence. Comparisons are single level against the reference domain. |
| Repair of damaged persistence | Recovery is conservative: the damaged tail is discarded and reported, never reconstructed. |
| Fabricated values for unknown quantities | A missing hop, an unsynchronized clock, an empty window and an overflowing sum all produce *no value* with a typed reason. |

## How to check the boundary

```console
latobs capabilities | grep -o '"capability":"[^"]*","status":"[^"]*"'
latobs demo --samples 16        # prints the SYNTHETIC label first
```

The same list is available in process through
`latobs::runtime::Engine::capabilities()`.
