# Coming Up

## ~~1. Per-User / Per-Application Quotas~~

Configurable resource limits per user and per application: RAM, number of concurrent jobs, CPU cores, total job count.

**Planned changes:**
- New `quotas` table in SQLite: `user_id`, `app_id`, `max_jobs`, `max_ram_mb`, `max_cores`, `max_concurrent`.
- `src/resources/allocator.c` and `src/core/scheduler.c` check quotas before `alloc_acquire()` / `job_dispatch()`.
- Add `user_id` field to the `Job` struct for per-user accounting.
- CRUD endpoints `/admin/quotas` for live quota management.

---

## ~~2. Authentication Key Management~~ ✅ Done

Clear separation between user and admin API keys, with key rotation without requiring a restart.

**Planned changes:**
- New `api_keys` table in SQLite: `key`, `role` (`admin` | `user`), `label`, `created_at`, `expires_at`.
- `src/http/auth.c` resolves the role from the database instead of `provisioning.json`.
- Admin routes (`/admin/*`) require `role=admin`.
- Endpoints `/admin/keys` (GET / POST / DELETE) to create, list, and revoke keys.

---

## 3. Startup Environment Check

Customizable verification script executed before the HTTP port is opened.

**Planned changes:**
- New parameter in `config/orchestrator.conf`: `startup_check_script`.
- Hook in `src/main.c` before `mg_listen()`: runs the script, aborts startup if the exit code is non-zero.
- Network connectivity checks via `mg_connect()` (Mongoose) to verify that required servers are reachable.
- Detailed logs of all check results at startup.

---

## 4. Machine Reachability Check (On-Premise / Static Machines)

Before dispatching a job to a provisioned machine that is **not** cloud-managed, verify that the target is actually online and reachable. This avoids wasting scheduler cycles on machines that are powered off, unreachable, or under maintenance.

**Planned changes:**
- New field in `provisioning.json` machine / pool entries: `"type": "static"` (default) or `"cloud"`. Only `static` machines go through the reachability check; cloud machines use the readiness flow described in §5.
- New helper in `src/resources/probe.c`:
  ```c
  typedef enum { PROBE_PING, PROBE_TCP, PROBE_SSH } ProbeMethod;
  int  machine_is_reachable(const char *host, ProbeMethod method, int timeout_ms);
  ```
  - `PROBE_PING` — ICMP echo (platform `ping` command).
  - `PROBE_TCP`  — TCP connect to configurable port (default 22).
  - `PROBE_SSH`  — Lightweight SSH handshake via `libssh2` or shell `ssh -o ConnectTimeout`.
- New optional parameters in `config/orchestrator.conf`:
  - `probe_method`  — `ping` | `tcp` | `ssh` (default `tcp`).
  - `probe_port`    — target port for `tcp` / `ssh` probes (default `22`).
  - `probe_timeout` — milliseconds to wait before declaring unreachable (default `3000`).
  - `probe_retries` — number of retries before marking offline (default `2`).
- Integration in `src/core/scheduler.c` → `job_dispatch()`: before assigning a job to a static machine, call `machine_is_reachable()`; if it fails, skip the machine and try the next candidate — log a warning.
- Machine status tracked in `src/resources/registry.c`: a machine that fails probing is temporarily marked `OFFLINE`; a background timer re-probes it periodically and restores it to `ONLINE` when it responds.
- New endpoint `GET /admin/machines/status` returns the live reachability state of every registered machine.

---

## 5. Cloud Support (AWS / GCP / Azure)

Dynamic provisioning and deprovisioning of machines on major cloud providers, including a **readiness gate** that confirms a freshly created VM is actually ready to receive simulation workloads.

**Planned changes:**
- New `src/cloud/` module with an abstract provider interface:
  ```c
  typedef struct {
      int (*provision)(const char *pool, MachineSpec *spec, char *out_id);
      int (*deprovision)(const char *machine_id);
      int (*status)(const char *machine_id, char *out_status);
  } CloudProvider;
  ```
- Implementations via cloud REST APIs (Mongoose HTTP client): `src/cloud/aws.c`, `src/cloud/gcp.c`, `src/cloud/azure.c`.
- Credentials (service accounts) stored in `config/provisioning.json`.
- Integration into `src/resources/registry.c`: dynamically provisioned machines join the existing pool.
- Endpoints `/admin/cloud/provision` and `/admin/cloud/deprovision`.

### 5a. Cloud Machine Readiness Gate

A cloud VM being reported as "running" by the provider API does not mean it is ready for simulation work. A configurable readiness gate determines when the machine can start receiving jobs.

**Planned changes:**
- New optional field per cloud entry in `provisioning.json`:
  ```json
  "ready_check": {
      "method": "ping | ssh | tcp | script",
      "port": 22,
      "timeout_ms": 60000,
      "poll_interval_ms": 5000,
      "script": "scripts/wait_for_ready.sh"
  }
  ```
- Readiness methods:
  - **ping** — ICMP echo succeeds.
  - **tcp** — TCP connection to a given port (e.g. 22 for SSH, 3389 for RDP).
  - **ssh** — Full SSH handshake confirming the OS and SSH daemon are up.
  - **script** — Execute a user-defined script (e.g. copy test input files, run a health-check command over SSH). The machine is deemed ready when the script exits with code `0`.
- New helper `src/cloud/readiness.c`:
  ```c
  int cloud_wait_until_ready(const char *host, const ReadyCheck *cfg);
  ```
  Polls the chosen method at `poll_interval_ms` until success or `timeout_ms` is exceeded.
- Integration in the provisioning flow: after `CloudProvider.provision()` returns a machine ID, the scheduler calls `cloud_wait_until_ready()` before marking the machine as `ONLINE` in the registry. Jobs are not dispatched to the machine until the gate passes.
- If the readiness gate times out, the machine is flagged `PROVISION_FAILED` and optionally deprovisioned automatically (`"auto_deprovision_on_fail": true`).
