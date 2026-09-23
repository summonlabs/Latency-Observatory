# Architecture

Latency Observatory is a layered C++20 library with a thin command line front
end. Every layer may only depend on the layers above it in this list, which is
also the link order of the exported targets.

| Layer | Target | Responsibility |
| --- | --- | --- |
| core | `latobs::core` | identities, checked arithmetic, digests, time, evidence vocabulary, runtime policy, canonical JSON |
| model | `latobs::model` | sources, endpoints, links, queues, hops, generations, paths, clock domains, clock synchronization, measurement records and their codecs |
| stats | `latobs::stats` | distributions, histograms, quantiles, path summaries, history bucketing |
| ingest | `latobs::ingest` | the admission gate: structural validation and every fence |
| baseline | `latobs::baseline` | generation bound baselines, comparisons, anomaly statements |
| attribute | `latobs::attribute` | per hop decomposition with the semantics gate and the residual |
| store | `latobs::store` | versioned, integrity checked persistence and conservative recovery |
| runtime | `latobs::runtime` | engine, worker pool, request service, TCP transport, explanation, export |

The command line tool `latobs` and the examples link `latobs::runtime` only.

## Data flow

```
source declaration ─┐
clock sync report ──┤
                    ▼
             ┌─────────────┐   fenced, enriched    ┌──────────────┐
 observations │ ingest gate │ ────────────────────▶ │ engine store │
             └─────────────┘                       └──────┬───────┘
                    │                                     │
                    │ typed reasons                        │ durable segments
                    ▼                                     ▼
             ingest report                          store (CRC checked)
                                                          │
                        ┌─────────────────────────────────┘
                        ▼
              ┌───────────────────┐   ┌──────────────┐   ┌──────────────┐
              │ stats: summarize  │──▶│ baseline     │──▶│ attribute    │
              │ history bucketing │   │ comparison   │   │ decomposition│
              └───────────────────┘   └──────────────┘   └──────────────┘
                        │                   │                  │
                        └──────────┬────────┴──────────────────┘
                                   ▼
                        explanation and export
```

## Identities

Every important object has its own type. The compiler refuses to pass a
`HopId` where a `PathId` is expected, and the runtime never has to guess what a
bare integer means.

| Identity | Object |
| --- | --- |
| `SourceId` | a telemetry source declaration |
| `EndpointId`, `LinkId`, `QueueId`, `HopId` | the observed topology elements |
| `PathId` | an ordered sequence of hops inside one generation |
| `GenerationId` | a deployment epoch of the observed system |
| `ClockDomainId` | a clock domain that readings belong to |
| `EpochId`, `IncarnationId` | the boot epoch and process incarnation of a source |
| `MeasurementId` | one observed exchange, derived from source, epoch, incarnation and sequence |
| `BaselineId` | one generation bound reference distribution |
| `SnapshotId`, `SessionId` | reserved for operator level references |

Identities are content addressed: `StrongId::derive_from(canonical_name)` hashes
the canonical text with FNV-1a 64 and maps zero to one. The same name therefore
yields the same identity in every process, before and after a restart. The
all-zero identity is reserved for "no identity" and round trips through its
canonical hexadecimal text form.

Names are validated (`Name::parse`): at most 128 characters from
`[A-Za-z0-9_.:/@+-]`, starting with an alphanumeric or underscore.

## Definitions, revisions and generations

The catalog holds one entry per identity in an ordered container, so listings and
exports are deterministic. Redefining identical content is idempotent;
redefining different content at the same or an older revision is refused with
`ErrorCode::Conflict`; a newer revision replaces the definition. References are
validated at definition time, so a path can never point at a hop that does not
exist.

A generation may supersede another. The lineage is explicit: a generation is
superseded when another generation declares it, and `current_generation()`
returns the single tip or an invalid identity when the lineage branches. Nothing
is inferred from a name or a timestamp.

## Clocks

`ClockRegistry` owns every comparability decision. Two readings may be compared
when:

1. they belong to the same domain (a difference inside one clock is always
   meaningful, even when the clock is free running), or
2. both domains are synchronized to the local reference domain with a fresh,
   generation, epoch and incarnation matched report whose reported uncertainty
   stays inside `ComparabilityPolicy::max_uncertainty_ns`.

Anything else is refused, with a typed reason and the evidence state that
matches the condition (`Unknown` when nothing is known, `Refused` when policy
declines, `Stale` when the report expired, `Conflicting` when the reports
contradict each other). Synchronization chains are not followed: transitivity is
disabled by construction and by policy validation.

## Aggregation

Distributions are computed from sorted values, so they do not depend on arrival
order. Latency values are integers throughout; there is no floating point in the
measurement path. Sums are accumulated in a 128 bit accumulator and reported as
unknown (with `sum_overflowed`) rather than wrapped when they do not fit a
signed 64 bit value. The mean is an exact floor division with the remainder
reported next to it, and quantiles use the nearest rank definition with an exact
rational probe, so the same probe always selects the same rank on every
platform.

## Persistence and restart

The store appends canonical JSON payloads inside a binary, CRC checked frame.
The manifest carries the format version and a checksum; a store written by a
newer format version is refused rather than guessed at. Recovery stops at the
first damaged record and reports the truncation.

On load, evidence keeps its recorded observation and receive times and its
recorded state, but its freshness is recomputed for the new session. Persisted
dynamic evidence therefore never silently becomes current: a restarted runtime
serves it as history and refuses to aggregate it as current timing. Fence state
is persisted too, so a replayed sequence is still recognised after a restart.

## Concurrency

The engine has one shared mutex. Writers (definitions, clock synchronization,
ingestion, baselines, shutdown) take it exclusively; readers (summaries,
history, attribution, explanation, export) take it shared, so parallel queries
really do run in parallel. No public entry point takes the lock twice: internal
helpers are suffixed `_locked` and assume the caller holds it. See
[docs/concurrency.md](concurrency.md) for the full audit.

## Bounded resources

`core::Limits` is part of the runtime policy, is validated at construction and
is included in the policy digest that every explanation carries. Externally
derived sizes are checked with `core/checked.hpp` before memory is reserved, and
reaching a bound produces a typed error rather than a silent drop or an
unbounded allocation.
