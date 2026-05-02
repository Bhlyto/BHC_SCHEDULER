**Python solvers — template and thermal example**

Purpose: allow presim solvers to be developed and run without recompiling the orchestrator. Each solver is a small script/executable that reads job inputs and writes `presim.json`.

Location: `tools/python_solvers/`

Provided files:
- `template_solver.py` — a minimal template to implement other domains.
- `thermal_presim.py` — a thermal presim implementation (deterministic synthetic estimator). Accepts `--model` to plug in ML models.

Usage as an orchestrator job:
- Submit a presim job (POST /jobs) with command:
  - `python3 tools/python_solvers/thermal_presim.py --parent <PARENT_ID> --out jobs/<PARENT_ID>/input/presim.json`
- Orchestrator should dispatch this job like any other; on completion move or copy the produced `presim.json` into the parent input dir and call the decision core.

Example presim invocation (CLI):
```bash
python3 tools/python_solvers/thermal_presim.py --parent 1234-abcd --out jobs/1234-abcd/input/presim.json
```

Notes and recommendations:
- Keep Python solvers lightweight; use worker machines (not the scheduler process) to run heavy ML or numerical code.
- Use a virtualenv or container image per solver to manage dependencies. Orchestrator jobs can specify machine tags or `app_id` to route to compatible machines.
- Provide unit tests for solver scripts and a small wrapper to validate `presim.json` conforms to the schema.

Docker / packaging template
- A Dockerfile template is available at `tools/python_solvers/Dockerfile.template` and a `requirements.txt` next to your solver.
- Build a solver image like:
  ```bash
  cd tools/python_solvers
  cp Dockerfile.template Dockerfile
  # edit requirements.txt as needed
  docker build -t myorg/thermal-presim:latest .
  ```

Routing presim jobs to specific machines
- Presim jobs are created with `app_id` set to `presim`. To route these jobs to a dedicated worker pool, add machines in `config/provisioning.json` with a `tags` field containing `presim` and configure the allocator to prefer machines whose tags match the job `app_id` or provided `tags` metadata.
- The scheduler currently sets the child presim job's `app_id` to `presim` (see `src/core/scheduler.c`). Ensure your provisioning entries have `presim` in their tags so presim jobs land on intended machines.

