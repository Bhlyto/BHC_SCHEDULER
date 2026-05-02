# Machine Provisioning

## Static file

Define the machine pool in `provisioning.json` next to the executable. Supports individual entries and **range-based pools**:

```json
{
  "machines": [
    {
      "id":              "local",
      "hostname":        "localhost",
      "ip":             "127.0.0.1",
      "enabled":         true,
      "cores":           4,
      "gpu_count":       0,
      "ram_mb":          8192,
      "disk_mb":         102400,
      "type":            "static",
      "mac_address":     "AA:BB:CC:DD:EE:01",
      "cloud_provider":  "",
      "cloud_instance_id": "",
      "cores_min":       0,
      "ram_mb_min":      0,
      "disk_mb_min":     0
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

### New fields reference

| Field | Type | Default | Description |
|---|---|---|---|
| `type` | string | `"static"` | `"static"` for on-premise, `"cloud"` for cloud-provisioned |
| `mac_address` | string | `""` | MAC address in `AA:BB:CC:DD:EE:FF` format (required for Wake-on-LAN) |
| `cloud_provider` | string | `""` | Cloud provider name (`aws`, `gcp`, `azure`) — set automatically by cloud provisioning |
| `cloud_instance_id` | string | `""` | Cloud instance ID — set automatically by cloud provisioning |
| `cores_min` | int | `0` | Minimum cores for flexible resource scheduling (0 = same as `cores`) |
| `ram_mb_min` | int | `0` | Minimum RAM for flexible scheduling |
| `disk_mb_min` | int | `0` | Minimum disk for flexible scheduling |

## Live provisioning

Machines can be added, updated, or removed at runtime via the API without restarting the daemon — see [Live Provisioning API](api-reference.md#live-provisioning).

## Cloud provisioning

Cloud machines can be provisioned on-demand via `POST /admin/cloud/provision`. The orchestrator calls the cloud provider's CLI tool, creates the instance, and automatically registers it in the machine pool with `type: "cloud"`. See [Cloud Provisioning API](api-reference.md#cloud-provisioning) and [Configuration — Cloud Provisioning](configuration.md#cloud-provisioning) for details.

## Wake-on-LAN

Static machines with a `mac_address` can be powered on remotely via `POST /admin/wol`. See [WoL API](api-reference.md#wake-on-lan).
