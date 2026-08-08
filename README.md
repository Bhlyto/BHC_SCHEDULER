# BHC Scheduler — Job Orchestrator

A lightweight daemon written in C that exposes a REST API for submitting, tracking, and cancelling jobs across a pool of machines. Built on [Mongoose](https://mongoose.ws/) (HTTP/SSE), SQLite (persistence), and cJSON. Includes a built-in **web UI** for managing jobs, users, quotas, and applications from the browser.

---

## Features

- **Job queue** — submit commands with resource requirements; the scheduler dispatches them as machines become available
- **Transactional batches** — submit up to 1000 independent jobs atomically and query aggregate progress
- **Priority scheduling** — lower priority value = higher priority (default `50`)
- **Resource-aware allocation** — jobs declare `req_cores`, `req_gpu`, `req_ram_mb`, `req_disk_mb`; the allocator matches them against available capacity
- **Multi-machine jobs** — when no single machine can satisfy a core request, the allocator can spread the job across multiple machines automatically
- **Machine pools** — define hundreds of machines compactly in `provisioning.json` using prefix/format ranges; add or remove machines at runtime without restart
- **Application definitions** — define apps as JSON files with pre-set resource requirements and dynamic form fields; users pick an app when submitting a job
- **Artifact metadata** — persist URIs and sizes for stdout, stderr, and recursively collected output files without turning the scheduler into a storage system
- **HELD jobs** — submit a job with expected input files; it stays in `HELD` state until all files are uploaded, then auto-releases to the queue
- **Job logs** — stdout and stderr of each job are captured and retrievable via API
- **Pre-job scripts** — run a setup script before each job; receives job context via environment variables
- **Real-time events** — subscribe to a Server-Sent Events (SSE) stream for live job state updates and an initial snapshot
- **Event persistence & reporting** — all system events (dispatches, cloud operations, status changes) are logged to the database with full reporting queries (per-user, per-app, per-machine, jobs over time)
- **Stats endpoint** — aggregated view of job counts by state and cluster resource utilisation
- **User management** — create, update, and delete users with password authentication via admin API
- **Password authentication** — users log in with user ID and password; receive a temporary API key
- **API key management** — create, list, and revoke API keys; bind keys to users with role-based access (admin/user)
- **Quotas** — enforce per-user and per-app limits on job count, RAM, cores, and concurrent jobs
- **Web UI** — built-in single-page application with dark theme; dashboard, job management, reporting charts, machine status grid, and admin panels
- **Bastion mode** — disable the web UI entirely (`web_ui_enabled = 0`) while keeping the REST API accessible; useful for headless or API-only deployments
- **Machine availability probing** — background thread checks whether static machines are reachable via TCP, ping, or SSH; offline machines are automatically excluded from scheduling
- **Cloud provisioning** — provision and deprovision VMs on AWS, GCP, and Azure on-demand via their CLI tools; instances are auto-registered in the machine pool
- **Cloud auto-scaling** — automatically spin up a cloud VM when no machine can satisfy a job’s requirements, and tear it down when the job finishes
- **Wake-on-LAN** — send WoL magic packets to power on on-premise machines remotely; triggered via API or the web UI
- **Auto-cleanup** — work directories are deleted automatically after a configurable TTL
- **Bulk purge** — single call to delete all finished/failed/cancelled jobs and their work directories
- **Cross-platform** — runs as a Windows Service or Linux daemon
 - **Presimulation (presim) workflow** — pluggable presim solvers (Python/OpenFOAM), a decision-core abstraction for refinement decisions, and a calibration harness to tune presim parameters for speed vs accuracy tradeoffs

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

### Run

```powershell
.\build\bin\Debug\orchestrator.exe                          # default config
.\build\bin\Debug\orchestrator.exe --conf path\to\conf      # custom config
```

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
| [Presimulation Design & Tools](doc/presim.md) | Presim contract, solver integration, OpenFOAM wrapper, calibration harness, and tooling (`tools/presim_calibrate.py`, `tools/openfoam_presim_wrapper.sh`) |

Other useful scripts:
- `scripts/anonymize_repo.py` — sanitize repository artifacts before sharing (dry-run by default; `--apply` to modify files, creates `.bak` backups).
- `scripts/verify_syntax.py` — quick syntax checker for Python/JSON/SH/C files used during repository maintenance.

---

## License

See [LICENSE](LICENSE) and [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md).
