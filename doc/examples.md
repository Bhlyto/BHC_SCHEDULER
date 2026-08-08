# PowerShell Examples

```powershell
$key     = "your-api-key-here"
$base    = "http://localhost:8080"
$headers = @{ "X-API-Key" = $key; "Content-Type" = "application/json" }

# ── Auth ──────────────────────────────────────────
# Login with password (returns a temporary API key)
$login = '{"user_id":"alice","password":"secret123"}'
Invoke-RestMethod -Uri "$base/auth/login" -Method POST `
    -Headers @{ "Content-Type" = "application/json" } -Body $login

# Change password
$chg = '{"old_password":"old","new_password":"new"}'
Invoke-RestMethod -Uri "$base/auth/change-password" -Method POST -Headers $headers -Body $chg

# ── Jobs ──────────────────────────────────────────
# Submit a job
$body = '{"command":"echo hello","req_cores":1,"req_ram_mb":512,"app_id":"app1"}'
Invoke-RestMethod -Uri "$base/jobs" -Method POST -Headers $headers -Body $body

# Submit with expected input files (job starts CREATED)
$body = '{"command":"process.exe","input_files":["data.csv"],"app_id":"app1"}'
Invoke-RestMethod -Uri "$base/jobs" -Method POST -Headers $headers -Body $body

# Release a held job
Invoke-RestMethod -Uri "$base/jobs/<id>/release" -Method POST -Headers $headers

# List all jobs
Invoke-RestMethod -Uri "$base/jobs" -Method GET -Headers $headers

# Get a specific job
Invoke-RestMethod -Uri "$base/jobs/<id>" -Method GET -Headers $headers

# List files for a job
Invoke-RestMethod -Uri "$base/jobs/<id>/files" -Method GET -Headers $headers

# Cancel a job
Invoke-RestMethod -Uri "$base/jobs/<id>" -Method DELETE -Headers $headers

# Purge all terminal jobs
Invoke-RestMethod -Uri "$base/jobs" -Method DELETE -Headers $headers

# Upload an input file
Invoke-RestMethod -Uri "$base/jobs/<id>/input/data.csv" -Method POST `
    -Headers @{ "X-API-Key" = $key } -ContentType "application/octet-stream" `
    -InFile "C:\data\data.csv"

# Download an output file
Invoke-RestMethod -Uri "$base/jobs/<id>/output/result.json" -Method GET `
    -Headers @{ "X-API-Key" = $key } -OutFile "C:\data\result.json"

# Get stdout / stderr log
Invoke-RestMethod -Uri "$base/jobs/<id>/log" -Method GET -Headers $headers
Invoke-RestMethod -Uri "$base/jobs/<id>/log/stderr" -Method GET -Headers $headers

# ── Apps ──────────────────────────────────────────
# List available apps
Invoke-RestMethod -Uri "$base/apps" -Method GET -Headers $headers

# Create / update an app (admin)
$app = '{"app_id":"my-app","name":"My App","command_template":"my_app.exe","req_cores":2,"req_ram_mb":4096,"req_disk_mb":512,"req_gpu":0,"fields":[]}'
Invoke-RestMethod -Uri "$base/admin/apps" -Method POST -Headers $headers -Body $app

# Delete an app (admin)
Invoke-RestMethod -Uri "$base/admin/apps/my-app" -Method DELETE -Headers $headers

# ── Users (admin) ────────────────────────────────
Invoke-RestMethod -Uri "$base/admin/users" -Method GET -Headers $headers
$user = '{"user_id":"bob","display_name":"Bob","email":"user@EXAMPLE.COM","password":"pass123"}'
Invoke-RestMethod -Uri "$base/admin/users" -Method POST -Headers $headers -Body $user

# ── API Keys (admin) ─────────────────────────────
Invoke-RestMethod -Uri "$base/admin/keys" -Method GET -Headers $headers
$newkey = '{"label":"ci-pipeline","role":"user","user_id":"bob"}'
Invoke-RestMethod -Uri "$base/admin/keys" -Method POST -Headers $headers -Body $newkey

# ── Quotas (admin) ───────────────────────────────
Invoke-RestMethod -Uri "$base/admin/quotas" -Method GET -Headers $headers
$quota = '{"user_id":"bob","app_id":"","max_jobs":10,"max_concurrent":3}'
Invoke-RestMethod -Uri "$base/admin/quotas" -Method POST -Headers $headers -Body $quota

# ── Cluster ──────────────────────────────────────
Invoke-RestMethod -Uri "$base/stats" -Method GET -Headers $headers
Invoke-RestMethod -Uri "$base/resources" -Method GET -Headers $headers

# Add a machine at runtime
$m = '{"id":"srv-03","hostname":"srv03.local","ip":"10.0.0.3","cores":32,"ram_mb":65536,"disk_mb":1048576}'
Invoke-RestMethod -Uri "$base/provision" -Method POST -Headers $headers -Body $m

# Remove a machine
Invoke-RestMethod -Uri "$base/provision/srv-03" -Method DELETE -Headers $headers

# ── Events & Reporting (admin) ───────────────────
# List recent events
Invoke-RestMethod -Uri "$base/admin/events?limit=50" -Method GET -Headers $headers

# List events filtered by category and time range
$from = [int](Get-Date "2025-03-01").ToUniversalTime().Subtract([datetime]"1970-01-01").TotalSeconds
$to   = [int](Get-Date).ToUniversalTime().Subtract([datetime]"1970-01-01").TotalSeconds
Invoke-RestMethod -Uri "$base/admin/events?category=job&from=$from&to=$to" -Method GET -Headers $headers

# Jobs-over-time report (daily granularity)
Invoke-RestMethod -Uri "$base/admin/reports/jobs?granularity=day&from=$from&to=$to" -Method GET -Headers $headers

# Per-user report
Invoke-RestMethod -Uri "$base/admin/reports/users?from=$from&to=$to" -Method GET -Headers $headers

# Per-application report
Invoke-RestMethod -Uri "$base/admin/reports/apps" -Method GET -Headers $headers

# Per-machine report
Invoke-RestMethod -Uri "$base/admin/reports/machines" -Method GET -Headers $headers

# ── Machine Status (admin) ───────────────────────
# Full machine status grid (probe status, type, MAC, etc.)
Invoke-RestMethod -Uri "$base/admin/machines/status" -Method GET -Headers $headers

# ── Cloud Provisioning (admin) ───────────────────
# Provision an AWS EC2 instance
$spec = @{
    provider      = "aws"
    instance_type = "t3.xlarge"
    region        = "eu-west-1"
    image_id      = "ami-0abcdef1234567890"
    cores         = 4
    ram_mb        = 16384
    disk_mb       = 100000
    cores_min     = 2
    ram_mb_min    = 8192
    disk_mb_min   = 50000
} | ConvertTo-Json
Invoke-RestMethod -Uri "$base/admin/cloud/provision" -Method POST -Headers $headers -Body $spec

# Provision a GCP instance
$spec = @{
    provider      = "gcp"
    instance_type = "e2-standard-4"
    region        = "us-central1-a"
    image_id      = "debian-11"
    cores         = 4
    ram_mb        = 16384
    disk_mb       = 50000
} | ConvertTo-Json
Invoke-RestMethod -Uri "$base/admin/cloud/provision" -Method POST -Headers $headers -Body $spec

# Provision an Azure VM
$spec = @{
    provider      = "azure"
    instance_type = "Standard_B2s"
    region        = "westeurope"
    image_id      = "UbuntuLTS"
    cores         = 2
    ram_mb        = 4096
    disk_mb       = 30000
} | ConvertTo-Json
Invoke-RestMethod -Uri "$base/admin/cloud/provision" -Method POST -Headers $headers -Body $spec

# Deprovision a cloud instance
$body = '{"provider":"aws","instance_id":"i-0abc123def456"}'
Invoke-RestMethod -Uri "$base/admin/cloud/deprovision" -Method POST -Headers $headers -Body $body

# ── Wake-on-LAN (admin) ─────────────────────────
# Wake a machine by its ID
$body = '{"machine_id":"srv-01"}'
Invoke-RestMethod -Uri "$base/admin/wol" -Method POST -Headers $headers -Body $body

# Wake with a specific broadcast address
$body = '{"machine_id":"srv-01","broadcast_ip":"192.168.1.255"}'
Invoke-RestMethod -Uri "$base/admin/wol" -Method POST -Headers $headers -Body $body
```
