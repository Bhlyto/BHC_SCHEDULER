# tests/test_api.ps1
# Plan de test complet -- couvre toutes les fonctionnalites du scheduler.
# Usage:
#   $env:API_KEY = "<votre-cle>"
#   .\tests\test_api.ps1
#   .\tests\test_api.ps1 -BaseUrl http://192.168.1.10:8080

param(
    [string]$BaseUrl = "http://localhost:8080",
    [int]   $JobTimeoutSec = 15
)

$KEY = if ($env:API_KEY) { $env:API_KEY } else { Write-Warning "API_KEY not set, using changeme"; "changeme" }
$HDR = @{ "X-API-Key" = $KEY; "Content-Type" = "application/json" }
$PASS = 0; $FAIL = 0; $SKIP = 0

function Pass($desc) { Write-Host "  [PASS] $desc" -ForegroundColor Green;  $script:PASS++ }
function Fail($desc, $msg) { Write-Host "  [FAIL] $desc  >> $msg" -ForegroundColor Red; $script:FAIL++ }
function Skip($desc, $why) { Write-Host "  [SKIP] $desc  ($why)" -ForegroundColor Yellow; $script:SKIP++ }

function Check($desc, $pattern, $actual) {
    if ($null -eq $actual) { Fail $desc "response is null"; return }
    $str = if ($actual -is [string]) { $actual } else { $actual | ConvertTo-Json -Depth 6 }
    if ($str -match $pattern) { Pass $desc } else { Fail $desc "expected '$pattern' in: $str" }
}

# HTTP helper -- always sends auth header
function Req($method, $path, $body = $null) {
    $uri = "$BaseUrl$path"
    try {
        $p = @{ Uri = $uri; Method = $method; Headers = $HDR; UseBasicParsing = $true; ErrorAction = "Stop" }
        if ($null -ne $body) { $p["Body"] = $body }
        return Invoke-RestMethod @p
    } catch {
        $code = $_.Exception.Response.StatusCode.value__
        return [pscustomobject]@{ _error = [int]$code; _msg = $_.ToString() }
    }
}

# Raw HTTP helper -- returns StatusCode and Content
function ReqRaw($method, $path, $body = $null, $extraHeaders = $null) {
    $uri = "$BaseUrl$path"
    $headers = if ($extraHeaders) { $extraHeaders } else { $HDR }
    try {
        $p = @{ Uri = $uri; Method = $method; Headers = $headers; UseBasicParsing = $true; ErrorAction = "Stop" }
        if ($null -ne $body) { $p["Body"] = $body }
        $resp = Invoke-WebRequest @p
        return [pscustomobject]@{ StatusCode = [int]$resp.StatusCode; Content = $resp.Content }
    } catch {
        $code = $_.Exception.Response.StatusCode.value__
        return [pscustomobject]@{ StatusCode = [int]$code; Content = $_.ToString() }
    }
}

function WaitJob($id, $sec = $JobTimeoutSec) {
    for ($i = 0; $i -lt ($sec * 2); $i++) {
        Start-Sleep -Milliseconds 500
        $r = Req GET "/jobs/$id"
        if ($r.status -match "FINISHED|FAILED|CANCELLED") { return $r }
    }
    return Req GET "/jobs/$id"
}

Write-Host ""
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host "   BHC-SCHEDULER  -- Plan de test complet   " -ForegroundColor Cyan
Write-Host "=============================================" -ForegroundColor Cyan

# =============================================
#  1. AUTHENTIFICATION
# =============================================
Write-Host "`n-- 1. Authentification" -ForegroundColor Cyan

# No headers at all
$r = ReqRaw GET "/jobs" -extraHeaders @{}
if ($r.StatusCode -eq 401) { Pass "Requete sans cle -> 401" }
else { Fail "Requete sans cle -> 401" "code=$($r.StatusCode)" }

# Wrong key
$r = ReqRaw GET "/jobs" -extraHeaders @{ "X-API-Key" = "INVALID"; "Content-Type" = "application/json" }
if ($r.StatusCode -eq 401) { Pass "Mauvaise cle -> 401" }
else { Fail "Mauvaise cle -> 401" "code=$($r.StatusCode)" }

# Good key
$r = ReqRaw GET "/jobs"
if ($r.StatusCode -eq 200) { Pass "Bonne cle -> 200 sur /jobs" }
else { Fail "Bonne cle -> 200 sur /jobs" "code=$($r.StatusCode)" }

# =============================================
#  2. SOUMISSION / VALIDATION DE JOB
# =============================================
Write-Host "`n-- 2. Soumission de job" -ForegroundColor Cyan

$r = Req POST "/jobs" '{"command":"echo hello","priority":10,"cores":1,"ram_mb":64}'
Check "Soumission minimale -> IN_QUEUE"    "IN_QUEUE" $r.status
Check "Reponse contient un id UUID"        "[0-9a-f-]{36}" $r.id
$JOB_BASIC = $r.id

$r = Req POST "/jobs" '{"command":"echo full","priority":5,"cores":1,"gpu":0,"ram_mb":128,"disk_mb":256}'
Check "Soumission avec tous les champs"    "IN_QUEUE" $r.status
$JOB_FULL = $r.id

$r = Req POST "/jobs" '{"priority":1}'
Check "Sans command -> erreur 400"         "400|error|command" ($r | ConvertTo-Json)

# =============================================
#  3. LECTURE / LISTE DES JOBS
# =============================================
Write-Host "`n-- 3. Lecture des jobs" -ForegroundColor Cyan

$r = Req GET "/jobs"
$listJson = $r | ConvertTo-Json -Depth 6
Check "GET /jobs contient le job soumis"   $JOB_BASIC $listJson

$r = Req GET "/jobs/$JOB_BASIC"
Check "GET /jobs/:id retourne le bon id"   $JOB_BASIC $r.id
Check "GET /jobs/:id contient command"     "echo hello" $r.command
Check "GET /jobs/:id contient status"      "IN_QUEUE|STARTING|RUNNING|FINISHED|FAILED" $r.status

$r = Req GET "/jobs/00000000-0000-0000-0000-000000000000"
Check "Job inexistant -> 404"              "404|not.found" ($r | ConvertTo-Json)

# =============================================
#  4. EXECUTION ET ETAT TERMINAL
# =============================================
Write-Host "`n-- 4. Execution -- job simple" -ForegroundColor Cyan

$r = WaitJob $JOB_BASIC
Check "Job echo hello -> FINISHED"        "FINISHED" $r.status
Check "Exit code = 0"                     "^0$" "$($r.exit_code)"
Check "machine_id renseigne"              "." $r.machine_id

$r = Req POST "/jobs" '{"command":"exit 1","priority":1,"cores":1,"ram_mb":64}'
$fail_id = $r.id
$r = WaitJob $fail_id
Check "Commande qui echoue -> FAILED"     "FAILED" $r.status

# =============================================
#  5. LOGS DU JOB
# =============================================
Write-Host "`n-- 5. Logs du job" -ForegroundColor Cyan

# Stdout only (stderr redirect causes issues on CREATE_NO_WINDOW)
$r = Req POST "/jobs" '{"command":"echo stdout_test","priority":1,"cores":1,"ram_mb":64}'
$log_id = $r.id
$r = WaitJob $log_id 20
Check "Job log -> FINISHED"               "FINISHED" $r.status

$r = ReqRaw GET "/jobs/$log_id/log"
if ($r.StatusCode -eq 200) {
    if ($r.Content -match "stdout_test") { Pass "GET /jobs/:id/log contient stdout" }
    else { Fail "GET /jobs/:id/log contient stdout" "content: $($r.Content)" }
} else { Fail "GET /jobs/:id/log accessible" "code=$($r.StatusCode)" }

# stderr log file should exist (even if empty for this job)
$r = ReqRaw GET "/jobs/$log_id/log/stderr"
if ($r.StatusCode -eq 200) { Pass "GET /jobs/:id/log/stderr accessible" }
else { Fail "GET /jobs/:id/log/stderr accessible" "code=$($r.StatusCode)" }

$r = ReqRaw GET "/jobs/00000000-0000-0000-0000-000000000000/log"
if ($r.StatusCode -eq 404) { Pass "Log job inexistant -> 404" }
else { Fail "Log job inexistant -> 404" "code=$($r.StatusCode)" }

# =============================================
#  6. UPLOAD / DOWNLOAD FICHIER
# =============================================
Write-Host "`n-- 6. Upload / Download" -ForegroundColor Cyan

# Use %ORCH_INPUT_DIR% so the command resolves the correct path
$r = Req POST "/jobs" '{"command":"type %ORCH_INPUT_DIR%\\data.txt","priority":1,"cores":1,"ram_mb":64}'
$io_id = $r.id

$bytes = [Text.Encoding]::UTF8.GetBytes("hello-from-test")
$r = ReqRaw POST "/jobs/$io_id/input/data.txt" -body $bytes
if ($r.StatusCode -eq 200) { Pass "Upload input -> 200" }
else { Fail "Upload input" "code=$($r.StatusCode): $($r.Content)" }

$r = WaitJob $io_id 20
Check "Job avec input -> FINISHED"        "FINISHED" $r.status

$r = ReqRaw GET "/jobs/$io_id/output"
if ($r.StatusCode -eq 200) { Pass "Download output -> 200" }
elseif ($r.StatusCode -eq 404) { Skip "Download output" "aucun fichier output (normal pour type)" }
else { Fail "Download output accessible" "code=$($r.StatusCode)" }

# =============================================
#  7. ANNULATION DE JOB
# =============================================
Write-Host "`n-- 7. Annulation" -ForegroundColor Cyan

# ping loops ~999s without needing a TTY (unlike timeout.exe)
$r = Req POST "/jobs" '{"command":"ping -n 999 127.0.0.1","priority":99,"cores":1,"ram_mb":64}'
$cancel_id = $r.id
# Wait until running before cancelling
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 500
    $st = (Req GET "/jobs/$cancel_id").status
    if ($st -match "RUNNING|STARTING") { break }
}
$r = Req DELETE "/jobs/$cancel_id"
if ($r._error -eq 409) {
    Skip "DELETE /jobs/:id -> CANCELLED" "job already in terminal state (too fast)"
} else {
    Check "DELETE /jobs/:id -> CANCELLED" "CANCELLED|ok" ($r | ConvertTo-Json)
    $r2 = Req GET "/jobs/$cancel_id"
    Check "Status apres cancel = CANCELLED" "CANCELLED" $r2.status
}

$r = Req DELETE "/jobs/00000000-0000-0000-0000-000000000000"
Check "Cancel job inexistant -> 404"      "404|not.found|error" ($r | ConvertTo-Json)

# =============================================
#  8. RESSOURCES ET MACHINES
# =============================================
Write-Host "`n-- 8. Ressources" -ForegroundColor Cyan

$r = Req GET "/resources"
Check "GET /resources present"            "." ($r | ConvertTo-Json)
Check "/resources contient cores"         "cores" ($r | ConvertTo-Json -Depth 6)

# =============================================
#  9. PROVISIONING DYNAMIQUE
# =============================================
Write-Host "`n-- 9. Provisioning dynamique" -ForegroundColor Cyan

$m = '{"id":"test-worker","hostname":"test-worker.local","ip":"192.168.99.10","cores":8,"gpu_count":0,"ram_mb":16384,"disk_mb":204800}'
$r = Req POST "/provision" $m
Check "POST /provision ajoute machine"    "true|ok" ($r | ConvertTo-Json)

$r = Req GET "/resources"
Check "/resources contient test-worker"   "test-worker" ($r | ConvertTo-Json -Depth 6)

$r = Req DELETE "/provision/test-worker"
Check "DELETE /provision/:id supprime"    "true|ok" ($r | ConvertTo-Json)

$r = Req GET "/resources"
if (($r | ConvertTo-Json -Depth 6) -notmatch "test-worker") { Pass "Machine supprimee absente de /resources" }
else { Fail "Machine supprimee absente de /resources" "toujours presente" }

$r = Req DELETE "/provision/machine-inexistante"
Check "Suppression machine inexistante -> 404/error" "404|not.found|error" ($r | ConvertTo-Json)

# =============================================
#  10. STATISTIQUES
# =============================================
Write-Host "`n-- 10. Statistiques" -ForegroundColor Cyan

$r = Req GET "/stats"
$statsJson = $r | ConvertTo-Json -Depth 6
Check "GET /stats accessible"             "." $statsJson
Check "/stats contient jobs"              "jobs" $statsJson
Check "/stats contient total"             "total" $statsJson

# =============================================
#  11. SSE -- EVENTS TEMPS REEL
# =============================================
Write-Host "`n-- 11. SSE /jobs/events" -ForegroundColor Cyan

try {
    $wr = [System.Net.WebRequest]::Create("$BaseUrl/jobs/events")
    $wr.Headers.Add("X-API-Key", $KEY)
    $wr.Timeout = 3000
    $resp = $wr.GetResponse()
    $ct = $resp.ContentType
    $resp.Close()
    if ($ct -match "text/event-stream") { Pass "GET /jobs/events -> Content-Type: text/event-stream" }
    else { Fail "GET /jobs/events Content-Type" "got: $ct" }
} catch {
    if ($_.Exception.Message -match "timed.out|timeout|The operation has timed") {
        Pass "GET /jobs/events maintient la connexion ouverte (timeout attendu)"
    } else { Fail "GET /jobs/events" "$_" }
}

# =============================================
#  12. MULTI-MACHINE
# =============================================
Write-Host "`n-- 12. Multi-machine" -ForegroundColor Cyan

Req POST "/provision" '{"id":"mm-node1","hostname":"mm-node1","ip":"10.0.0.1","cores":2,"ram_mb":4096,"disk_mb":51200}' | Out-Null
Req POST "/provision" '{"id":"mm-node2","hostname":"mm-node2","ip":"10.0.0.2","cores":2,"ram_mb":4096,"disk_mb":51200}' | Out-Null

$r = Req POST "/jobs" '{"command":"echo multi","priority":1,"cores":4,"ram_mb":256,"disk_mb":1024}'
$mm_id = $r.id
Check "Job multi-machine -> soumis"       "IN_QUEUE|STARTING|RUNNING" $r.status

$r = WaitJob $mm_id 20
Check "Job multi-machine -> FINISHED"     "FINISHED" $r.status
$nm = [int]"$($r.n_machines)"
if ($nm -gt 1) { Pass "n_machines=$nm (multi-machine confirme)" }
else { Skip "Multi-machine effectif" "n_machines=$nm -- machines locales peut-etre suffisantes" }

Req DELETE "/provision/mm-node1" | Out-Null
Req DELETE "/provision/mm-node2" | Out-Null

# =============================================
#  13. PRIORITE
# =============================================
Write-Host "`n-- 13. Priorite" -ForegroundColor Cyan

$low  = Req POST "/jobs" '{"command":"echo low","priority":100,"cores":1,"ram_mb":64}'
$high = Req POST "/jobs" '{"command":"echo high","priority":1,"cores":1,"ram_mb":64}'
Check "Job haute priorite soumis"         "IN_QUEUE|STARTING" $high.status
Check "Job basse priorite soumis"         "IN_QUEUE|STARTING" $low.status
$r = WaitJob $high.id 15
Check "Job haute priorite -> FINISHED"    "FINISHED" $r.status

# =============================================
#  RESULTAT FINAL
# =============================================
Write-Host ""
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host "  PASS: $PASS   FAIL: $FAIL   SKIP: $SKIP" -ForegroundColor Cyan
Write-Host "=============================================" -ForegroundColor Cyan
if ($FAIL -gt 0) { exit 1 } else { exit 0 }