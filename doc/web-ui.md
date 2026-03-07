# Web UI

The orchestrator includes a built-in single-page web application served at the root URL (`/`). No additional web server is required.

## Accessing the UI

Open `http://<host>:<port>/` in a browser. Log in with a user ID and password, or paste an API key directly.

## Tabs

| Tab | Audience | Description |
|---|---|---|
| **Dashboard** | All users | Job count stats, resource utilisation, recent jobs |
| **Jobs** | All users | List, submit, cancel, and inspect jobs; view logs and files; upload/download |
| **Users** | Admin only | Create, edit, enable/disable, and delete user accounts |
| **API Keys** | Admin only | Create keys with role/user binding; revoke keys |
| **Quotas** | Admin only | Create and delete per-user/per-app resource quotas |
| **Apps** | Admin only | Create, edit, and delete application definitions |

## Job submission

When submitting a job from the web UI, users select an application from a dropdown. The app's pre-defined resource requirements (cores, RAM, disk, GPU) are displayed but not editable. Custom fields defined in the app's JSON (checkboxes, dropdowns, text inputs, numbers) are rendered dynamically.

## Auto-refresh

The active tab auto-refreshes every 60 seconds and immediately after any action (submit, cancel, create, delete, etc.).
