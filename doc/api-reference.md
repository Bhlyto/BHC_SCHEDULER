# REST API Reference

All routes require an API key header unless noted otherwise:
```
X-API-Key: <your-key>
```

---

## Authentication

These routes are **public** — no API key required.

### Login with password
```http
POST /auth/login
Content-Type: application/json

{ "user_id": "alice", "password": "secret123" }
```

Response `200`:
```json
{ "user_id": "alice", "api_key": "<64-hex-chars>", "role": "user" }
```
The returned key expires in 24 hours. Any previous key for the user is revoked.

Returns `401` for invalid credentials, `403` if the account is disabled.

Passwords contain 12 to 128 characters and are stored with PBKDF2-HMAC-SHA256. Legacy hashes are upgraded after a successful login.

---

### Available auth methods
```http
GET /auth/methods
```

```json
{ "methods": ["password", "api_key"] }
```

---

### Current identity
```http
GET /auth/me
X-API-Key: <your-key>
```

```json
{ "user_id": "alice", "role": "user" }
```

---

### Change own password
```http
POST /auth/change-password
Content-Type: application/json
X-API-Key: <your-key>

{ "old_password": "old-secret", "new_password": "new-secret" }
```

Returns `{"ok": true}` or `401` if old password is wrong.

---

## Jobs

### Submit a job
```http
POST /jobs
Content-Type: application/json
Idempotency-Key: client-request-123
```

| Field | Type | Required | Description |
|---|---|---|---|
| `command` | string | Free mode only | Command to execute |
| `priority` | int | — | Priority (lower = higher priority). Default: `50` |
| `req_cores` | int | — | CPU cores required. Default: `1` |
| `req_gpu` | int | — | GPUs required. Default: `0` |
| `req_ram_mb` | int | — | RAM in MB. Default: `0` |
| `req_disk_mb` | int | — | Disk in MB. Default: `0` |
| `timeout_seconds` | int | — | Per-job timeout (0 = use global default) |
| `app_id` | string | — | Associate job with an application definition |
| `input_files` | string[] | — | Expected input filenames; job starts as `HELD` until all are uploaded |

In the default `app_only` mode, `app_id` and a typed `parameters` object replace `command`. Raw commands, undeclared parameters, and incorrect types are rejected. Resource fields are loaded from the server-side application definition, so client-provided `req_*` values cannot raise a job's allocation. The `command` field is available only when an administrator explicitly enables `command_mode = free`.

`Idempotency-Key` is optional, accepts 1–128 characters from `[A-Za-z0-9_.:-]`, and requires an API key bound to a user. Repeating a submission with the same user and key returns the original job with `200` instead of creating a duplicate.

Response `201`:
```json
{
  "id":              "JOB_ID",
  "command":         "python3 train.py",
  "status":          "QUEUED",
  "priority":        50,
  "req_cores":       2,
  "req_gpu":         0,
  "req_ram_mb":      2048,
  "req_disk_mb":     10240,
  "timeout_seconds": 3600,
  "user_id":         "alice",
  "app_id":          "app1",
  "machine_id":      "",
  "submitted_at":    1740744229,
  "started_at":      0,
  "ended_at":        0,
  "exit_code":       0
}
```

Job states: `QUEUED` → `STARTING` → `RUNNING` → `FINISHED` / `FAILED` / `CANCELLED`

When submitted with `input_files`: `HELD` → (files uploaded) → `QUEUED` → ...

---

### Release a held job
```http
POST /jobs/:id/release
```
Manually releases a `HELD` job to the queue, even if not all files have been uploaded.

---

### List all jobs
```http
GET /jobs?limit=100&offset=0&status=RUNNING&app_id=app1
```
Returns a JSON array of job objects. `limit` accepts `1–500`; `offset` must be non-negative. `status` accepts `QUEUED`/`IN_QUEUE`, `HELD`, `STARTING`, `RUNNING`, `FINISHED`, `FAILED`, or `CANCELLED`. Administrators may also filter with `user_id`; regular users always see only their own jobs.

---

### Get a job
```http
GET /jobs/:id
```

---

### Cancel a job
```http
DELETE /jobs/:id
```
Returns the updated job object. Returns `409` if the job is already in a terminal state.

---

### Purge terminal jobs
```http
DELETE /jobs
```
Deletes all `FINISHED`, `FAILED`, and `CANCELLED` jobs from the database and removes their work directories.

Response:
```json
{ "deleted": 12, "cleaned": 10 }
```

---

### Upload an input file
```http
POST /jobs/:id/input/:filename
Content-Type: application/octet-stream

<binary content>
```
Files are stored in the job's input directory and injected as `ORCH_INPUT_DIR` into the pre-job script and job command.

---

### Download an output file
```http
GET /jobs/:id/output/:filename
```

---

### Get stdout log
```http
GET /jobs/:id/log
```

### Get stderr log
```http
GET /jobs/:id/log/stderr
```

Both return `text/plain`. Returns `404` if the log file does not exist yet.

---

### List job files
```http
GET /jobs/:id/files
```

```json
{
  "input": [
    { "name": "data.csv", "size": 10485 }
  ],
  "output": [
    { "name": "results.json", "size": 2048 }
  ],
  "logs": {
    "has_stdout": true,
    "stdout_size": 4096,
    "has_stderr": true,
    "stderr_size": 128
  }
}
```

---

### Subscribe to real-time events (SSE)
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

## Resources

### List machines
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

## Stats

### Cluster statistics
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

### Prometheus metrics (admin)
```http
GET /metrics
```

Returns Prometheus text exposition for job states, machine states, drain mode, and total/reserved CPU and RAM.

### Scheduler maintenance / drain mode (admin)
```http
GET /admin/maintenance
```

```http
POST /admin/maintenance
Content-Type: application/json

{ "accepting_jobs": false }
```

Drain mode rejects new single-job and workflow submissions with `503` while queued and active jobs continue. Re-enable submissions by posting `true`.

---

## Live Provisioning

### Add or update a machine
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

### Remove a machine
```http
DELETE /provision/:id
```

Returns `409` while the machine still has reserved resources.

---

## Applications

App definitions are stored as JSON files in the `apps_dir` directory. Each app pre-defines resource requirements and custom form fields shown in the web UI.

---

## Events & Reporting

All reporting endpoints require **admin** role.

### List events
```http
GET /admin/events?category=job&from=1710000000&to=1720000000&limit=100
```

| Query param | Type | Default | Description |
|---|---|---|---|
| `category` | string | — | Filter by category (`job`, `cloud`, `machine`, `auth`) |
| `from` | int | — | Unix timestamp — events after this time |
| `to` | int | — | Unix timestamp — events before this time |
| `limit` | int | 200 | Max events to return (capped at 1000) |

Response `200`:
```json
[
  {
    "id": 1,
    "category": "job",
    "event_type": "dispatch",
    "detail": "Job abc123 dispatched to srv-01",
    "user_id": "alice",
    "job_id": "abc123",
    "machine_id": "srv-01",
    "created_at": 1710500000
  }
]
```

---

### Jobs over time
```http
GET /admin/reports/jobs?granularity=day&from=1710000000&to=1720000000
```

| Query param | Type | Default | Description |
|---|---|---|---|
| `granularity` | string | `day` | Time bucket: `hour`, `day`, or `month` |
| `from` | int | — | Start timestamp |
| `to` | int | — | End timestamp |

```json
[
  {
    "period": "2025-03-15",
    "total": 42,
    "finished": 35,
    "failed": 5,
    "avg_duration_s": 123.4
  }
]
```

---

### Per-user report
```http
GET /admin/reports/users?from=1710000000&to=1720000000
```

```json
[
  {
    "user_id": "alice",
    "total_jobs": 120,
    "finished": 110,
    "failed": 8,
    "avg_duration_s": 95.2,
    "total_cores_used": 240,
    "total_ram_mb_used": 491520
  }
]
```

---

### Per-application report
```http
GET /admin/reports/apps?from=1710000000&to=1720000000
```

```json
[
  {
    "app_id": "app1",
    "total_jobs": 80,
    "finished": 75,
    "failed": 3,
    "avg_duration_s": 200.0
  }
]
```

---

### Per-machine report
```http
GET /admin/reports/machines?from=1710000000&to=1720000000
```

```json
[
  {
    "machine_id": "srv-01",
    "total_allocations": 50,
    "total_cores_reserved": 200,
    "total_ram_mb_reserved": 102400,
    "avg_utilization_pct": 62.5
  }
]
```

---

## Machine Status

### Get full machine status grid
```http
GET /admin/machines/status
```

Returns all machines with probe status, type, cloud metadata, and MAC address:

```json
[
  {
    "id": "srv-01",
    "hostname": "server01.local",
    "ip": "192.168.1.10",
    "enabled": true,
    "status": "online",
    "last_probe_time": 1710500000,
    "probe_fail_count": 0,
    "type": "static",
    "cloud_provider": "",
    "cloud_instance_id": "",
    "mac_address": "AA:BB:CC:DD:EE:01",
    "cores_total": 16,
    "cores_reserved": 4,
    "ram_mb_total": 32768,
    "ram_mb_reserved": 4096
  }
]
```

`status` values: `"online"`, `"offline"`, `"probing"`.

---

## Cloud Provisioning

### Provision a cloud machine
```http
POST /admin/cloud/provision
Content-Type: application/json

{
  "provider":      "aws",
  "instance_type": "t3.xlarge",
  "region":        "eu-west-1",
  "image_id":      "ami-0abcdef1234567890",
  "cores":         4,
  "gpu_count":     0,
  "ram_mb":        16384,
  "disk_mb":       100000,
  "tags":          "{\"env\":\"prod\"}",
  "cores_min":     2,
  "ram_mb_min":    8192,
  "disk_mb_min":   50000
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `provider` | string | ✅ | `"aws"`, `"gcp"`, or `"azure"` |
| `instance_type` | string | — | Instance/VM size (e.g. `t3.xlarge`, `e2-standard-4`, `Standard_B2s`) |
| `region` | string | — | Cloud region / zone |
| `image_id` | string | AWS only | AMI ID (AWS), image family (GCP), or image URN (Azure) |
| `cores` | int | — | vCPUs to register in the pool |
| `gpu_count` | int | — | GPUs to register |
| `ram_mb` | int | — | RAM in MB to register |
| `disk_mb` | int | — | Disk in MB to register |
| `tags` | string | — | JSON string of key-value tags (AWS only) |
| `cores_min` | int | — | Minimum cores for flexible scheduling |
| `ram_mb_min` | int | — | Minimum RAM for flexible scheduling |
| `disk_mb_min` | int | — | Minimum disk for flexible scheduling |

Response `201`:
```json
{
  "machine_id": "cloud-i-0abc123def456",
  "provider": "aws",
  "status": "provisioning"
}
```

The machine is immediately registered in the pool and starts receiving probes. Once reachable, it transitions to `"online"` and becomes eligible for job dispatch.

---

### Deprovision a cloud machine
```http
POST /admin/cloud/deprovision
Content-Type: application/json

{
  "provider":    "aws",
  "instance_id": "i-0abc123def456"
}
```

Response `200`:
```json
{ "ok": true }
```

The instance is terminated via the provider CLI and removed from the machine pool. Deprovisioning returns `409` when the registered machine still has reserved resources.

### Auto-scaling

Automatic provision/deprovision can be enabled in `orchestrator.conf` so that cloud machines are created on-demand when no existing machine fits a job, and terminated when they become idle. No API calls are needed — the scheduler handles everything. See [Configuration — Auto-scaling](configuration.md#auto-scaling-automatic-provision--deprovision).

---

## Wake-on-LAN

### Send a WoL magic packet
```http
POST /admin/wol
Content-Type: application/json

{
  "machine_id":   "srv-01",
  "broadcast_ip": "192.168.1.255"
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `machine_id` | string | ✅ | ID of the machine to wake |
| `broadcast_ip` | string | — | Subnet broadcast address (default: `255.255.255.255`) |

The machine must have a `mac_address` defined in `provisioning.json`.

Response `200`:
```json
{ "ok": true, "message": "WoL packet sent" }
```

Returns `400` if the machine has no MAC address configured, or `404` if the machine ID is not found.

### List all apps
```http
GET /apps
```
Returns a JSON array of all app definitions. Available to any authenticated user.

---

### Get an app
```http
GET /apps/:app_id
```

---

### Create or update an app (admin)
```http
POST /admin/apps
Content-Type: application/json

{
  "app_id": "sim-engine",
  "name": "Simulation Engine",
  "command_template": "sim_engine.exe",
  "req_cores": 4,
  "req_ram_mb": 8192,
  "req_disk_mb": 2048,
  "req_gpu": 0,
  "fields": [
    { "name": "enable_logging", "type": "checkbox", "label": "Enable logging", "default": false },
    { "name": "algorithm",      "type": "select",   "label": "Algorithm", "options": ["fast","accurate","balanced"], "default": "balanced" },
    { "name": "iterations",     "type": "number",   "label": "Iterations", "default": 100 },
    { "name": "description",    "type": "text",     "label": "Description", "default": "" }
  ]
}
```

`PUT /admin/apps` also accepted (same behaviour — upsert).

**Field types:** `checkbox`, `select` (requires `options` array), `text`, `number`.

The `app_id` must contain only `[a-zA-Z0-9_-]`.

---

### Delete an app (admin)
```http
DELETE /admin/apps/:app_id
```

---

## User Management (admin)

### List users
```http
GET /admin/users
```

Response includes per-user job statistics:
```json
[
  {
    "user_id": "alice",
    "display_name": "Alice Smith",
    "email": "user@EXAMPLE.COM",
    "enabled": true,
    "created_at": 1709827200,
    "total_jobs": 5,
    "running": 1,
    "in_queue": 2,
    "held": 0,
    "finished": 1,
    "failed": 1
  }
]
```

---

### Create user(s)
```http
POST /admin/users
Content-Type: application/json

{
  "user_id": "alice",
  "display_name": "Alice Smith",
  "email": "user@EXAMPLE.COM",
  "password": "initial-password"
}
```
Also accepts an array for batch creation.

---

### Update a user
```http
PUT /admin/users
Content-Type: application/json

{ "user_id": "alice", "display_name": "Alice S.", "email": "user@EXAMPLE.COM", "enabled": false }
```

---

### Delete a user
```http
DELETE /admin/users
Content-Type: application/json

{ "user_id": "alice" }
```

---

## API Key Management (admin)

### List keys
```http
GET /admin/keys
```
Returns each key's administrative hash, masked display hash, label, role, bound user,
creation/expiration timestamps, and revocation state. The administrative hash is not
an authentication credential; it is used to identify a key for revocation.

---

### Create a key
```http
POST /admin/keys
Content-Type: application/json

{
  "label": "my-key",
  "role": "user",
  "user_id": "alice",
  "expires_at": 1712505600
}
```
`label` and `expires_at` are optional. `role` defaults to `"user"`; user-role keys must reference an enabled `user_id`. Administrators may create an unbound admin key explicitly with `role = "admin"`. `expires_at = 0` means no expiration; non-zero timestamps must be in the future.

Response `201`:
```json
{
  "api_key": "<64-hex-chars — shown only once>",
  "label": "my-key",
  "role": "user",
  "user_id": "alice",
  "expires_at": 1712505600
}
```

---

### Revoke a key
```http
DELETE /admin/keys
Content-Type: application/json

{ "key_hash": "<64-hex-hash returned by GET /admin/keys>" }
```

The raw `api_key` is also accepted when it is still available.

---

## Quotas (admin)

Quotas enforce resource limits per user and/or per app. Empty `user_id` or `app_id` acts as a wildcard.

### List quotas
```http
GET /admin/quotas
```

---

### Create or update a quota
```http
POST /admin/quotas
Content-Type: application/json

{
  "user_id": "alice",
  "app_id": "app1",
  "max_jobs": 10,
  "max_ram_mb": 32768,
  "max_cores": 8,
  "max_concurrent": 3
}
```
`PUT /admin/quotas` also accepted. A value of `0` means unlimited.

---

### Delete a quota
```http
DELETE /admin/quotas
Content-Type: application/json

{ "user_id": "alice", "app_id": "app1" }
```
