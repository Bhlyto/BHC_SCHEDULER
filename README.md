# BHC Scheduler — Job Orchestrator

A lightweight C daemon for submitting batches, scheduling resource-aware jobs
across a small worker pool, and collecting results. The v1 default is API-only
and uses Mongoose, SQLite, cJSON, SSH, and a filesystem artifact backend.

---

## Features

- **Job queue** — submit commands with resource requirements; the scheduler dispatches them as machines become available
- **Transactional batches** — submit up to 1000 independent jobs atomically and query aggregate progress
- **Priority scheduling** — lower priority value = higher priority (default `50`)
- **Resource-aware allocation** — jobs declare `req_cores`, `req_gpu`, `req_ram_mb`, `req_disk_mb`; the allocator matches them against available capacity
- **Multi-worker batches** — every job runs on exactly one compatible worker; large batches are distributed across the worker pool
- **Machine pools** — define hundreds of machines compactly in `provisioning.json` using prefix/format ranges; add or remove machines at runtime without restart
- **Agentless workers** — run locally or push jobs to a small heterogeneous worker pool over SSH, with scheduler-owned resource state and availability probes
- **Artifact metadata** — persist URIs and sizes for stdout, stderr, and recursively collected output files without turning the scheduler into a storage system
- **CREATED jobs** — submit expected input files; the job stays `CREATED` until inputs arrive, then moves to `QUEUED`
- **Job logs** — stdout and stderr of each job are captured and retrievable via API
- **Pre-job scripts** — run a setup script before each job; receives job context via environment variables
- **Real-time events** — subscribe to a Server-Sent Events (SSE) stream for live job state updates and an initial snapshot
- **Event persistence** — status and dispatch events are recorded for diagnosis
- **Stats endpoint** — aggregated view of job counts by state and cluster resource utilisation
- **Operations endpoints** — Prometheus metrics, drain mode, filtered pagination, and idempotent job submissions
- **User management** — create, update, and delete users with password authentication via admin API
- **Password authentication** — users log in with user ID and password; receive a temporary API key
- **API key management** — create, list, and revoke API keys; bind keys to users with role-based access (admin/user)
- **Quotas** — enforce per-user and per-app limits on job count, RAM, cores, and concurrent jobs
- **Machine availability probing** — background thread checks whether static machines are reachable via TCP, ping, or SSH; offline machines are automatically excluded from scheduling
- **Auto-cleanup** — work directories are deleted automatically after a configurable TTL
- **Bulk purge** — single call to delete all finished/failed/cancelled jobs and their work directories
- **Cross-platform** — runs as a Windows Service or Linux daemon

The legacy web UI, saved workflows, cloud/Wake-on-LAN controls, and
presimulation hooks are compiled for migration compatibility but disabled by
default. They are not part of the v1 support contract. See
[V1 scope and decisions](doc/v1-scope.md). The `v1.0.0` release evidence covers
the localhost topology and is recorded in the
[V1 localhost bulk qualification](doc/v1-local-qualification.md).

---

## Requirements

| Tool | Minimum version |
|---|---|
| CMake | 3.16 |
| Visual Studio / MSVC (Windows) | VS 2019+ (toolset v142+) |
| GCC + Make (Linux) | GCC 9+ |

---

## Quick Start

### Build

**Windows:**
```powershell
cmake -S . -B build
cmake --build build --config Debug
# Output: build\bin\Debug\orchestrator.exe
```

**Linux:**
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Output: build/bin/orchestrator
```

### Test

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The API smoke tests target an already running instance and require `API_KEY`:

```powershell
$env:API_KEY = "<key>"
.\tests\test_api.ps1
```

```bash
API_KEY="<key>" ./tests/test_api.sh
```

### Package

```powershell
cmake --build build --config Release --target package
```

CPack produces self-contained ZIP/TGZ archives with the binaries, `config/`, and `web/` assets.

### Run

```powershell
.\build\bin\Debug\orchestrator.exe                          # default config
.\build\bin\Debug\orchestrator.exe --conf path\to\conf      # custom config
```

The secure default is `command_mode = app_only`: clients submit a registered `app_id` and typed `parameters`; resource requirements come from the server-side app definition. Enable `free` mode only in a trusted environment.

### Generate an API Key

```powershell
.\build\bin\Debug\orchestrator.exe keygen --label "my-app"
```

---

## Documentation

| Document | Description |
|---|---|
| [Configuration](doc/configuration.md) | Full config reference, orchestrator host setup, job host setup, SSH, directory layout |
| [API Reference](doc/api-reference.md) | All REST API endpoints — auth, jobs, resources, stats, provisioning, apps, users, keys, quotas |
| [Machine Provisioning](doc/provisioning.md) | Static pool definition and live provisioning |
| [Web UI](doc/web-ui.md) | Built-in browser interface — tabs, job submission, auto-refresh |
| [PowerShell Examples](doc/examples.md) | Copy-paste examples for every API endpoint |
| [Project Structure](doc/architecture.md) | Source tree and module overview |
| [V1 Scope and Decisions](doc/v1-scope.md) | Supported architecture, deferred features, migration rationale, and release gates |
| [Presimulation Design & Tools](doc/presim.md) | Presim contract, solver integration, OpenFOAM wrapper, calibration harness, and tooling (`tools/presim_calibrate.py`, `tools/openfoam_presim_wrapper.sh`) |

Other useful scripts:
- `scripts/anonymize_repo.py` — sanitize repository artifacts before sharing (dry-run by default; `--apply` to modify files, creates `.bak` backups).
- `scripts/verify_syntax.py` — quick syntax checker for Python/JSON/SH/C files used during repository maintenance.

---

## License

See [LICENSE](LICENSE) and [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md).
