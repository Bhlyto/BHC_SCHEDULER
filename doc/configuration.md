# Configuration

`config/orchestrator.conf` is copied next to the executable on every build.
All keys are optional — built-in defaults are used for any missing key.
Lines starting with `#` are comments.
Paths can be **absolute** or **relative to the executable directory** (resolved automatically on Windows).

## Full configuration reference

```ini
# ── HTTP server ───────────────────────────────────
listen_port           = 8080

# ── Web UI / Bastion mode ─────────────────────────
# Set to 0 to disable the web UI entirely (API-only / bastion mode).
# When disabled, only the REST API is accessible — static assets return 403.
web_ui_enabled        = 1

# Bind address. Use "127.0.0.1" to restrict to localhost only,
# "0.0.0.0" (default) for all interfaces.
listen_address        = 0.0.0.0

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
# Maximum seconds a job can be RUNNING before the scheduler
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
#                    ORCH_WORKER_ID
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

# ── Machine Availability Probe ───────────────────
# How the scheduler checks whether static machines are reachable.
# method: "tcp" (default), "ping", or "ssh"
probe_method          = tcp

# Port to probe for TCP/SSH methods (default: 22)
probe_port            = 22

# Connection timeout in milliseconds (default: 3000)
probe_timeout_ms      = 3000

# Number of retries before marking a machine offline (default: 2)
probe_retries         = 2

# Interval between probe sweeps in milliseconds (default: 60000)
probe_interval_ms     = 60000

# ── Cloud Provisioning ───────────────────────────
# Path to a cloud credentials file (provider-specific).
# Leave empty to rely on the default CLI credentials (AWS CLI, gcloud, az).
cloud_credentials_file =

# ── Cloud Auto-Scaling ───────────────────────────
# Automatically provision a cloud VM when no machine can satisfy a job.
cloud_auto_provision          = 0
# Automatically terminate idle cloud VMs after job completion.
cloud_auto_deprovision        = 0

# Default template for auto-provisioned instances.
cloud_default_provider        = aws
cloud_default_instance_type   = t3.medium
cloud_default_region          = us-east-1
cloud_default_image_id        = ami-0abcdef1234567890
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
| `ORCH_WORKER_ID` | ID of the worker allocated to this job |
| `ORCH_MACHINE_IDS` | Compatibility alias for `ORCH_WORKER_ID` |
| `ORCH_MACHINE_COUNT` | Compatibility value, always `1` in v1 |

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

## Bastion Mode

When `web_ui_enabled = 0`, the orchestrator runs in **bastion mode**: the REST API remains fully functional, but all web UI routes (`/`, `/web/*`) return `403 Forbidden`. This is useful when the orchestrator is exposed on a network where only programmatic access should be allowed.

```ini
web_ui_enabled = 0
```

You can also restrict the bind address to limit which network interfaces accept connections:

```ini
listen_address = 127.0.0.1   # localhost only
```

---

## Machine Availability Probe

The orchestrator runs a background thread that periodically checks whether static machines are reachable. Machines that fail the probe are marked **offline** and will not receive new job dispatches until they become reachable again.

### Probe methods

| Method | Config value | Behaviour |
|--------|-------------|-----------|
| **TCP** | `tcp` | Non-blocking TCP connect to `probe_port` (default 22) |
| **Ping** | `ping` | System `ping` command (ICMP echo) |
| **SSH** | `ssh` | TCP connect to port 22 |

### Configuration

```ini
probe_method      = tcp       # "tcp", "ping", or "ssh"
probe_port        = 22        # port for TCP/SSH probes
probe_timeout_ms  = 3000      # connection timeout (ms)
probe_retries     = 2         # attempts before marking offline
probe_interval_ms = 60000     # time between sweeps (ms)
```

The probe only applies to **static** machines (type `"static"`). Cloud-provisioned machines are managed separately.

---

## Cloud Provisioning

The orchestrator can provision and deprovision virtual machines on **AWS**, **GCP**, and **Azure** by delegating to their respective CLI tools. No cloud SDK is linked — the orchestrator calls the CLI as a subprocess.

### Prerequisites

Install and authenticate the CLI for each provider you plan to use:

| Provider | CLI tool | Auth setup |
|----------|----------|-----------|
| **AWS** | `aws` (AWS CLI v2) | `aws configure` or environment variables |
| **GCP** | `gcloud` (Google Cloud SDK) | `gcloud auth login` or service account key |
| **Azure** | `az` (Azure CLI) | `az login` or service principal |

The CLI must be in `PATH` on the orchestrator host.

### Configuration

```ini
# Optional — leave empty to rely on default CLI credentials
cloud_credentials_file =
```

### How it works

1. An admin sends a `POST /admin/cloud/provision` request with a `CloudMachineSpec` (provider, instance type, region, image, resources).
2. The orchestrator calls the provider CLI (e.g. `aws ec2 run-instances`) with the specified parameters.
3. On success, the new instance is automatically **registered in the machine pool** as a cloud machine (`type: "cloud"`) and becomes available for job scheduling.
4. To tear down, send `POST /admin/cloud/deprovision` with the provider and instance ID. The instance is terminated and removed from the pool.

See [API Reference — Cloud Provisioning](api-reference.md#cloud-provisioning) for endpoint details.

### Auto-scaling (automatic provision & deprovision)

When `cloud_auto_provision` is enabled, the scheduler will **automatically spin up a cloud VM** any time a job cannot be placed on an existing machine. The VM is sized to match the job’s resource requirements, using the default template from the config.

When `cloud_auto_deprovision` is enabled, cloud machines that become **completely idle** (zero reservations) after a job finishes or times out are **automatically terminated** and removed from the pool.

```ini
cloud_auto_provision         = 1
cloud_auto_deprovision       = 1

# Default template for auto-provisioned instances
cloud_default_provider       = aws
cloud_default_instance_type  = t3.medium
cloud_default_region         = us-east-1
cloud_default_image_id       = ami-0abcdef1234567890
```

| Setting | Default | Description |
|---|---|---|
| `cloud_auto_provision` | `0` | `1` to auto-provision when no machine fits a job |
| `cloud_auto_deprovision` | `0` | `1` to terminate idle cloud machines after job completion |
| `cloud_default_provider` | (empty) | Provider: `aws`, `gcp`, or `azure` |
| `cloud_default_instance_type` | (empty) | Instance size (e.g. `t3.medium`, `e2-standard-4`) |
| `cloud_default_region` | (empty) | Region/zone (e.g. `us-east-1`, `us-central1-a`) |
| `cloud_default_image_id` | (empty) | AMI / image family / image URN |

> **Note:** `cloud_auto_provision` requires `cloud_default_provider` to be set. The provisioned VM’s CPU/RAM/disk are set to at least the job’s requirements. Each job triggers at most one provision attempt to avoid spinning up duplicates.

---

## Wake-on-LAN

For on-premise static machines that support Wake-on-LAN, the orchestrator can send a WoL magic packet to power them on remotely.

### Requirements

- The target machine's **MAC address** must be defined in `provisioning.json`.
- The orchestrator must be on the same broadcast domain (or you must specify the correct broadcast IP).
- WoL must be enabled in the target machine's BIOS/UEFI and network adapter settings.

### Usage

Send a `POST /admin/wol` request with the `machine_id`. See [API Reference — Wake-on-LAN](api-reference.md#wake-on-lan).

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

## Presimulation tuning

Presimulation parameters are optionally stored in `config/presim.conf`. When present, the orchestrator loads `presim.conf` after `orchestrator.conf` and applies presim tuning without modifying core runtime settings.

Common keys in `presim.conf`:
- `threshold_max`: float — maximum acceptable per-zone error for presim selection.
- `refine_mult`: float — multiplier controlling refinement aggressiveness.
- `high_mult`: float — multiplier applied to high-fidelity runtime estimates.
- `uncertainty_weight`: float — weight applied to zone uncertainty when ranking zones for refinement.

Use `tools/presim_calibrate.py` to search for good values; the script can write the best-found parameters into `config/presim.conf`.
