# Web UI

The orchestrator includes a built-in single-page web application served at the root URL (`/`). No additional web server is required.

## Accessing the UI

Open `http://<host>:<port>/` in a browser. Log in with a user ID and password, or paste an API key directly.

## Tabs

| Tab | Audience | Description |
|---|---|---|
| **Dashboard** | All users | Job count stats, resource utilisation, recent jobs |
| **Jobs** | All users | List, submit, cancel, and inspect jobs; view logs and files; upload/download |
| **Reports** | Admin only | Charts and tables for jobs over time, per-user, per-app, and per-machine usage; event log with category filter |
| **Machines** | Admin only | Live machine status grid (online/offline/probing), Wake-on-LAN buttons, cloud provisioning form |
| **Users** | Admin only | Create, edit, enable/disable, and delete user accounts |
| **API Keys** | Admin only | Create keys with role/user binding; revoke keys |
| **Quotas** | Admin only | Create and delete per-user/per-app resource quotas |
| **Apps** | Admin only | Create, edit, and delete application definitions |

## Job submission

When submitting a job from the web UI, users select an application from a dropdown. The app's pre-defined resource requirements (cores, RAM, disk, GPU) are displayed but not editable. Custom fields defined in the app's JSON (checkboxes, dropdowns, text inputs, numbers) are rendered dynamically.

## Reports tab

The Reports tab provides built-in analytics for administrators:

- **Date range selector** — filter by 24h, 7 days, 30 days, 90 days, or all time
- **Granularity** — choose hour, day, or month for time-series charts
- **Jobs Over Time** — bar chart + table showing total / finished / failed jobs per period
- **Per User** — resource consumption breakdown by user
- **Per Application** — job success/failure rates per app
- **Per Machine** — allocation counts and utilisation per machine
- **Event Log** — filterable stream of system events (job dispatches, cloud provisioning, machine status changes, auth events)

All charts are rendered as pure CSS bar charts — no external charting library is required.

## Machines tab

The Machines tab shows a live status grid of all registered machines:

- Each card displays hostname, IP, type (static/cloud), resource capacity, and current probe status
- Cards are **color-coded**: green (online), red (offline), yellow (probing)
- **Wake-on-LAN button** appears on static machines that have a MAC address configured
- **Cloud provisioning form** at the bottom allows admins to spin up new cloud instances (AWS, GCP, or Azure) directly from the UI

## Bastion mode

When `web_ui_enabled = 0` in `orchestrator.conf`, the web UI is completely disabled. All requests to `/` and `/web/*` return `403 Forbidden`. The REST API remains fully operational. This is useful for headless or API-only deployments.

## Auto-refresh

The active tab auto-refreshes every 60 seconds and immediately after any action (submit, cancel, create, delete, etc.).
