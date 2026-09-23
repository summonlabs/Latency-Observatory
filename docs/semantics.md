# Semantics

This document is the contract for what the runtime is willing to say. It is the
reference for the tests in `tests/t_missing_hops.cpp`,
`tests/t_incomparable_clocks.cpp`, `tests/t_baselines.cpp` and
`tests/t_attribution.cpp`.

## Evidence states

Every observation, aggregate and derived value carries an `Evidence` with three
orthogonal dimensions.

`EvidenceState` - what the runtime knows:

| State | Meaning |
| --- | --- |
| `observed` | the evidence is present and usable |
| `incomplete` | some of the required evidence is missing; what is present is usable |
| `missing` | the evidence was never reported |
| `stale` | the evidence exists but is outside its validity window |
| `conflicting` | two pieces of evidence contradict each other |
| `unknown` | the runtime has no basis to decide |
| `unsupported` | the declared semantics do not permit the computation |
| `refused` | policy declined the computation |

`Freshness` - how old the evidence is relative to the observation that uses it:
`fresh`, `aging`, `stale`, `expired`, or `unknown` when the age cannot be
established. The classification is a pure function of the age and
`FreshnessPolicy`; a negative age (evidence observed after it was received) is
`unknown`, never fresh.

`Confidence` - an ordinal, never a fabricated probability: `high`,
`moderate`, `low`, `degraded`, `none`. A holdover clock domain yields
`degraded`; a fresh synchronization report yields `high`.

Merging is deterministic and order independent: coverage states combine into
`incomplete`, anything else takes the more severe state, freshness takes the
worse value, confidence takes the lower value, and reasons are deduplicated and
sorted. Reasons are capped at `Evidence::kMaxReasons` with an explicit
truncation flag.

**Absence of evidence is never positive evidence.** Every numeric quantity is an
`std::optional`: a missing hop has no mean, a JSON export writes `null`, a CSV
export writes an empty field, and no code path substitutes zero.

## Fences applied at admission

The admission gate applies the fences in a fixed order and records each outcome
as a typed reason.

1. **Structure.** The path, source and generation must exist; request and
   response must share the declared clock domain; the response must not precede
   the request; the declared end to end latency must equal the difference of the
   timestamps; hop indices must be strictly increasing and must match the
   generation bound path. Failures are `conflicting` and the record is rejected.
2. **Freshness.** The age of the observation relative to the receive time is
   computed on the reference timeline through the clock registry. An age that
   cannot be established yields `unknown` freshness and makes the record
   historical.
3. **Hop derivation.** Same domain hops are always derived. Cross domain hops
   require comparability; otherwise no dwell is produced and the hop carries the
   reason (`incomparable_clocks`, `clock_unsynced`, `clock_sync_expired`,
   `clock_uncertainty_exceeded`, `clock_sync_generation_mismatch`, ...). Queue
   residency is only derived when the queue declares `queue_dwell` semantics.
4. **Epoch and incarnation.** Sessions are ordered by first observation. A new
   session becomes current; evidence from an older session is accepted as
   history with `epoch_replayed` or `incarnation_replayed` and can never be
   current.
5. **Revision and authority.** A record stamped with an older source revision is
   historical; a record stamped with a revision the registry has not seen is
   refused. This is the authority replay fence: a definition cannot be rolled
   back by replaying old evidence.
6. **Sequence.** A sequence lower than the last accepted one and never seen is
   *reordered*: accepted as history. A sequence already inside the bounded replay
   window is a *replay*: rejected as `conflicting`. A gap is reported with its
   size and the accepted evidence is marked incomplete.

The verdict is `accepted_current`, `accepted_historical` or `rejected`.
Only `accepted_current` evidence, with fresh or aging freshness, participates in
a current aggregation.

## Comparability

A cross domain duration is only derived when the comparison succeeds. The
comparison returns the combined uncertainty and the offsets that place both
readings on the reference timeline. A failure returns no value at all.

Refusal is not a missing feature: it is the mechanism that makes fabricated
attribution impossible. `tests/t_incomparable_clocks.cpp` runs the same evidence
through a comparable and an incomparable configuration and asserts that the
incomparable one publishes no accounted time and no shares.

## Aggregation

* Count, minimum, maximum, exact sum, floor mean plus remainder, exact rational
  nearest rank quantiles, explicit histogram buckets.
* `sum_overflowed` is set when the exact sum is not representable as a signed 64
  bit value; the sum and the mean are then unknown rather than wrapped.
* A hop summary always reports how many exchanges contributed a value
  (`dwells_observed`), how many did not report it at all (`dwells_missing`) and
  how many reported it without a usable value (`dwells_unsupported`), plus
  `coverage_ppm`.
* An empty window is `unknown`, never a zero distribution.
* Current mode admits only usable evidence; historical mode admits stale and
  expired evidence and still reports its freshness per bucket.

## Baselines

A baseline is bound to a path, a generation, a clock domain, a histogram
specification, a probe set and a revision. Comparing it with a summary that
disagrees on any of those produces an explicit `BaselineMismatch` kind
(`path`, `generation`, `clock_domain`, `histogram`, `probes`, `revision`,
`stale`, `expired`, `missing`, `not_usable`) and no delta at all. A store
lookup that only finds baselines of another generation fails with a conflict,
not with "no baseline".

## Attribution

A decomposition is produced only when all of the following hold:

1. at least one contributing source declares `hop_dwell` semantics;
2. every participating clock domain is comparable with the exchange domain;
3. every declared hop of an exchange was observed with a usable dwell (otherwise
   the exchange is incomplete);
4. no hop failed to produce any value at all.

If (1) fails the result is `unsupported`; if (2) or (4) fail it is `refused`;
if (3) fails it is `refused` unless
`AttributionPolicy::allow_partial_decomposition` is enabled, in which case the
result is a `partial_decomposition` that still reports the incomplete exchange
count. Whenever the result is refused or unsupported, every share is zero and no
accounted time is published: a share without a decomposition would be an
attribution the semantics do not permit.

For a produced decomposition the runtime reports:

* the end to end mean over the complete exchanges,
* the sum of the hop means over the same exchanges (`accounted_mean_ns`),
* the residual (`unaccounted_mean_ns`) together with its minimum and maximum,
  including when it is negative,
* each hop's share in parts per million, computed exactly from the sums, and its
  coverage,
* the reason `residual_within_tolerance` or `unaccounted_residual` from
  `AttributionPolicy::residual_tolerance_ns`, and `hop_overlap` when hops claim
  more time than the exchange took.

## Causality

The runtime has no representation of a cause. A deviation is reported as an
`AnomalyEvidence`: the classification (`none`, `watch`, `elevated`,
`suppressed`, `suppressed_elevated`, `unknown`), the mean and p99 deltas, the
ratio, the sample counts, the thresholds from `AnomalyPolicy` and the reason
`no_causal_inference`. Too few samples is `unknown`, never "no deviation".
