# BHC Scheduler — Job Orchestrator

A lightweight daemon written in C that exposes a REST API for submitting, tracking, and cancelling jobs across a pool of machines. Built on [Mongoose](https://mongoose.ws/) (HTTP/SSE), SQLite (persistence), and cJSON.

---

## Features

- **Job queue** — submit commands with resource requirements; the scheduler dispatches them as machines become available
- **Priority scheduling** — lower priority value = higher priority (default `50`)
- **Resource-aware allocation** — jobs declare `req_cores`, `req_gpu`, `req_ram_mb`, `req_disk_mb`; the allocator matches them against available capacity
- **Multi-machine jobs** — when no single machine can satisfy a core request, the allocator can spread the job across multiple machines automatically
- **Machine pools** — define hundreds of machines compactly in `provisioning.json` using prefix/format ranges; add or remove machines at runtime without restart
- **File I/O** — upload input files before a job runs; download output files after it finishes
- **Job logs** — stdout and stderr of each job are captured and retrievable via API
- **Pre-job scripts** — run a setup script before each job; receives job context via environment variables
- **Real-time events** — subscribe to a Server-Sent Events (SSE) stream for live job state updates and an initial snapshot
- **Stats endpoint** — aggregated view of job counts by state and cluster resource utilisation
- **API key authentication** — SHA-256 hashed keys stored in SQLite; generated via CLI
- **Auto-cleanup** — work directories are deleted automatically after a configurable TTL
- **Bulk purge** — single call to delete all finished/failed/cancelled jobs and their work directories
- **Cross-platform** — runs as a Windows Service or Linux daemon

---

## Requirements

| Tool | Minimum version |
|---|---|
| CMake | 3.16 |
| Visual Studio / MSVC (Windows) | VS 2019+ (toolset v142+) |
| GCC + Make (Linux) | GCC 9+ |

---

## Build

### Windows

```powershell
git clone https://github.com/<your-org>/BHC_SCHEDULER.git
cd BHC_SCHEDULER

cmake -S . -B build
cmake --build build --config Debug

# Output: build\bin\Debug\orchestrator.exe
# config\ is copied automatically next to the executable.
```

### Linux

```bash
git clone https://github.com/<your-org>/BHC_SCHEDULER.git
cd BHC_SCHEDULER

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Output: build/bin/orchestrator
```

---

## Configuration

`config/orchestrator.conf` is copied next to the executable on every build.
All keys are optional — built-in defaults are used for any missing key.
Lines starting with `#` are comments.
Paths can be **absolute** or **relative to the executable directory** (resolved automatically on Windows).

### Full configuration reference

```ini
# ── HTTP server ───────────────────────────────────
listen_port           = 8080

# ── Paths ─────────────────────────────────────────
# All paths can be absolute or relative to the executable directory.
work_dir              = jobs                      # local job work directory (input/ + output/ per job)
db_path               = orchestrator.db           # SQLite database
provisioning_json     = config/provisioning.json  # initial machine pool definition

# Temporary directory for per-job known_hosts files and other temp data.
# Defaults to system temp directory (TEMP on Windows, /tmp on Linux).
temp_dir              =

# PID file path (Linux daemon mode only).
pid_file              = /var/run/orchestrator.pid

# ── Logging ───────────────────────────────────────
# Levels: debug | info | warn | error
log_level             = info

# ── Scheduler ─────────────────────────────────────
# How often the scheduler checks for runnable jobs (milliseconds)
scheduler_poll_ms     = 500

# ── Job timeout ───────────────────────────────────
# Maximum seconds a job can be in STARTING or RUNNING before the scheduler
# marks it FAILED and releases its resources. 0 = no global timeout.
# Can also be overridden per-job with the "timeout_seconds" field at submit.
job_timeout_seconds   = 3600

# ── Auto-cleanup ──────────────────────────────────
# Seconds after a job reaches a terminal state before its work directory
# (input/ + output/) is deleted. 0 = disabled.
cleanup_ttl_seconds   = 3600

# ── Pre-job scripts ──────────────────────────────
# Executed at the start of each job. Leave empty to disable.
# Receives env vars: ORCH_JOB_ID, ORCH_INPUT_DIR, ORCH_OUTPUT_DIR,
#                    ORCH_MACHINE_IDS, ORCH_MACHINE_COUNT
pre_job_script_win    = config\pre_job_script_win.bat
pre_job_script_linux  =

# ── Remote SSH execution ─────────────────────────
# SSH user on remote machines. When set, jobs are executed on the
# allocated machine via SSH. Leave blank for local-only mode.
ssh_user              = deploy

# Path to the SSH private key (no passphrase). Absolute or relative to exe.
ssh_key               = .ssh/orch_key

# Base working directory on remote machines (must be writable by ssh_user)
ssh_remote_work_dir   = /tmp/orch
```

---

### Orchestrator host setup

The orchestrator is the **Windows (or Linux) machine** that runs the `orchestrator.exe` daemon. It serves the REST API, stores the SQLite database, manages the job queue, and orchestrates SSH connections to job hosts.

#### Directory layout (relative to the executable)

```
orchestrator.exe
config/
├── orchestrator.conf         # main configuration
├── provisioning.json         # machine pool definition
└── pre_job_script_win.bat    # pre-job script (Windows)
jobs/                         # work_dir — created automatically
├── <job-id>/
│   ├── input/                # uploaded input files
│   ├── output/               # output files retrieved from remote
│   ├── stdout.log
│   └── stderr.log
orchestrator.db               # SQLite database — created automatically
```

#### Steps

1. **Build** the project (see [Build](#build)).
2. **Edit** `config/orchestrator.conf`:
   - Set `listen_port` to the port the API should listen on.
   - Set `work_dir`, `db_path`, `provisioning_json` to desired locations (relative paths are resolved against the exe directory on Windows).
   - Set `ssh_user` to the login used on remote job hosts.
   - Set `ssh_key` to the path to an SSH private key (no passphrase) that is authorised on every job host.
   - Set `ssh_remote_work_dir` to a writable directory on the remote hosts (e.g. `/tmp/orch`).
   - Optionally set `temp_dir` for temporary files (per-job SSH known_hosts). Defaults to the system temp directory.
3. **Define the machine pool** in `config/provisioning.json` (see [Machine Provisioning](#machine-provisioning)).
4. **Generate an API key** (see [Generating an API Key](#generating-an-api-key)).
5. **Start** the daemon:
   ```powershell
   .\orchestrator.exe                                   # default config
   .\orchestrator.exe --conf C:\path\to\orchestrator.conf  # custom config
   ```

> **Note:** The orchestrator creates the `work_dir` and `orchestrator.db` automatically on first run. No manual directory creation is required.

---

### Job hosts setup

Job hosts are the **remote Linux machines** where the actual job commands are executed via SSH. The orchestrator connects to them over SSH using the configured `ssh_user` and `ssh_key`.

#### Requirements on each job host

| Requirement | Details |
|---|---|
| **SSH server** | OpenSSH `sshd` running and reachable from the orchestrator |
| **User account** | The `ssh_user` (e.g. `deploy`) must exist and be accessible via the configured `ssh_key` |
| **Writable work directory** | `ssh_remote_work_dir` (e.g. `/tmp/orch`) must be writable by `ssh_user` |
| **Tools** | `sh`, `mkdir`, `scp` must be available in `PATH` |

#### How the orchestrator uses a job host

For each job dispatched to a remote host, the orchestrator performs these steps over SSH/SCP:

1. **Create directories** — `ssh <host> "mkdir -p '<work_dir>/<job_id>/input' '<work_dir>/<job_id>/output'"`
2. **Upload input files** — `scp -r <local_input>/. <host>:<remote_input>`
3. **Upload run script** — a generated `.run.sh` that exports `ORCH_*` environment variables and runs the job command
4. **Execute** — `ssh <host> "sh '<work_dir>/<job_id>/.run.sh'"`
5. **Retrieve output** — after the job finishes, `scp -r <host>:<remote_output>/. <local_output>`

#### Environment variables available inside the job

| Variable | Description |
|---|---|
| `ORCH_JOB_ID` | UUID of the job |
| `ORCH_INPUT_DIR` | Absolute path to the input directory on the remote host |
| `ORCH_OUTPUT_DIR` | Absolute path to the output directory on the remote host |
| `ORCH_MACHINE_IDS` | Comma-separated list of allocated machine IDs |
| `ORCH_MACHINE_COUNT` | Number of allocated machines |

#### SSH key setup

```bash
# On the orchestrator host — generate a key pair (if not already done)
ssh-keygen -t ed25519 -f ~/.ssh/orch_key -N ""

# On each job host — authorize the public key
cat orch_key.pub >> ~deploy/.ssh/authorized_keys
chmod 600 ~deploy/.ssh/authorized_keys
```

Then set the path in `orchestrator.conf`:
```ini
ssh_key = .ssh/orch_key
```

#### Directory layout on a job host

```
/tmp/orch/                        # ssh_remote_work_dir
├── <job-id>/
│   ├── .run.sh                   # generated run script (auto-uploaded)
│   ├── input/                    # uploaded input files
│   └── output/                   # job writes results here
```

> **Tip:** The remote work directory is not cleaned up automatically by the orchestrator. Set up a cron job or systemd timer on job hosts to periodically purge old job directories under `ssh_remote_work_dir`.

---

## Machine Provisioning

### Static file

Define the machine pool in `provisioning.json` next to the executable. Supports individual entries and **range-based pools**:

```json
{
  "machines": [
    {
      "id":        "local",
      "hostname":  "localhost",
      "ip":        "127.0.0.1",
      "enabled":   true,
      "cores":     4,
      "gpu_count": 0,
      "ram_mb":    8192,
      "disk_mb":   102400
    }
  ],
  "pools": [
    {
      "id_prefix":       "server-",
      "hostname_format": "server-%03d.example.com",
      "ip_format":       "10.0.1.%d",
      "range_start":     1,
      "range_end":       128,
      "enabled":         false,
      "cores":           16,
      "gpu_count":       0,
      "ram_mb":          32768,
      "disk_mb":         512000
    }
  ]
}
```

### Live provisioning

Machines can be added, updated, or removed at runtime via the API without restarting the daemon — see `POST /provision` and `DELETE /provision/:id`.

---

## Starting the Daemon

```powershell
# Windows — console mode
.\build\bin\Debug\orchestrator.exe

# Custom config
.\build\bin\Debug\orchestrator.exe --conf C:\path\to\orchestrator.conf
```

```bash
# Linux
./build/bin/orchestrator
./build/bin/orchestrator --conf /etc/orch/orchestrator.conf
```

---

## Generating an API Key

The raw key is shown **only once** — only its SHA-256 hash is stored in the database.

```powershell
.\build\bin\Debug\orchestrator.exe keygen --label "my-application"
```

```
API Key generated for label 'my-application':
a3f8c2e1d4b79f0e...  (64 hex characters)
Store this key — it will not be shown again.
```

| Parameter | Description | Default |
|---|---|---|
| `--label "name"` | Human-readable label | `default` |
| `--conf "path"` | Alternative config file | auto-detected |

---

## REST API

All routes require:
```
X-API-Key: <your-key>
```

### Jobs

#### Submit a job
```http
POST /jobs
Content-Type: application/json
```

| Field | Type | Required | Description |
|---|---|---|---|
| `command` | string | ✅ | Command to execute |
| `priority` | int | — | Priority (lower = higher priority). Default: `50` |
| `req_cores` | int | — | CPU cores required. Default: `1` |
| `req_gpu` | int | — | GPUs required. Default: `0` |
| `req_ram_mb` | int | — | RAM in MB. Default: `0` |
| `req_disk_mb` | int | — | Disk in MB. Default: `0` |

Response `201`:
```json
{
  "id":           "550e8400-e29b-41d4-a716-446655440000",
  "command":      "python3 train.py",
  "status":       "IN_QUEUE",
  "priority":     50,
  "req_cores":    2,
  "req_gpu":      0,
  "req_ram_mb":   2048,
  "req_disk_mb":  10240,
  "machine_id":   "",
  "submitted_at": 1740744229,
  "started_at":   0,
  "ended_at":     0,
  "exit_code":    0
}
```

Job states: `IN_QUEUE` → `STARTING` → `RUNNING` → `FINISHED` / `FAILED` / `CANCELLED`

---

#### List all jobs
```http
GET /jobs
```
Returns a JSON array of all job objects.

---

#### Get a job
```http
GET /jobs/:id
```

---

#### Cancel a job
```http
DELETE /jobs/:id
```
Returns the updated job object. Returns `409` if the job is already in a terminal state.

---

#### Purge terminal jobs
```http
DELETE /jobs
```
Deletes all `FINISHED`, `FAILED`, and `CANCELLED` jobs from the database and removes their work directories.

Response:
```json
{ "deleted": 12, "cleaned": 10 }
```

---

#### Upload an input file
```http
POST /jobs/:id/input/:filename
Content-Type: application/octet-stream

<binary content>
```
Files are stored in the job's input directory and injected as `ORCH_INPUT_DIR` into the pre-job script and job command.

---

#### Download an output file
```http
GET /jobs/:id/output/:filename
```

---

#### Get stdout log
```http
GET /jobs/:id/log
```

#### Get stderr log
```http
GET /jobs/:id/log/stderr
```

Both return `text/plain`. Returns `404` if the log file does not exist yet.

---

#### Subscribe to real-time events (SSE)
```http
GET /jobs/events
```
Opens a persistent `text/event-stream` connection.

- **`snapshot`** event — sent immediately on connect; contains the current state of all jobs as a JSON array.
- **`job_status`** event — sent every time a job changes state:
  ```
  event: job_status
  data: {"event":"job_status","id":"<id>","status":"RUNNING","machine_id":"srv-01"}
  ```

---

### Resources

#### List machines
```http
GET /resources
```

Response — array of machine objects:
```json
[
  {
    "id":               "srv-01",
    "hostname":         "server01.local",
    "ip":               "192.168.1.10",
    "enabled":          true,
    "cores_total":      16,
    "cores_reserved":   4,
    "gpu_total":        2,
    "gpu_reserved":     0,
    "ram_mb_total":     32768,
    "ram_mb_reserved":  4096,
    "disk_mb_total":    512000,
    "disk_mb_reserved": 10240
  }
]
```

---

### Stats

#### Cluster statistics
```http
GET /stats
```

```json
{
  "jobs": {
    "total": 42, "in_queue": 3, "starting": 1,
    "running": 5, "finished": 30, "failed": 2, "cancelled": 1
  },
  "machines": { "total": 8, "enabled": 8 },
  "resources": {
    "cores_total": 128, "cores_used": 24,
    "ram_mb_total": 262144, "ram_mb_used": 20480
  }
}
```

---

### Live Provisioning

#### Add or update a machine
```http
POST /provision
Content-Type: application/json

{
  "id":        "srv-02",
  "hostname":  "server02.local",
  "ip":        "192.168.1.11",
  "enabled":   true,
  "cores":     16,
  "gpu_count": 2,
  "ram_mb":    32768,
  "disk_mb":   512000
}
```

#### Remove a machine
```http
DELETE /provision/:id
```

---

## PowerShell Examples

```powershell
$key     = "your-api-key-here"
$base    = "http://localhost:8080"
$headers = @{ "X-API-Key" = $key; "Content-Type" = "application/json" }

# Submit a job
$body = '{"command":"echo hello","req_cores":1,"req_ram_mb":512}'
Invoke-RestMethod -Uri "$base/jobs" -Method POST -Headers $headers -Body $body

# List all jobs
Invoke-RestMethod -Uri "$base/jobs" -Method GET -Headers $headers

# Get a specific job
Invoke-RestMethod -Uri "$base/jobs/<id>" -Method GET -Headers $headers

# Cancel a job
Invoke-RestMethod -Uri "$base/jobs/<id>" -Method DELETE -Headers $headers

# Purge all terminal jobs
Invoke-RestMethod -Uri "$base/jobs" -Method DELETE -Headers $headers

# Upload an input file
Invoke-RestMethod -Uri "$base/jobs/<id>/input/data.csv" -Method POST `
    -Headers @{ "X-API-Key" = $key } -ContentType "application/octet-stream" `
    -InFile "C:\data\data.csv"

# Download an output file
Invoke-RestMethod -Uri "$base/jobs/<id>/output/result.json" -Method GET `
    -Headers @{ "X-API-Key" = $key } -OutFile "C:\data\result.json"

# Get stdout log
Invoke-RestMethod -Uri "$base/jobs/<id>/log" -Method GET -Headers $headers

# Get stderr log
Invoke-RestMethod -Uri "$base/jobs/<id>/log/stderr" -Method GET -Headers $headers

# Cluster stats
Invoke-RestMethod -Uri "$base/stats" -Method GET -Headers $headers

# List machines with utilisation
Invoke-RestMethod -Uri "$base/resources" -Method GET -Headers $headers

# Add a machine at runtime
$m = '{"id":"srv-03","hostname":"srv03.local","ip":"10.0.0.3","cores":32,"ram_mb":65536,"disk_mb":1048576}'
Invoke-RestMethod -Uri "$base/provision" -Method POST -Headers $headers -Body $m

# Remove a machine
Invoke-RestMethod -Uri "$base/provision/srv-03" -Method DELETE -Headers $headers
```

---

## Project Structure

```
BHC_SCHEDULER/
├── src/
│   ├── main.c                  # Entry point, startup, shutdown
│   ├── core/
│   │   ├── scheduler.c         # Dispatch loop, poll, TTL cleanup
│   │   ├── job.c               # Job struct, state machine, SSE events
│   │   ├── queue.c             # Priority queue
│   │   └── executor.c          # Process launch, pre-job script, stdout/stderr capture
│   ├── http/
│   │   ├── httpd.c             # Mongoose event loop, SSE subscriber list
│   │   ├── routes.c            # Route dispatcher and all handlers
│   │   ├── auth.c              # SHA-256 API key check
│   │   └── response.c          # JSON / error helpers
│   ├── persistence/
│   │   ├── db.c                # SQLite — jobs, keys, stats, purge
│   │   ├── config.c            # INI parser
│   │   └── log.c               # Leveled logger
│   ├── resources/
│   │   ├── registry.c          # Machine registry, pool expansion
│   │   ├── allocator.c         # Single-machine and multi-machine allocation
│   │   └── probe.c             # Resource probing
│   ├── transfer/
│   │   ├── store.c             # Work directory management
│   │   ├── upload.c            # Input file write
│   │   └── download.c          # Output file serve
│   └── platform/
│       ├── service_win.c       # Windows Service integration
│       └── service_linux.c     # Linux daemon (double-fork)
├── include/                    # Public headers
├── vendor/                     # Mongoose, SQLite, cJSON (amalgamated)
├── config/
│   ├── orchestrator.conf       # Main configuration
│   └── provisioning.json       # Initial machine pool
└── CMakeLists.txt
```

---

## License

See [LICENSE](LICENSE) and [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md).
