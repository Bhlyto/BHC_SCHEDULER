# V1 localhost bulk qualification

This release record qualifies the v1 scheduler on the only worker currently
available to the project: `local` (`127.0.0.1`), with four scheduler cores.
It does not claim that the SSH multi-worker path has been operationally tested.

## Synthetic replacement for ShardSim

`bhc_synthetic_workload` replaces the solver during scheduler qualification.
Each job receives an index and a reproducible campaign seed, derives two
operands and a pseudo-random sleep duration, then computes:

```text
result = (operand_a + operand_b) * multiplier
```

The default sleep range is 1 through 60 seconds. Every execution:

- writes `START` and `RESULT` markers to stdout;
- writes its seed and operands to stderr;
- writes `result.json` under `ORCH_OUTPUT_DIR`;
- records the job and worker identities, inputs, result, sleep, and timestamps.

This payload gives variable execution time without making the evidence
non-reproducible. The campaign runner independently recalculates the algebraic
result and rejects missing or inconsistent data.

## Reproduce the campaign

Build and start BHC Scheduler, then generate an admin key:

```powershell
cmake --build build --config Debug --target orchestrator bhc_synthetic_workload
.\build\bin\Debug\orchestrator.exe keygen --label v1-local-qualification
$env:API_KEY = "<generated-key>"
.\build\bin\Debug\orchestrator.exe --console
```

In a second PowerShell terminal, submit and audit 100 variable jobs:

```powershell
.\tools\run_v1_bulk_qualification.ps1 `
  -Count 100 `
  -Seed 20260808 `
  -MinSleepSeconds 1 `
  -MaxSleepSeconds 60
```

The runner targets `http://127.0.0.1:8099` explicitly because `localhost` may
resolve to IPv6 while the configured daemon listener is IPv4. It writes a
timestamped JSON report under `build/qualification/` and never stores the API
key in that report.

## Checks performed

The run is PASS only when all of the following are true:

1. the complete batch is accepted by one `POST /batches` transaction;
2. queued and running states are both observed;
3. every job reaches `SUCCEEDED` on the expected worker;
4. at least two sleep values are observed when a variable range is requested;
5. every algebraic result is correct and every synthetic index is unique;
6. stdout and stderr markers are present for every job;
7. `result.json` is readable for every job through the output API;
8. stdout, stderr, and output artifact metadata exist for every job.

## Evidence recorded on 2026-08-08

The full CTest suite passed 11/11 immediately before the campaign, including
the independent `scale_1000` capacity proof.

The variable localhost campaign produced:

| Measure | Result |
|---|---:|
| Batch ID | `141ee3e157388324f8c5e7c102c3bf73` |
| Jobs | 100 submitted, 100 succeeded, 0 failed, 0 cancelled |
| Wall time | 800.361 seconds |
| Maximum queued | 96 |
| Maximum running | 4 |
| Sleep range observed | 1–60 seconds |
| Distinct sleep values | 48 |
| Average sleep | 31.08 seconds |
| Accumulated sleep | 3,108 seconds |
| Stdout | 100 files, 17,419 bytes |
| Stderr | 100 files, 4,500 bytes |
| Results | 100 files, 26,646 bytes |
| Artifact metadata | 300 records, 48,565 accumulated bytes |
| Verdict | **PASS** |

The 3,108 seconds of accumulated sleeps completed in about 800 seconds because
the scheduler kept the four advertised localhost cores busy. This is evidence
of queueing and work-conserving execution, not a portable performance promise.

## Release interpretation

V1 combines two complementary load gates:

- `scale_1000` proves the maximum 1,000-job batch capacity with short real
  processes;
- this 100-job variable campaign proves realistic queue pressure and audits
  every log, result, and artifact.

Together they qualify the `v1.0.0` localhost release. SSH distribution remains
implemented but is explicitly unqualified until two real workers are
available; that later evidence must not be retroactively attributed to this
tag.
