# Background Jobs

`kin/core/jobs.hpp` runs work on a pool of worker threads: long computations
in the background, and loops split across cores.

```cpp
kin::JobSystem& jobs = kin::default_job_system(); // the pool kin itself uses
kin::JobSystem mine;                              // or a separate one
```

Most games should use `kin::default_job_system()`: the ECS scheduler, the asset
server and the path server all run on it, so the whole program has one worker
per core instead of each system starting its own threads.

## Background jobs

`run()` starts work on a worker and returns a `kin::Job<T>` handle:

```cpp
kin::Job<ShadowMap> job = jobs.run([terrain] { return compute_shadows(terrain); });

// In a later frame:
if (job.ready()) {
    _shadows = std::move(job.get());
}
```

`get()` waits if the job is not done, and rethrows any exception the work threw.
Work should capture copies or data nothing else writes while the job runs;
the job system does not guard shared state.

### Applying results in order

Polling `ready()` makes game state depend on thread timing. Pass a second
function to `run()`, and it is called with the result in `pump()` on your thread:

```cpp
jobs.run([terrain] { return compute_shadows(terrain); },
         [this](ShadowMap& shadows) { _shadows = std::move(shadows); });

// Once per frame, on the main thread:
jobs.pump();
```

`pump()` applies finished jobs strictly in the order they were started. It stops
at the first job still running, even if later ones are done. The order results are
applied is therefore the same on every run. Which frame a result lands in still
depends on timing, so headless runs, tests and replays call `drain()` at fixed
points: it waits for every job, including ones started by those callbacks, and
applies them all. The asset and path servers follow the same pattern.

If a job with a callback throws, `pump()` or `drain()` rethrows the exception and
skips the callback.

### Cancelling

`job.cancel()` stops a job that has not started yet; it never runs, and `get()`
throws. To stop a running job, write the work to take a `std::stop_token`:

```cpp
kin::Job<Path> search = jobs.run([=](std::stop_token stop) {
    return plan_route(world, stop);   // check stop.stop_requested() now and then
});
search.cancel();                        // e.g. the player picked another target
```

Destroying the `JobSystem` cancels queued jobs, requests stop on running ones and
waits for them.

## Parallel loops

`parallel_for(count, body)` calls `body(i)` for every `i` in `[0, count)`, spread
over the workers and the calling thread, and returns when all are done:

```cpp
jobs.parallel_for(map.rows, [&](kin::i32 row) { blur_row(map, row); });
```

A `parallel_for` inside another runs serially. If any call throws, the first
exception is rethrown after the loop.

## Sharing workers

Background jobs may use at most `max_background` workers at once (half of them by
default). `parallel_for` always takes priority, so a long background job never
leaves a frame's parallel loop without threads. The ECS scheduler's parallel
systems run through `parallel_for`; asset loads and path searches are background
jobs.

A job system busy with other work never stalls a frame: `AssetServer::drain()`
and `PathServer::drain()` do queued work on the calling thread while they wait,
and apply results in request order whoever computed them.

To size the shared pool yourself, install one before creating worlds or servers
(they keep the pool they started with) and keep it alive while they exist:

```cpp
kin::JobSystem jobs{{.workers = 6, .max_background = 2}};
kin::set_default_job_system(&jobs);
```

Or give a single system its own pool: `world.systems().set_job_system(pool)`,
or the `jobs` field of `AssetServerConfig` and `PathServerConfig`.

`jobs.stats()` reports queued, running and not-yet-applied jobs.
