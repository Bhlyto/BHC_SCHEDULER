param(
    [string]$BaseUrl = "http://localhost:8099"
)

$Key = $env:API_KEY
if (-not $Key) {
    Write-Error "API_KEY is required"
    exit 2
}

$Headers = @{ "X-API-Key" = $Key }
$Pass = 0
$Fail = 0

function Invoke-TestRequest([string]$Method, [string]$Path, $Body = $null, [hashtable]$RequestHeaders = $Headers) {
    try {
        $parameters = @{
            Uri = "$BaseUrl$Path"
            Method = $Method
            Headers = $RequestHeaders
            UseBasicParsing = $true
            TimeoutSec = 5
            ErrorAction = "Stop"
        }
        if ($null -ne $Body) {
            if ($Body -is [byte[]]) {
                $parameters.ContentType = "application/octet-stream"
                $parameters.Body = $Body
            } else {
                $parameters.ContentType = "application/json"
                $parameters.Body = ($Body | ConvertTo-Json -Depth 8 -Compress)
            }
        }
        $response = Invoke-WebRequest @parameters
        [pscustomobject]@{ Code = [int]$response.StatusCode; Body = $response.Content; Headers = $response.Headers }
    } catch {
        $response = $_.Exception.Response
        $code = if ($response) { [int]$response.StatusCode } else { 0 }
        $content = ""
        if ($response) {
            try {
                $reader = [IO.StreamReader]::new($response.GetResponseStream())
                $content = $reader.ReadToEnd()
                $reader.Dispose()
            } catch {}
        }
        if (-not $content -and $_.ErrorDetails.Message) {
            $content = $_.ErrorDetails.Message
        }
        [pscustomobject]@{ Code = $code; Body = $content; Headers = @{} }
    }
}

function Assert-Code([string]$Description, [int]$Expected, $Response) {
    if ($Response.Code -eq $Expected) {
        Write-Host "PASS: $Description" -ForegroundColor Green
        $script:Pass++
    } else {
        Write-Host "FAIL: $Description (expected $Expected, got $($Response.Code), body: $($Response.Body))" -ForegroundColor Red
        $script:Fail++
    }
}

function Assert-Match([string]$Description, [string]$Pattern, $Response) {
    if ($Response.Body -match $Pattern) {
        Write-Host "PASS: $Description" -ForegroundColor Green
        $script:Pass++
    } else {
        Write-Host "FAIL: $Description (body: $($Response.Body))" -ForegroundColor Red
        $script:Fail++
    }
}

$unauthorized = Invoke-TestRequest GET "/jobs" $null @{}
Assert-Code "missing API key is rejected" 401 $unauthorized

$me = Invoke-TestRequest GET "/auth/me"
Assert-Code "authenticated identity endpoint" 200 $me
Assert-Match "identity includes role" '"role":"(admin|user)"' $me

$raw = Invoke-TestRequest POST "/jobs" @{ command = "echo unsafe" }
Assert-Code "raw commands are rejected in app_only mode" 400 $raw

$job = Invoke-TestRequest POST "/jobs" @{
    app_id = "app1"
    parameters = @{ enable_logging = $false; parallel_mode = $true; dry_run = $false }
    req_cores = 9999
}
Assert-Code "registered application job is accepted" 201 $job
Assert-Match "server-owned app resources override client values" '"req_cores":4' $job

$injection = Invoke-TestRequest POST "/jobs" @{
    app_id = "app2"
    parameters = @{ algorithm = "fast;whoami"; verbose = $false }
}
Assert-Code "shell metacharacters are rejected" 400 $injection

$unknown = Invoke-TestRequest POST "/jobs" @{
    app_id = "app1"
    parameters = @{ unknown = $true }
}
Assert-Code "unknown app parameters are rejected" 400 $unknown

$invalidApp = Invoke-TestRequest POST "/admin/apps" @{
    app_id = "invalid-schema-smoke"
    name = "Invalid schema"
    command_template = "echo"
    req_cores = 1
    req_ram_mb = 0
    req_disk_mb = 0
    req_gpu = 0
    fields = @(@{ name = "unsafe'field"; type = "text" })
}
Assert-Code "unsafe application field names are rejected" 400 $invalidApp

$keyLabel = "api-smoke-revoke-$PID"
$secondaryKey = Invoke-TestRequest POST "/admin/keys" @{
    label = $keyLabel
    role = "admin"
    user_id = ""
}
Assert-Code "secondary admin key is created" 201 $secondaryKey

$keys = Invoke-TestRequest GET "/admin/keys"
Assert-Code "API keys can be listed" 200 $keys
if ($keys.Code -eq 200) {
    $keyInfo = $null
    $keyRecords = $keys.Body | ConvertFrom-Json
    foreach ($record in $keyRecords) {
        if ($record.label -eq $keyLabel) { $keyInfo = $record; break }
    }
    if ($keyInfo -and $keyInfo.key_hash -match '^[0-9a-f]{64}$') {
        $keyHash = [string]$keyInfo.key_hash
        $revoke = Invoke-TestRequest DELETE "/admin/keys" @{ key_hash = $keyHash }
        Assert-Code "API key is revoked by administrative hash" 200 $revoke
    } else {
        Write-Host "FAIL: key listing omits the administrative hash" -ForegroundColor Red
        $Fail++
    }
}

$traversal = Invoke-TestRequest GET "/jobs/%2e%2e/log"
Assert-Code "job path traversal is hidden" 404 $traversal

$held = Invoke-TestRequest POST "/jobs" @{
    app_id = "app1"
    parameters = @{ enable_logging = $false; parallel_mode = $false; dry_run = $true }
    input_files = @("input data.txt")
}
Assert-Code "job with expected input is accepted" 201 $held
Assert-Match "job waits in HELD state" '"status":"HELD"' $held

if ($held.Code -eq 201) {
    $heldJob = $held.Body | ConvertFrom-Json
    $upload = Invoke-TestRequest POST "/jobs/$($heldJob.id)/input/input%20data.txt" ([Text.Encoding]::UTF8.GetBytes("test-data")) @{ "X-API-Key" = $Key }
    Assert-Code "URL-decoded safe filename uploads" 200 $upload
}

$ui = Invoke-TestRequest GET "/"
if ($ui.Headers["X-Content-Type-Options"] -eq "nosniff") {
    Write-Host "PASS: security headers are present" -ForegroundColor Green
    $Pass++
} else {
    Write-Host "FAIL: security headers are missing" -ForegroundColor Red
    $Fail++
}

$csp = [string]$ui.Headers["Content-Security-Policy"]
if ($csp -match "script-src 'self'" -and $csp -notmatch "script-src[^;]*unsafe-inline") {
    Write-Host "PASS: CSP blocks inline JavaScript" -ForegroundColor Green
    $Pass++
} else {
    Write-Host "FAIL: CSP still allows inline JavaScript ($csp)" -ForegroundColor Red
    $Fail++
}

Write-Host "PASS=$Pass FAIL=$Fail"
if ($Fail -gt 0) { exit 1 }
