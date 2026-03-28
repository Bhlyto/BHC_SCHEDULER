**Decision core — input / output and integration**

Purpose: the decision core receives a `dc_context_t` for a job and returns a `dc_result_t` that contains an action (REFINE / RUN_COARSE / RUN_FINE / RUN_PRE_SIM / DEFER / MIGRATE), a `target_cores` hint and an `allocation_json` describing per-zone fidelity and core requests.

Inputs (dc_context_t):
- `job_id` — the UUID of the main job
- `local_error_estimate` — optional scalar estimate for the whole job
- `available_cpus` / `available_mem_mb` — resource availability hints

Outputs (dc_result_t):
- `action` — one of the DC_ACTION_* enum values
- `target_cores` — how many cores the scheduler should allocate if running/refining
- `allocation_json` — JSON describing zones with fields: `zone`, `error`, `size`, `priority`, `fidelity`, `req_cores`.

Example `allocation_json` (condensed):

{"job_id":"...","zones":[{"zone":0,"error":0.061,"size":1.0,"priority":0.061,"fidelity":2,"req_cores":2},...],"threshold":0.05,"zones_count":8}

Behavior notes:
- Decision core prefers explicit `presim.json` produced by a solver; if absent it may fall back to deterministic heuristics.
- To add a new domain solver, implement `solver_presim_run()` (see `include/solver.h`) and register/link it with `presim_runner`.
- `allocation_json` is stored in `dc_result_t.allocation_json` and the scheduler may persist it to the job DB or status reason field for auditability.

Where to extend:
- `src/decision_core_default.c` implements a presim-based decision strategy that consumes `presim.json` fields `zone_errors` and `zone_sizes`.
- `src/tools/presim_runner.c` and `src/solvers/*` implement the presimulation workers.
