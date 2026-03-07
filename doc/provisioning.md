# Machine Provisioning

## Static file

Define the machine pool in `provisioning.json` next to the executable. Supports individual entries and **range-based pools**:

```json
{
  "machines": [
    {
      "id":        "local",
      "hostname":  "localhost",
      "ip":        "127.0.0.1",
      "enabled":   true,
      "cores":     4,
      "gpu_count": 0,
      "ram_mb":    8192,
      "disk_mb":   102400
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

## Live provisioning

Machines can be added, updated, or removed at runtime via the API without restarting the daemon — see [Live Provisioning API](api-reference.md#live-provisioning).
