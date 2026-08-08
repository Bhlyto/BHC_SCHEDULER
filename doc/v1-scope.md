# BHCScheduler v1 scope and decisions

## Supported product path

```text
API client -> batch -> jobs -> deterministic queue -> single-worker allocation
           -> local/SSH execution -> logs/output -> artifact metadata -> API
```

The scheduler is the source of truth. Workers are agentless for v1: they are
declared in `provisioning.json` or through the admin API, probed by the
scheduler, and receive commands through SSH. A job owns resources on exactly
one worker. Distribution means scheduling the independent jobs of a batch
across several workers; BHCScheduler does not pretend to turn an arbitrary
single process into an MPI job.

The persistence model is SQLite in serialized/WAL mode. Artifact bytes stay on
a local or shared filesystem; SQLite stores their type, URI, size, optional
checksum, and creation date. This boundary permits a later object-store backend
without coupling it to the scheduler.

## Decisions

| Priority | Problem before migration | V1 decision | Benefit | Cost / risk |
|---|---|---|---|---|
| P0 | Strict C11 build failed | Move nested functions to file scope and test every target | Reproducible Windows/Linux build | None material |
| P0 | Cancel/timeout changed DB state without killing work | Track process trees and terminate them | No orphan simulations or leaked capacity | OS-specific lifecycle code |
| P0 | Shutdown could close SQLite while threads used it | Stop scheduler, terminate/drain executor, then close DB | Deterministic shutdown | Shutdown may wait up to 30 s |
| P0 | Allocation records exhausted after 1024 historical jobs | Grow and reuse allocation records; persist releases transactionally | Long-running service supports repeated campaigns | Allocation history remains in SQLite until operational cleanup |
| P0 | Restart lost queued jobs and active allocations | Reload queued jobs; fail interrupted work; release allocations | Predictable crash recovery | Running work is not resumed in v1 |
| P0 | A blocked priority job starved runnable work | Select the highest-priority compatible job | Work-conserving deterministic queue | Strict priority is relaxed only when the head cannot run |
| P0 | “Multi-machine job” launched only on the first reserved host | One job = one worker; distribute batches | Correct semantics and 262 fewer lines | MPI-style jobs are deferred |
| P0 | No large atomic submission | Transactional batches, maximum 1000 jobs | One request for a ShardSim campaign | Request body capped at 16 MB |
| P0 | Results were only discovered by directory scans | Persistent Artifact metadata and recursive collection | Stable result API and backend boundary | Checksums remain optional |
| P0 | Seven public states included transitional implementation detail | Six states: CREATED, QUEUED, RUNNING, SUCCEEDED, FAILED, CANCELLED | Smaller client state machine | Legacy STARTING rows are migrated on restart |
| P1 | Retry could race a cancelled process or stale queue entry | Retry same ID only after exit; clear prior outputs/artifacts | Safe operational recovery | Automatic retry policy is deferred |
| P1 | API used historical names | Canonical `/workers`, `/queue`, `/cancel`, `/retry`, `/logs`, `/artifacts` | Small client-facing contract | Old routes remain aliases during migration |

## Explicitly outside the v1 support contract

The following code is disabled by default through
`experimental_features_enabled = 0`:

- saved workflow/DAG facilities and same-machine workflow helpers;
- cloud provisioning and autoscaling;
- Wake-on-LAN controls;
- presimulation/decision-core automation;
- the legacy web UI (`web_ui_enabled = 0` independently).

They are retained temporarily to avoid a destructive migration before users
have confirmed that no deployment depends on them. They should be removed or
extracted after the v1 branch is accepted. Kubernetes, multi-master HA,
distributed consensus, billing, advanced accounting, and a custom distributed
storage system are not planned for v1.

## Validation and release gates

Automated gates:

- strict C11 build succeeds;
- all 11 CTest targets pass, including lifecycle, recovery, allocator, queue,
  batch, artifact, retry, upstream unit tests, and the scale proof;
- `scale_1000` submits and executes 1000 real local processes in one batch,
  requires 1000 `SUCCEEDED`, and verifies collected artifacts;
- `git diff --check` is clean.

The scale proof measured 10.73 seconds after the upstream-hardening merge on
the development Windows machine on
2026-08-08. This number is evidence of completion, not a portable performance
guarantee.

## Upstream hardening integrated before release

The branch merges `origin/main` hardening before any v1 tag. The integration
keeps upstream authentication, CORS/HTTPS validation, idempotent submissions,
operational metrics, safe registry snapshots, transactional allocation writes,
and the additional unit-test target. Where the two branches disagreed, the v1
contract remains authoritative: six public states, one worker per job,
process-tree cancellation, filesystem artifact metadata, bounded anti-starvation
queue scans, and experimental decision-core/cloud/workflow behavior disabled by
default. This ordering avoids tagging a v1 that is already behind the mainline
security baseline.

Manual gates before the final v1 tag:

1. run the same representative ShardSim command, first with 100 scenarios;
2. run it with 1000 scenarios on one machine;
3. repeat on two real SSH workers and verify both receive jobs;
4. optionally repeat on three to five workers;
5. verify output content, not only exit status and artifact presence;
6. back up and reopen a production-like SQLite database to exercise migration;
7. confirm that no deployment requires the disabled experimental endpoints.

The v1 tag must not claim multi-worker validation until gate 3 has been run on
real infrastructure.

## Incremental change ledger

The `codex/v1-simplification` branch intentionally uses one reviewable commit
per decision:

1. `18cffe6` restore strict C11 compatibility;
2. `6fd5b13` register the CTest baseline;
3. `ab79bf9` enforce cancellable process lifecycle;
4. `ffe793a` drain scheduler/executor on shutdown;
5. `17405df` make allocations reusable and durable;
6. `1fbb0ef` recover persisted jobs after restart;
7. `eb2aaf1` prevent queue starvation;
8. `c974e86` add transactional 1000-job batches;
9. `7950332` persist filesystem artifact metadata;
10. `7f60e52` enforce one worker per job;
11. `7d06ad1` expose the minimal worker and queue API;
12. `cc1e104` reduce jobs to six v1 states;
13. `7b45da0` retry terminal jobs safely;
14. `57244e2` execute a 1000-job campaign in CI/local tests;
15. `e3a8061` document and enforce the v1 support scope;
16. the final merge commit integrates upstream hardening and records the
    conflict-resolution decisions above.
