# Project Structure

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
│   │   ├── auth.c              # SHA-256 API key check, password auth, role resolution
│   │   ├── response.c          # JSON / error helpers
│   │   └── events.c            # SSE event broadcasting
│   ├── persistence/
│   │   ├── db.c                # SQLite — jobs, keys, users, quotas, stats, purge
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
├── web/                        # Built-in web UI
│   ├── index.html              # SPA shell
│   ├── style.css               # Styles (dark theme)
│   ├── app.js                  # Core logic, auth, navigation, auto-refresh
│   ├── dashboard.js            # Dashboard tab
│   ├── jobs.js                 # Job list, submit, detail, file upload/download
│   └── admin.js                # Users, keys, quotas, apps admin tabs
├── config/
│   ├── orchestrator.conf       # Main configuration
│   ├── provisioning.json       # Initial machine pool
│   └── apps/                   # Application definitions
│       ├── app1.json
│       └── app2.json
├── doc/                        # Documentation
│   ├── configuration.md        # Configuration reference, host setup, SSH
│   ├── api-reference.md        # REST API endpoints
│   ├── provisioning.md         # Machine provisioning
│   ├── web-ui.md               # Web UI guide
│   ├── examples.md             # PowerShell examples
│   └── architecture.md         # This file — project structure
├── tests/                      # API test scripts
└── CMakeLists.txt
```
