# Latency Observatory

Latency Observatory is a standalone, vendor neutral Fabric OS runtime that owns
end to end and per hop latency **observation** and **attribution**. It is a C++20
infrastructure project with no external dependencies: the measurement path, the
aggregation, the persistence format, the worker pool and the network transport
are all implemented here.

It does not route traffic, enforce latency SLOs, control queues, or infer
causality. It records what was observed, by which source, for which generation,
at which observation and receive times, under which source incarnation, and it
states whether that evidence is fresh, stale, conflicting, incomplete,
unsupported or unknown.

## What it does

* **Typed identity everywhere.** Measurements, paths, hops, links, queues,
  endpoints, sources, generations, epochs, incarnations, clock domains,
  baselines and snapshots are distinct strong types. An identity is derived from
  a canonical name, so the same object has the same identity in every process
  and across restarts.
* **End to end and per hop evidence.** A reported exchange carries an end to end
  observation plus whatever hop evidence the source supplied. Hops that were not
  reported stay unknown; they are never counted as zero.
* **Clock domain gating.** A duration measured inside one clock domain is always
  meaningful. A duration that crosses domains is only derived when both domains
  are comparable: synchronized to the same reference, fresh, generation matched,
  and within the configured uncertainty ceiling. Otherwise no value is produced
  and the evidence says why.
* **Deterministic aggregation.** Count, minimum, maximum, exact sum, floor mean
  with an explicit remainder, exact rational nearest rank quantiles and an
  explicit histogram. The same set of observations produces byte identical
  output regardless of the order in which the observations arrived.
* **Generation bound baselines.** A baseline is bound to a path, a generation, a
  clock domain, a histogram bucketing and a probe set. Applying it anywhere else
  is refused with an explicit mismatch kind, never silently compared.
* **Anomaly evidence without causality.** A deviation from a baseline is a
  statistical statement with the sample counts, the deltas and the thresholds
  that produced it. Every anomaly and every attribution carries the
  `no_causal_inference` reason.
* **Versioned, integrity checked persistence.** Segments with a magic header, a
  format version, per record CRC-32C and a checksummed manifest. Recovery is
  conservative: reading stops at the first damaged record and reports it.
* **A restart never makes history current.** Loaded evidence keeps its original
  observation and receive times; freshness is recomputed for the new session, so
  persisted evidence is history, not current timing.
* **Bounded everything.** Workers, queue depth, batch size, samples per path,
  stored records, histogram buckets, history buckets, export size, JSON depth,
  payload size and store size are all bounded, with typed errors when a bound is
  reached.
* **Real transport.** A line oriented TCP transport speaks the same request and
  response protocol as the command line tool, and it is exercised from
  independent processes in the test suite.

## Quick start

```console
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The tool is then available at `build/tools/latobs`:

```console
latobs version
latobs capabilities                      # the REAL / SYNTHETIC / UNSUPPORTED matrix
latobs demo --samples 256 --seed 7       # labelled synthetic scenario
latobs request --request '{"op":"operations"}'
latobs serve --store /var/lib/latobs --port 7100 --announce-stdout
latobs client --port 7100 --request @request.json
```

## Embedding

```cpp
#include "latency_observatory/runtime/engine.hpp"

latobs::runtime::RuntimeConfig config;
config.store_directory = "/var/lib/latobs";
auto engine = latobs::runtime::Engine::create(config).value();
// define sources, generations, hops and paths, then ingest observations
auto report = engine->ingest(std::move(request)).value();
auto summary = engine->summarize(summary_request).value();
```

The installed package exports `latobs::core`, `latobs::model`, `latobs::stats`,
`latobs::ingest`, `latobs::baseline`, `latobs::attribute`, `latobs::store` and
`latobs::runtime`:

```cmake
find_package(LatencyObservatory 1.0 REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE latobs::runtime)
```

`examples/quickstart.cpp` uses only the public C++ API;
`examples/service_client.cpp` drives the same request protocol the network
transport uses. `tests/downstream` is an independent consumer that is
configured, built and run against the installed package by the test suite.

## Repository layout

| Path | Contents |
| --- | --- |
| `include/latency_observatory/core` | identities, checked arithmetic, digests, time, evidence, policy, canonical JSON |
| `include/latency_observatory/model` | entities, catalog, clock registry, measurements and their codecs |
| `include/latency_observatory/stats` | distributions, histograms, quantiles, summaries, history bucketing |
| `include/latency_observatory/ingest` | admission gate: validation and every staleness, replay and generation fence |
| `include/latency_observatory/baseline` | generation bound baselines, comparisons and anomaly statements |
| `include/latency_observatory/attribute` | decomposition with the semantics gate and the residual |
| `include/latency_observatory/store` | versioned, integrity checked persistence and conservative recovery |
| `include/latency_observatory/runtime` | engine, worker pool, service, transport, explanation, export |
| `tools` | the `latobs` command line tool |
| `examples`, `benchmarks` | usage examples and completed work benchmarks |
| `tests` | unit, property, adversarial, concurrency, restart, transport, CLI and package tests |
| `docs` | architecture, semantics, persistence format, concurrency audit, testing and boundaries |

## Documentation

* [docs/architecture.md](docs/architecture.md) - layers, identities and data flow
* [docs/semantics.md](docs/semantics.md) - evidence states, fences, comparability, attribution rules
* [docs/persistence.md](docs/persistence.md) - store format, recovery rules, restart semantics
* [docs/concurrency.md](docs/concurrency.md) - locking, worker pool and the deadlock audit
* [docs/testing.md](docs/testing.md) - how to build, run and read the test suite
* [docs/boundaries.md](docs/boundaries.md) - the REAL, SYNTHETIC and UNSUPPORTED proof surfaces

## Limitations

* Observation is only as good as the sources that declare it. The runtime never
  invents a topology, a hardware counter or a clock relationship.
* Comparability between clock domains is single level: a chain of
  synchronization reports is not treated as evidence, so a domain synchronized
  to another non reference domain is not comparable transitively.
* Store recovery is conservative rather than repairing: a damaged tail is
  discarded and reported, never reconstructed.
* The worker pool executes runtime requests; it does not shard the evidence
  itself, which lives behind one shared mutex with shared (reader) parallelism.
* The synthetic scenario in `latobs demo` is generated in process. It proves the
  pipeline, not any fabric behaviour.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
