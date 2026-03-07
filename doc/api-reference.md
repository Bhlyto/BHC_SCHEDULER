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

---

### Available auth methods
```http
GET /auth/methods
```

```json
{ "methods": ["password", "api_key"] }
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
```

| Field | Type | Required | Description |
|---|---|---|---|
| `command` | string | ✅ | Command to execute |
| `priority` | int | — | Priority (lower = higher priority). Default: `50` |
| `req_cores` | int | — | CPU cores required. Default: `1` |
| `req_gpu` | int | — | GPUs required. Default: `0` |
| `req_ram_mb` | int | — | RAM in MB. Default: `0` |
| `req_disk_mb` | int | — | Disk in MB. Default: `0` |
| `timeout_seconds` | int | — | Per-job timeout (0 = use global default) |
| `app_id` | string | — | Associate job with an application definition |
| `input_files` | string[] | — | Expected input filenames; job starts as `HELD` until all are uploaded |

Response `201`:
```json
{
  "id":              "550e8400-e29b-41d4-a716-446655440000",
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
GET /jobs
```
Returns a JSON array of all job objects.

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

---

## Applications

App definitions are stored as JSON files in the `apps_dir` directory. Each app pre-defines resource requirements and custom form fields shown in the web UI.

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
    "email": "alice@example.com",
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
  "email": "alice@example.com",
  "password": "initial-password"
}
```
Also accepts an array for batch creation.

---

### Update a user
```http
PUT /admin/users
Content-Type: application/json

{ "user_id": "alice", "display_name": "Alice S.", "email": "alice@new.com", "enabled": false }
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
Returns keys with masked hashes (first 8 + last 4 characters).

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
All fields optional. Defaults: label=`"default"`, role=`"user"`, user_id=`""`, expires_at=`0` (never).

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

{ "api_key": "<raw-64-hex-key>" }
```

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
