# Concurrency and deadlock audit

## Threading model

* **Engine state** is protected by one `std::shared_mutex`.
  * Writers take it exclusively: definitions, clock synchronization, ingestion,
    baseline creation, flush, shutdown.
  * Readers take it shared: summarize, history, attribute, explain, export,
    stats, recovery, capabilities.
  * Readers therefore run concurrently with each other and are serialised
    against writers.
* **Worker pool** threads execute submitted functions. The pool owns a mutex
  protecting its queue and statistics, and condition variables for "not empty",
  "not full" and "idle".
* **Transport** runs one thread per accepted connection, bounded by
  `max_connections`, plus the accept loop.

## Lock order

The only nested acquisition in the repository is:

```
engine mutex  ->  worker pool mutex        (Engine::stats)
store I/O     ->  no other lock            (the writer is only touched while the
                                            engine mutex is held)
```

The worker pool never takes the engine mutex, and no lock is held while a task
runs: the pool releases its mutex before invoking a function. There is no path
that acquires the engine mutex while holding the pool mutex, so the order above
cannot be inverted.

## Reentrancy audit

* `std::shared_mutex` is not recursive. Every public engine entry point takes
  the lock exactly once and then calls private helpers suffixed `_locked`
  (`records_locked`, `explain_locked`, `export_locked`), which assume the
  caller holds it.
* Explicitly non-reentrant paths that were checked:
  * `attribute` needs the summary for the baseline comparison. It calls
    `stats::summarize` directly under the lock it already holds rather than
    re-entering `Engine::summarize`.
  * `Engine::history` and `Engine::export_data` build their own requests and do
    not call back into the engine.
  * `Engine::shutdown` takes `shutdown_mutex_`, releases it, then shuts the pool
    down and finally takes the engine mutex. The pool shutdown waits for workers
    that never take the engine mutex, so the wait cannot deadlock.
* `Service::execute` holds no lock of its own: it calls engine entry points,
  each of which takes the lock once.

## Worker pool rules

1. **Bounded queue.** `submit` fails with `capacity_exceeded` when the queue is
   full. Nothing grows without bound.
2. **No reentrant submission.** A task that submits to the pool running it is
   refused with `invalid_argument` (tracked with a thread local identifying the
   pool). This removes the classic "worker waits for a task that can only run on
   the same pool" deadlock by construction.
3. **Every task settles.** A task's promise is set exactly once, either by the
   worker (value, error or cancellation) or by the shutdown path. `Handle::get`
   never throws and never blocks forever: a task whose promise is broken is
   reported as an error value.
4. **Real shutdown.** `shutdown(true)` drains the queue and joins the workers;
   `shutdown(false)` cancels the queued work and settles it with
   `cancelled`, then joins. Shutdown is idempotent, and submission after
   shutdown is refused with `shutting_down`.
5. **Destruction is safe.** The pool state destructor stops the workers, settles
   every queued task and joins every thread, so no joinable thread and no pending
   promise can survive the pool.
6. **Cancellation is cooperative and real.** `cancel_pending` cancels the token
   shared by the queued work; tasks check the token at their checkpoints. A
   cancelled task reports `cancelled`, never a partial result.

## Where tests pin this down

| Property | Test |
| --- | --- |
| every submitted task completes and is counted | `t_concurrency.worker_pool_runs_every_task` |
| overload is refused, reentrant submission is refused | `t_concurrency.worker_pool_refuses_overload_and_reentrancy` |
| cancellation settles every task exactly once | `t_concurrency.cancellation_is_real` |
| abort shutdown settles queued work, shutdown is idempotent | `t_concurrency.shutdown_settles_queued_work` |
| parallel readers agree byte for byte with a serial reader | `t_concurrency.parallel_readers_agree_with_a_serial_reader` |
| concurrent ingestion stores every record exactly once | `t_concurrency.concurrent_ingest_stores_every_record_once` |
| transport serves concurrent connections | `t_transport_e2e.serves_requests_over_a_real_socket` |

Tests never use a timeout to decide an outcome. The concurrency tests
synchronise with latches and join every thread, so a real deadlock would hang the
suite rather than pass it.
