# tests/test_api.ps1
# Windows integration test using Invoke-RestMethod
# Usage: $env:API_KEY="your-key"; .\tests\test_api.ps1

$BASE = "http://localhost:8080"
$KEY  = if ($env:API_KEY) { $env:API_KEY } else { "changeme" }
$HDR  = @{ "X-API-Key" = $KEY; "Content-Type" = "application/json" }
$PASS = 0; $FAIL = 0

function Check($desc, $expected, $actual) {
    if ($actual -match $expected) {
        Write-Host "  PASS  $desc" -ForegroundColor Green
        $script:PASS++
    } else {
        Write-Host "  FAIL  $desc  (expected '$expected' in: $actual)" -ForegroundColor Red
        $script:FAIL++
    }
}

Write-Host "=== Orchestrator API Tests ==="

# Auth check
Write-Host "`n-- Auth --"
try {
    $r = Invoke-WebRequest -Uri "$BASE/jobs" -UseBasicParsing -ErrorAction Stop
    Check "No key -> 401" "401" "200"
} catch {
    Check "No key -> 401" "401" $_.Exception.Response.StatusCode.value__
}

# Submit job
Write-Host "`n-- Submit job --"
$body = '{"command":"echo hello","priority":10,"cores":1,"ram_mb":64}'
$r = Invoke-RestMethod -Uri "$BASE/jobs" -Method Post -Headers $HDR -Body $body
Check "Submit job" "IN_QUEUE" $r.status
$JOB_ID = $r.id
Write-Host "  Job ID: $JOB_ID"

# List jobs
Write-Host "`n-- List jobs --"
$r = Invoke-RestMethod -Uri "$BASE/jobs" -Headers $HDR
Check "List jobs" $JOB_ID ($r | ConvertTo-Json)

# Get job
Write-Host "`n-- Get job --"
$r = Invoke-RestMethod -Uri "$BASE/jobs/$JOB_ID" -Headers $HDR
Check "Get job by id" $JOB_ID $r.id

# Upload input
Write-Host "`n-- Upload input --"
$tmpFile = [System.IO.Path]::GetTempFileName()
"hello world" | Out-File $tmpFile -Encoding ascii
$bytes = [System.IO.File]::ReadAllBytes($tmpFile)
$r = Invoke-RestMethod -Uri "$BASE/jobs/$JOB_ID/input/input.txt" -Method Post -Headers $HDR -Body $bytes
Check "Upload input" "bytes" ($r | ConvertTo-Json)

# Resources
Write-Host "`n-- Resources --"
$r = Invoke-RestMethod -Uri "$BASE/resources" -Headers $HDR
Check "Resources list" "cores_total" ($r | ConvertTo-Json)

# Wait for finish
Write-Host "`n-- Wait for FINISHED (up to 10s) --"
$status = ""
for ($i = 0; $i -lt 20; $i++) {
    $r = Invoke-RestMethod -Uri "$BASE/jobs/$JOB_ID" -Headers $HDR
    $status = $r.status
    Write-Host "  status: $status"
    if ($status -eq "FINISHED" -or $status -eq "FAILED") { break }
    Start-Sleep -Milliseconds 500
}
Check "Job terminal state" "FINISHED|FAILED" $status

# Provision
Write-Host "`n-- Provision --"
$m = '{"id":"worker1","hostname":"worker1.local","ip":"192.168.1.10","cores":8,"gpu_count":1,"ram_mb":16384,"disk_mb":204800}'
$r = Invoke-RestMethod -Uri "$BASE/provision" -Method Post -Headers $HDR -Body $m
Check "Add machine" "True" ($r.ok.ToString())
$r = Invoke-RestMethod -Uri "$BASE/provision/worker1" -Method Delete -Headers $HDR
Check "Remove machine" "True" ($r.ok.ToString())

# Cancel job
Write-Host "`n-- Cancel job --"
$r = Invoke-RestMethod -Uri "$BASE/jobs" -Method Post -Headers $HDR -Body '{"command":"timeout 9999","priority":99,"cores":1}'
$cj = $r.id
$r = Invoke-RestMethod -Uri "$BASE/jobs/$cj" -Method Delete -Headers $HDR
Check "Cancel job" "CANCELLED" $r.status

Write-Host "`n=============================="
Write-Host "  Results: $PASS passed, $FAIL failed"
if ($FAIL -gt 0) { exit 1 }
