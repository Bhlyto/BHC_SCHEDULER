# Coming Up

## 1. Per-User / Per-Application Quotas

Configurable resource limits per user and per application: RAM, number of concurrent jobs, CPU cores, total job count.

**Planned changes:**
- New `quotas` table in SQLite: `user_id`, `app_id`, `max_jobs`, `max_ram_mb`, `max_cores`, `max_concurrent`.
- `src/resources/allocator.c` and `src/core/scheduler.c` check quotas before `alloc_acquire()` / `job_dispatch()`.
- Add `user_id` field to the `Job` struct for per-user accounting.
- CRUD endpoints `/admin/quotas` for live quota management.

---

## 2. Authentication Key Management

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

## 4. Cloud Support (AWS / GCP / Azure)

Dynamic provisioning and deprovisioning of machines on major cloud providers.

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
