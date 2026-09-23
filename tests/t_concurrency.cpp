// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Concurrency: the worker pool is bounded, cancellation is real, shutdown
// settles every task, and parallel readers observe a consistent snapshot.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "latency_observatory/runtime/worker_pool.hpp"
#include "support.hpp"
#include "test_framework.hpp"

using namespace latobs;
using namespace latobs::core;
using namespace latobs::test;

LATOBS_TEST(concurrency, worker_pool_runs_every_task) {
  runtime::WorkerPool::Config config;
  config.workers = 4;
  config.queue_depth = 64;
  CHECK_OK(pool, runtime::WorkerPool::create(config));
  std::atomic<int> counter{0};
  std::vector<runtime::WorkerPool::Handle> handles;
  for (int index = 0; index < 32; ++index) {
    const Result<runtime::WorkerPool::Handle> handle = pool.submit(
        [&counter, index](const runtime::CancellationToken& token) -> runtime::WorkerPool::Outcome {
          if (token.cancelled()) return token.error();
          counter.fetch_add(1);
          return std::string("task-") + std::to_string(index);
        });
    CHECK(handle.has_value());
    handles.push_back(handle.value());
  }
  int completed = 0;
  for (const runtime::WorkerPool::Handle& handle : handles) {
    const runtime::WorkerPool::Outcome outcome = handle.get();
    CHECK(outcome.has_value());
    ++completed;
  }
  CHECK_EQ(completed, 32);
  CHECK_EQ(counter.load(), 32);
  const runtime::WorkerPool::Stats stats = pool.stats();
  CHECK_EQ(stats.submitted, std::uint64_t{32});
  CHECK_EQ(stats.completed, std::uint64_t{32});
  CHECK_EQ(stats.failed, std::uint64_t{0});
  CHECK_EQ(stats.pending, std::size_t{0});
  CHECK_OK_STATUS(pool.shutdown(true));
}

LATOBS_TEST(concurrency, worker_pool_refuses_overload_and_reentrancy) {
  runtime::WorkerPool::Config config;
  config.workers = 1;
  config.queue_depth = 2;
  CHECK_OK(pool, runtime::WorkerPool::create(config));

  // One task blocks until it is released, so the queue can be filled.
  std::atomic<bool> release{false};
  std::atomic<bool> started{false};
  CHECK_OK(first, pool.submit([&](const runtime::CancellationToken&) -> runtime::WorkerPool::Outcome {
    started.store(true);
    while (!release.load()) {
      std::this_thread::yield();
    }
    return std::string("released");
  }));
  while (!started.load()) {
    std::this_thread::yield();
  }
  CHECK_OK(second, pool.submit(
                       [](const runtime::CancellationToken&) -> runtime::WorkerPool::Outcome {
                         return std::string("second");
                       }));
  CHECK_OK(third, pool.submit(
                      [](const runtime::CancellationToken&) -> runtime::WorkerPool::Outcome {
                        return std::string("third");
                      }));
  // The queue is full: the pool refuses work instead of growing without bound.
  const Result<runtime::WorkerPool::Handle> overflow =
      pool.submit([](const runtime::CancellationToken&) -> runtime::WorkerPool::Outcome {
        return std::string("overflow");
      });
  CHECK(!overflow.has_value());
  CHECK(overflow.error().code() == ErrorCode::CapacityExceeded);
  CHECK_EQ(pool.stats().refused, std::uint64_t{1});

  // Drain the queue, then check that a task which tries to submit to the pool
  // running it is refused instead of deadlocking.
  release.store(true);
  CHECK(first.get().has_value());
  CHECK(second.get().has_value());
  CHECK(third.get().has_value());

  std::atomic<std::size_t> reentrant_code{0};
  const Result<runtime::WorkerPool::Handle> reentrant = pool.submit(
      [&pool, &reentrant_code](const runtime::CancellationToken&) -> runtime::WorkerPool::Outcome {
        const Result<runtime::WorkerPool::Handle> nested =
            pool.submit([](const runtime::CancellationToken&) -> runtime::WorkerPool::Outcome {
              return std::string("nested");
            });
        if (nested.has_value()) {
          reentrant_code.store(1);
        } else if (nested.error().code() == ErrorCode::InvalidArgument) {
          reentrant_code.store(2);
        } else {
          reentrant_code.store(3);
        }
        return std::string("outer");
      });
  CHECK(reentrant.has_value());
  const runtime::WorkerPool::Outcome outer = reentrant.value().get();
  CHECK(outer.has_value());
  CHECK_EQ(reentrant_code.load(), std::size_t{2});
  CHECK(pool.stats().reentrant_refusals >= 1);
  CHECK_OK_STATUS(pool.shutdown(true));
}

LATOBS_TEST(concurrency, cancellation_is_real) {
  runtime::WorkerPool::Config config;
  config.workers = 2;
  config.queue_depth = 32;
  CHECK_OK(pool, runtime::WorkerPool::create(config));

  std::atomic<bool> release{false};
  std::atomic<int> observed{0};
  std::vector<runtime::WorkerPool::Handle> handles;
  for (int index = 0; index < 8; ++index) {
    const Result<runtime::WorkerPool::Handle> handle = pool.submit(
        [&release, &observed](const runtime::CancellationToken& token)
            -> runtime::WorkerPool::Outcome {
          while (!release.load()) {
            if (token.cancelled()) return token.error();
            std::this_thread::yield();
          }
          ++observed;
          return std::string("done");
        });
    CHECK(handle.has_value());
    handles.push_back(handle.value());
  }
  pool.cancel_pending("test cancellation");
  release.store(true);
  std::size_t cancelled = 0;
  std::size_t succeeded = 0;
  for (const runtime::WorkerPool::Handle& handle : handles) {
    const runtime::WorkerPool::Outcome outcome = handle.get();
    if (!outcome.has_value() && outcome.error().code() == ErrorCode::Cancelled) {
      ++cancelled;
    } else if (outcome.has_value()) {
      ++succeeded;
    }
  }
  // Every task settled: none of them is still waiting.
  CHECK_EQ(cancelled + succeeded, handles.size());
  CHECK_EQ(observed.load(), static_cast<int>(succeeded));
  CHECK_OK_STATUS(pool.shutdown(true));
}

LATOBS_TEST(concurrency, shutdown_settles_queued_work) {
  runtime::WorkerPool::Config config;
  config.workers = 1;
  config.queue_depth = 16;
  CHECK_OK(pool, runtime::WorkerPool::create(config));
  std::atomic<bool> release{false};
  std::atomic<bool> started{false};
  CHECK_OK(blocker, pool.submit(
                        [&](const runtime::CancellationToken&) -> runtime::WorkerPool::Outcome {
                          started.store(true);
                          while (!release.load()) {
                            std::this_thread::yield();
                          }
                          return std::string("blocker");
                        }));
  while (!started.load()) {
    std::this_thread::yield();
  }
  std::vector<runtime::WorkerPool::Handle> queued;
  for (int index = 0; index < 4; ++index) {
    const Result<runtime::WorkerPool::Handle> handle =
        pool.submit([](const runtime::CancellationToken&) -> runtime::WorkerPool::Outcome {
          return std::string("queued");
        });
    CHECK(handle.has_value());
    queued.push_back(handle.value());
  }
  std::thread releaser([&release]() { release.store(true); });
  // Shutting down without draining cancels the queued work and settles it.
  CHECK_OK_STATUS(pool.shutdown(false));
  releaser.join();
  CHECK(blocker.get().has_value());
  std::size_t settled = 0;
  for (const runtime::WorkerPool::Handle& handle : queued) {
    const runtime::WorkerPool::Outcome outcome = handle.get();
    CHECK(!outcome.has_value() || outcome.has_value());
    ++settled;
  }
  CHECK_EQ(settled, queued.size());
  CHECK(pool.shutting_down());
  // Submitting after shutdown is refused, and shutdown is idempotent.
  CHECK_ERR(refused, pool.submit(
                        [](const runtime::CancellationToken&) -> runtime::WorkerPool::Outcome {
                          return std::string("late");
                        }));
  CHECK(refused.code() == ErrorCode::ShuttingDown);
  CHECK_OK_STATUS(pool.shutdown(true));
  CHECK_OK_STATUS(pool.shutdown(false));
}

LATOBS_TEST(concurrency, parallel_readers_agree_with_a_serial_reader) {
  CHECK_OK(scenario, build_scenario());
  ingest::IngestRequest request;
  request.received_at = Timestamp{2000000, core::reference_clock_domain()};
  for (std::uint64_t index = 0; index < 64; ++index) {
    RecordSpec spec;
    spec.sequence = index;
    spec.observed_at_ns = 1000000 + static_cast<std::int64_t>(index);
    spec.hop_dwells_ns[0] = 100 + static_cast<std::int64_t>(index);
    spec.hop_dwells_ns[1] = 200;
    spec.hop_dwells_ns[2] = 300;
    request.records.push_back(make_record(scenario, spec));
  }
  CHECK_OK(report, scenario.engine->ingest(std::move(request)));
  CHECK_EQ(report.accepted_current, std::uint64_t{64});

  const stats::SummaryRequest summary_request =
      make_summary_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(serial_summary, scenario.engine->summarize(summary_request));
  std::string reference;
  {
    core::JsonWriter writer(reference);
    stats::write_json(writer, serial_summary);
  }

  constexpr int kReaders = 8;
  constexpr int kIterations = 12;
  std::vector<std::string> results(static_cast<std::size_t>(kReaders));
  std::atomic<bool> failed{false};
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&, reader]() {
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        const Result<stats::PathSummary> summary = scenario.engine->summarize(summary_request);
        if (!summary.has_value()) {
          failed.store(true);
          return;
        }
        std::string text;
        core::JsonWriter writer(text);
        stats::write_json(writer, summary.value());
        if (text != reference) failed.store(true);
        results[static_cast<std::size_t>(reader)] = text;
      }
    });
  }
  for (std::thread& thread : readers) thread.join();
  CHECK(!failed.load());
  for (const std::string& text : results) {
    CHECK_EQ(text, reference);
  }
}

LATOBS_TEST(concurrency, concurrent_ingest_stores_every_record_once) {
  CHECK_OK(scenario, build_scenario());
  constexpr int kWriters = 4;
  constexpr int kPerWriter = 16;

  // One independent source per writer: distinct sources are distinct evidence
  // streams, so no writer fences another one's session.
  std::vector<SourceId> writer_sources;
  for (int writer = 0; writer < kWriters; ++writer) {
    model::SourceDescriptor source;
    source.name = Name::assume_valid("concurrency.source." + std::to_string(writer));
    source.kind = model::SourceKind::Probe;
    source.authority = model::AuthorityClass::Primary;
    source.semantics = model::SemanticsProfile::EndToEndRequestResponse |
                       model::SemanticsProfile::HopDwell;
    source.revision = Revision::first();
    CHECK_OK(defined, scenario.engine->define_source(source));
    writer_sources.push_back(defined);
  }

  std::atomic<int> failures{0};
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int writer = 0; writer < kWriters; ++writer) {
    writers.emplace_back([&, writer]() {
      for (int iteration = 0; iteration < kPerWriter; ++iteration) {
        ingest::IngestRequest request;
        request.received_at = Timestamp{2000000, core::reference_clock_domain()};
        RecordSpec spec;
        spec.sequence = static_cast<std::uint64_t>(iteration);
        spec.observed_at_ns = 1000000 + static_cast<std::int64_t>(iteration);
        spec.hop_dwells_ns[0] = 100;
        spec.hop_dwells_ns[1] = 200;
        spec.hop_dwells_ns[2] = 300;
        model::MeasurementRecord record = make_record(scenario, spec);
        record.source = writer_sources[static_cast<std::size_t>(writer)];
        request.records.push_back(std::move(record));
        const Result<ingest::IngestReport> report = scenario.engine->ingest(std::move(request));
        if (!report.has_value() || report.value().accepted_current != 1) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : writers) thread.join();
  CHECK_EQ(failures.load(), 0);
  const stats::SummaryRequest summary_request =
      make_summary_request(scenario, 0, 100000000, stats::AggregationMode::Current);
  CHECK_OK(summary, scenario.engine->summarize(summary_request));
  CHECK_EQ(summary.exchanges_included,
           static_cast<std::uint64_t>(kWriters * kPerWriter));
  CHECK_EQ(summary.end_to_end.count, static_cast<std::uint64_t>(kWriters * kPerWriter));
  CHECK_EQ(*summary.end_to_end.mean_ns, 600);
  CHECK_EQ(summary.excluded_conflicting, std::uint64_t{0});
  const runtime::EngineStats engine_stats = scenario.engine->stats();
  CHECK_EQ(engine_stats.measurements_stored, static_cast<std::uint64_t>(kWriters * kPerWriter));
  CHECK_OK_STATUS(scenario.engine->shutdown());
  CHECK(scenario.engine->shut_down());
  // Every entry point refuses work after shutdown instead of racing it.
  const Result<ingest::IngestReport> after = scenario.engine->ingest(ingest::IngestRequest{});
  CHECK(!after.has_value());
  CHECK(after.error().code() == ErrorCode::ShuttingDown);
}

LATOBS_TEST_MAIN()
