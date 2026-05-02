**Presimulation workflow**

- **Purpose:** run a lightweight presimulation on a worker to produce an error map that guides refinement and resource allocation.
- **One solver = one domain:** each domain (thermal, fluid, structural, ...) implements a solver plugin that knows how to read job inputs and produce a standardized presim JSON.

Workflow (thermal example):

- User submits main job (POST /jobs) with `app_id` or flag indicating presim is required, e.g. `app_id: thermal` or CLI `--requires-presim`.
- Orchestrator creates a small presim subjob:
  - command: `presim_runner --parent <PARENT_ID> --case thermal --out presim.json --input-dir <parent_input_dir>`
  - `app_id` or `tags`: `presim` (so routing and logs can identify presim jobs)
- Worker runs presim_runner which dispatches to the `thermal` solver to compute a per-zone error map and writes `presim.json` in its job output directory.
- On presim job completion, orchestrator ingests the `presim.json` (moves it to the parent job input or passes it to the decision core) and re-invokes the decision core for the parent job.
- Decision core returns allocation plan and action (REFINE/COARSE/FINE) which the scheduler applies (create refinement subjobs or schedule parent run).

Presim JSON schema (recommended minimal):

{
  "parent_job_id": "<uuid>",
  "case": "thermal",
  "method": "thermal_presim_v1",
  "timestamp": "TIMESTAMPZ",
  "error_threshold": 0.05,
  "zones": [
    {"zone":0, "error":0.061, "size":1.0},
    {"zone":1, "error":0.023, "size":2.0}
  ],
  "zone_errors": [0.061, 0.023],
  "zone_sizes": [1.0, 2.0],
  "notes": "coarse thermal estimate"
}

Notes:
- `zones` array is recommended for human readability; `zone_errors` and `zone_sizes` are convenience arrays used by the decision core.
- Solvers should fill `parent_job_id` and `case` fields.
- Solvers may include `method` and `notes` to aid debugging.

Integration tips:
- The presim_runner binary is provided in `src/tools/presim_runner.c` and linked as `presim_runner` in the build.
- The `thermal` solver implementation is in `src/solvers/thermal/thermal_solver.c` and is a model for adding other domain solvers.

Tooling & config
- `config/presim.conf`: presim-specific tuning parameters (thresholds, multipliers, fidelity map). The orchestrator reads `presim.conf` at startup if present. Use `tools/presim_calibrate.py` to run grid searches that write `config/presim.conf` automatically.
- `tools/presim_calibrate.py`: runs `build/bin/presim_e2e_test` across parameter combinations and collects full vs presim error/runtimes. Use `--use-openfoam` to enable the OpenFOAM wrapper when available.
- `tools/openfoam_presim_wrapper.sh`: optional wrapper to invoke OpenFOAM and synthesize `presim.json` (falls back to `tools/python_solvers/thermal_high_fidelity.py` when OpenFOAM is unavailable).
- `scripts/anonymize_repo.py`: helper to sanitize personal data (paths, emails, API keys) from the repository before sharing. Dry-run by default; pass `--apply` to modify files (creates `.bak` backups).

Running a calibration (example):
```
export RUN_OPENFOAM=1
python3 tools/presim_calibrate.py --runs 3 --use-openfoam
```

Notes on presim JSON produced by external tools:
- Avoid embedding absolute home paths or timestamps with identifiable info; `scripts/anonymize_repo.py --apply` can help sanitize outputs before committing them to a repo or sharing.
