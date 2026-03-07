# Configuration

`config/orchestrator.conf` is copied next to the executable on every build.
All keys are optional — built-in defaults are used for any missing key.
Lines starting with `#` are comments.
Paths can be **absolute** or **relative to the executable directory** (resolved automatically on Windows).

## Full configuration reference

```ini
# ── HTTP server ───────────────────────────────────
listen_port           = 8080

# ── Paths ─────────────────────────────────────────
# All paths can be absolute or relative to the executable directory.
work_dir              = jobs                      # local job work directory (input/ + output/ per job)
db_path               = orchestrator.db           # SQLite database
provisioning_json     = config/provisioning.json  # initial machine pool definition
apps_dir              = config/apps               # directory containing app definition JSON files

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

## Orchestrator host setup

The orchestrator is the **Windows (or Linux) machine** that runs the `orchestrator.exe` daemon. It serves the REST API, stores the SQLite database, manages the job queue, and orchestrates SSH connections to job hosts.

### Directory layout (relative to the executable)

```
orchestrator.exe
config/
├── orchestrator.conf         # main configuration
├── provisioning.json         # machine pool definition
├── pre_job_script_win.bat    # pre-job script (Windows)
└── apps/                     # app definitions
    ├── app1.json
    └── app2.json
web/                          # web UI assets (served at /web/*)
├── index.html
├── style.css
├── app.js
├── dashboard.js
├── jobs.js
└── admin.js
jobs/                         # work_dir — created automatically
├── <job-id>/
│   ├── input/                # uploaded input files
│   ├── output/               # output files retrieved from remote
│   ├── stdout.log
│   └── stderr.log
orchestrator.db               # SQLite database — created automatically
```

### Steps

1. **Build** the project (see [Build](../README.md#build)).
2. **Edit** `config/orchestrator.conf`:
   - Set `listen_port` to the port the API should listen on.
   - Set `work_dir`, `db_path`, `provisioning_json` to desired locations (relative paths are resolved against the exe directory on Windows).
   - Set `ssh_user` to the login used on remote job hosts.
   - Set `ssh_key` to the path to an SSH private key (no passphrase) that is authorised on every job host.
   - Set `ssh_remote_work_dir` to a writable directory on the remote hosts (e.g. `/tmp/orch`).
   - Optionally set `temp_dir` for temporary files (per-job SSH known_hosts). Defaults to the system temp directory.
3. **Define the machine pool** in `config/provisioning.json` (see [Machine Provisioning](provisioning.md)).
4. **Generate an API key** (see [Generating an API Key](#generating-an-api-key)).
5. **Start** the daemon:
   ```powershell
   .\orchestrator.exe                                   # default config
   .\orchestrator.exe --conf C:\path\to\orchestrator.conf  # custom config
   ```

> **Note:** The orchestrator creates the `work_dir` and `orchestrator.db` automatically on first run. No manual directory creation is required.

---

## Job hosts setup

Job hosts are the **remote Linux machines** where the actual job commands are executed via SSH. The orchestrator connects to them over SSH using the configured `ssh_user` and `ssh_key`.

### Requirements on each job host

| Requirement | Details |
|---|---|
| **SSH server** | OpenSSH `sshd` running and reachable from the orchestrator |
| **User account** | The `ssh_user` (e.g. `deploy`) must exist and be accessible via the configured `ssh_key` |
| **Writable work directory** | `ssh_remote_work_dir` (e.g. `/tmp/orch`) must be writable by `ssh_user` |
| **Tools** | `sh`, `mkdir`, `scp` must be available in `PATH` |

### How the orchestrator uses a job host

For each job dispatched to a remote host, the orchestrator performs these steps over SSH/SCP:

1. **Create directories** — `ssh <host> "mkdir -p '<work_dir>/<job_id>/input' '<work_dir>/<job_id>/output'"`
2. **Upload input files** — `scp -r <local_input>/. <host>:<remote_input>`
3. **Upload run script** — a generated `.run.sh` that exports `ORCH_*` environment variables and runs the job command
4. **Execute** — `ssh <host> "sh '<work_dir>/<job_id>/.run.sh'"`
5. **Retrieve output** — after the job finishes, `scp -r <host>:<remote_output>/. <local_output>`

### Environment variables available inside the job

| Variable | Description |
|---|---|
| `ORCH_JOB_ID` | UUID of the job |
| `ORCH_INPUT_DIR` | Absolute path to the input directory on the remote host |
| `ORCH_OUTPUT_DIR` | Absolute path to the output directory on the remote host |
| `ORCH_MACHINE_IDS` | Comma-separated list of allocated machine IDs |
| `ORCH_MACHINE_COUNT` | Number of allocated machines |

### SSH key setup

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

### Directory layout on a job host

```
/tmp/orch/                        # ssh_remote_work_dir
├── <job-id>/
│   ├── .run.sh                   # generated run script (auto-uploaded)
│   ├── input/                    # uploaded input files
│   └── output/                   # job writes results here
```

> **Tip:** The remote work directory is not cleaned up automatically by the orchestrator. Set up a cron job or systemd timer on job hosts to periodically purge old job directories under `ssh_remote_work_dir`.

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
