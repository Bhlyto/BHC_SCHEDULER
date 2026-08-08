param(
    [string]$BaseUrl = "http://127.0.0.1:8099",
    [string]$ApiKey = $env:API_KEY,
    [ValidateRange(1, 1000)]
    [int]$Count = 100,
    [ValidateRange(0, 2147483647)]
    [int]$Seed = 20260808,
    [ValidateRange(0, 3600)]
    [int]$MinSleepSeconds = 1,
    [ValidateRange(0, 3600)]
    [int]$MaxSleepSeconds = 60,
    [ValidateRange(1, 60)]
    [int]$PollIntervalSeconds = 1,
    [ValidateRange(0, 604800)]
    [int]$TimeoutSeconds = 0,
    [string]$ExpectedWorkerId = "local",
    [string]$WorkloadPath = "",
    [string]$ReportDirectory = ""
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

function Invoke-BhcRequest {
    param(
        [Parameter(Mandatory = $true)][string]$Method,
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$Body = ""
    )

    $parameters = @{
        Uri = "$($script:NormalizedBaseUrl)$Path"
        Method = $Method
        Headers = @{ "X-API-Key" = $script:ApiKeyValue }
        UseBasicParsing = $true
        TimeoutSec = 30
        ErrorAction = "Stop"
    }
    if ($Body) {
        $parameters.ContentType = "application/json"
        $parameters.Body = $Body
    }
    Invoke-WebRequest @parameters
}

function Invoke-BhcJson {
    param(
        [Parameter(Mandatory = $true)][string]$Method,
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$Body = ""
    )
    $response = Invoke-BhcRequest -Method $Method -Path $Path -Body $Body
    if (-not $response.Content) { return $null }
    $parsed = $response.Content | ConvertFrom-Json
    if ($parsed -is [Array]) {
        foreach ($item in $parsed) { Write-Output $item }
    } else {
        Write-Output $parsed
    }
}

function Convert-BhcContentToText {
    param($Content)
    if ($Content -is [byte[]]) {
        return [Text.Encoding]::UTF8.GetString($Content)
    }
    return [string]$Content
}

function Add-ObservedBatchStates {
    param($Batch)
    foreach ($state in @("created", "queued", "running", "succeeded", "failed", "cancelled")) {
        if ([int]$Batch.$state -gt 0) { [void]$script:ObservedStates.Add($state.ToUpperInvariant()) }
    }
}

function Add-ValidationFailure {
    param([string]$Message)
    $script:ValidationFailures.Add($Message)
}

if (-not $ApiKey) { throw "ApiKey is required (parameter -ApiKey or environment variable API_KEY)." }
if ($MinSleepSeconds -gt $MaxSleepSeconds) { throw "MinSleepSeconds must be <= MaxSleepSeconds." }

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $WorkloadPath) {
    $WorkloadPath = Join-Path $repositoryRoot "build\bin\Debug\bhc_synthetic_workload.exe"
}
if (-not (Test-Path -LiteralPath $WorkloadPath -PathType Leaf)) {
    throw "Synthetic workload not found at '$WorkloadPath'. Build target bhc_synthetic_workload first."
}
$resolvedWorkload = (Resolve-Path -LiteralPath $WorkloadPath).Path

if (-not $ReportDirectory) {
    $ReportDirectory = Join-Path $repositoryRoot "build\qualification"
}
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
$resolvedReportDirectory = (Resolve-Path -LiteralPath $ReportDirectory).Path

$script:NormalizedBaseUrl = $BaseUrl.TrimEnd("/")
$script:ApiKeyValue = $ApiKey
$script:ObservedStates = [Collections.Generic.HashSet[string]]::new()
$script:ValidationFailures = [Collections.Generic.List[string]]::new()

$startedAt = [DateTimeOffset]::UtcNow
$batch = $null
$finalBatch = $null
$jobResults = @()
$maxQueued = 0
$maxRunning = 0
$fatalError = ""
$workerSnapshot = $null

try {
    [void](Invoke-BhcJson -Method GET -Path "/auth/me")

    for ($attempt = 0; $attempt -lt 10; $attempt++) {
        $workers = @(Invoke-BhcJson -Method GET -Path "/workers")
        if ($ExpectedWorkerId) {
            $workerSnapshot = $workers | Where-Object { $_.id -eq $ExpectedWorkerId } |
                Select-Object -First 1
        } else {
            $workerSnapshot = $workers | Where-Object { $_.enabled -and $_.status -eq "online" } |
                Select-Object -First 1
        }
        if ($workerSnapshot -and $workerSnapshot.status -eq "online") { break }
        Start-Sleep -Seconds 1
    }
    if (-not $workerSnapshot) { throw "Expected worker '$ExpectedWorkerId' was not found." }
    if (-not $workerSnapshot.enabled -or $workerSnapshot.status -ne "online") {
        throw "Worker '$($workerSnapshot.id)' is not enabled and online (status=$($workerSnapshot.status))."
    }
    Write-Host ("Worker {0}: online, {1} scheduler core(s)." -f `
        $workerSnapshot.id, $workerSnapshot.cores_total)

    $jobDefinitions = [Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $Count; $index++) {
        $command = '"{0}" --index {1} --seed {2} --min-sleep {3} --max-sleep {4}' -f `
            $resolvedWorkload, $index, $Seed, $MinSleepSeconds, $MaxSleepSeconds
        $jobDefinitions.Add([ordered]@{
            command = $command
            app_id = "bhc-synthetic"
            priority = 50
            req_cores = 1
            req_ram_mb = 16
            req_disk_mb = 1
            timeout_seconds = $MaxSleepSeconds + 120
        })
    }

    $campaignName = "BHC v1 local qualification {0:yyyyMMdd-HHmmss}Z" -f $startedAt
    $requestBody = [ordered]@{
        name = $campaignName
        jobs = $jobDefinitions
    } | ConvertTo-Json -Depth 6 -Compress

    Write-Host "Submitting $Count synthetic jobs to $($script:NormalizedBaseUrl) ..."
    $batch = Invoke-BhcJson -Method POST -Path "/batches" -Body $requestBody
    if (-not $batch.id) { throw "Batch submission did not return an id." }
    Add-ObservedBatchStates $batch
    $maxQueued = [Math]::Max($maxQueued, [int]$batch.queued)
    $maxRunning = [Math]::Max($maxRunning, [int]$batch.running)
    Write-Host "Batch $($batch.id) accepted atomically."

    if ($TimeoutSeconds -eq 0) {
        $effectiveTimeout = [Math]::Max(300, ($Count * $MaxSleepSeconds) + 300)
    } else {
        $effectiveTimeout = $TimeoutSeconds
    }
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($effectiveTimeout)
    $nextProgressPercent = 0

    while ($true) {
        $finalBatch = Invoke-BhcJson -Method GET -Path "/batches/$($batch.id)"
        $queue = @(Invoke-BhcJson -Method GET -Path "/queue?limit=1000")
        $batchQueueCount = @($queue | Where-Object { $_.batch_id -eq $batch.id }).Count

        Add-ObservedBatchStates $finalBatch
        $maxQueued = [Math]::Max($maxQueued,
            [Math]::Max([int]$finalBatch.queued, [int]$batchQueueCount))
        $maxRunning = [Math]::Max($maxRunning, [int]$finalBatch.running)

        $percent = if ([int]$finalBatch.total -gt 0) {
            [int][Math]::Floor(100.0 * [int]$finalBatch.completed / [int]$finalBatch.total)
        } else { 0 }
        if ($percent -ge $nextProgressPercent -or [int]$finalBatch.completed -eq $Count) {
            Write-Host ("Progress {0,3}% | queued={1} running={2} succeeded={3} failed={4}" -f `
                $percent, $finalBatch.queued, $finalBatch.running,
                $finalBatch.succeeded, $finalBatch.failed)
            while ($nextProgressPercent -le $percent) { $nextProgressPercent += 10 }
        }

        if ([int]$finalBatch.completed -eq $Count) { break }
        if ([DateTimeOffset]::UtcNow -ge $deadline) {
            throw "Campaign timed out after $effectiveTimeout seconds."
        }
        Start-Sleep -Seconds $PollIntervalSeconds
    }

    $jobs = [Collections.Generic.List[object]]::new()
    $offset = 0
    while ($jobs.Count -lt $Count) {
        $page = @(Invoke-BhcJson -Method GET -Path "/jobs?limit=500&offset=$offset")
        foreach ($job in $page) {
            if ($job.batch_id -eq $batch.id) { $jobs.Add($job) }
        }
        if ($page.Count -lt 500) { break }
        $offset += 500
        if ($offset -gt 100000) { throw "Could not locate all jobs for batch $($batch.id)." }
    }
    if ($jobs.Count -ne $Count) {
        Add-ValidationFailure "Expected $Count listed jobs for the batch, found $($jobs.Count)."
    }

    $seenIndexes = [Collections.Generic.HashSet[int]]::new()
    $sleepValues = [Collections.Generic.List[int]]::new()
    [int64]$stdoutBytes = 0
    [int64]$stderrBytes = 0
    [int64]$resultBytes = 0
    [int64]$artifactBytes = 0
    $artifactCount = 0

    Write-Host "Auditing logs, results and artifacts for $($jobs.Count) jobs ..."
    foreach ($job in $jobs) {
        if ($job.status -ne "SUCCEEDED") {
            Add-ValidationFailure "Job $($job.id) ended in state $($job.status)."
            continue
        }
        if ($ExpectedWorkerId -and $job.machine_id -ne $ExpectedWorkerId) {
            Add-ValidationFailure "Job $($job.id) ran on '$($job.machine_id)', expected '$ExpectedWorkerId'."
        }

        $stdoutResponse = Invoke-BhcRequest -Method GET -Path "/jobs/$($job.id)/logs"
        $stderrResponse = Invoke-BhcRequest -Method GET -Path "/jobs/$($job.id)/logs/stderr"
        $resultResponse = Invoke-BhcRequest -Method GET -Path "/jobs/$($job.id)/output/result.json"
        $artifacts = @(Invoke-BhcJson -Method GET -Path "/jobs/$($job.id)/artifacts")

        $stdout = Convert-BhcContentToText $stdoutResponse.Content
        $stderr = Convert-BhcContentToText $stderrResponse.Content
        $resultText = Convert-BhcContentToText $resultResponse.Content
        $result = $resultText | ConvertFrom-Json
        $jobResults += $result

        $stdoutBytes += [Text.Encoding]::UTF8.GetByteCount($stdout)
        $stderrBytes += [Text.Encoding]::UTF8.GetByteCount($stderr)
        $resultBytes += [Text.Encoding]::UTF8.GetByteCount($resultText)
        foreach ($artifact in $artifacts) {
            $artifactCount++
            $artifactBytes += [int64]$artifact.size_bytes
        }

        if ($stdout -notmatch "START job=$([regex]::Escape($job.id))" -or
            $stdout -notmatch "RESULT job=$([regex]::Escape($job.id))") {
            Add-ValidationFailure "stdout markers are incomplete for job $($job.id)."
        }
        if ($stderr -notmatch "TRACE seed=$Seed") {
            Add-ValidationFailure "stderr trace is incomplete for job $($job.id)."
        }
        if ($result.job_id -ne $job.id -or $result.worker_id -ne $job.machine_id) {
            Add-ValidationFailure "Result identity mismatch for job $($job.id)."
        }
        if ([int]$result.seed -ne $Seed -or
            [int]$result.sleep_seconds -lt $MinSleepSeconds -or
            [int]$result.sleep_seconds -gt $MaxSleepSeconds) {
            Add-ValidationFailure "Variability metadata is invalid for job $($job.id)."
        }
        $expectedResult = ([int64]$result.operand_a + [int64]$result.operand_b) *
            [int64]$result.multiplier
        if ([int64]$result.result -ne $expectedResult) {
            Add-ValidationFailure "Algebra result mismatch for job $($job.id)."
        }
        if (-not $seenIndexes.Add([int]$result.index)) {
            Add-ValidationFailure "Duplicate synthetic index $($result.index)."
        }
        $sleepValues.Add([int]$result.sleep_seconds)

        $artifactTypes = @($artifacts | ForEach-Object { $_.type })
        foreach ($requiredType in @("stdout", "stderr", "output")) {
            if ($artifactTypes -notcontains $requiredType) {
                Add-ValidationFailure "Artifact '$requiredType' is missing for job $($job.id)."
            }
        }
    }

    $uniqueSleeps = @($sleepValues | Sort-Object -Unique)
    if ($Count -gt 1 -and $MaxSleepSeconds -gt $MinSleepSeconds -and $uniqueSleeps.Count -lt 2) {
        Add-ValidationFailure "Sleep variability was not observed."
    }
    if ($maxQueued -lt 1) { Add-ValidationFailure "No queued job was observed." }
    if ($maxRunning -lt 1) { Add-ValidationFailure "No running job was observed." }
    if ([int]$finalBatch.succeeded -ne $Count -or [int]$finalBatch.failed -ne 0 -or
        [int]$finalBatch.cancelled -ne 0) {
        Add-ValidationFailure "Terminal batch counters are not fully successful."
    }

    $sleepMeasure = $sleepValues | Measure-Object -Minimum -Maximum -Average -Sum
    $endedAt = [DateTimeOffset]::UtcNow
    $report = [ordered]@{
        schema_version = 1
        verdict = if ($script:ValidationFailures.Count -eq 0) { "PASS" } else { "FAIL" }
        started_at_utc = $startedAt.ToString("o")
        ended_at_utc = $endedAt.ToString("o")
        duration_seconds = [Math]::Round(($endedAt - $startedAt).TotalSeconds, 3)
        batch = [ordered]@{
            id = $batch.id
            name = $batch.name
            total = [int]$finalBatch.total
            succeeded = [int]$finalBatch.succeeded
            failed = [int]$finalBatch.failed
            cancelled = [int]$finalBatch.cancelled
        }
        configuration = [ordered]@{
            base_url = $script:NormalizedBaseUrl
            workload = $resolvedWorkload
            count = $Count
            seed = $Seed
            min_sleep_seconds = $MinSleepSeconds
            max_sleep_seconds = $MaxSleepSeconds
            expected_worker_id = $ExpectedWorkerId
            worker_cores = [int]$workerSnapshot.cores_total
        }
        queue = [ordered]@{
            max_queued = $maxQueued
            max_running = $maxRunning
            observed_states = @($script:ObservedStates | Sort-Object)
        }
        variability = [ordered]@{
            unique_sleep_values = $uniqueSleeps.Count
            observed_min_sleep_seconds = if ($sleepValues.Count) { [int]$sleepMeasure.Minimum } else { $null }
            observed_max_sleep_seconds = if ($sleepValues.Count) { [int]$sleepMeasure.Maximum } else { $null }
            average_sleep_seconds = if ($sleepValues.Count) { [Math]::Round([double]$sleepMeasure.Average, 3) } else { $null }
            accumulated_sleep_seconds = if ($sleepValues.Count) { [int64]$sleepMeasure.Sum } else { 0 }
        }
        accumulation = [ordered]@{
            audited_jobs = $jobs.Count
            stdout_files = $jobs.Count
            stdout_bytes = $stdoutBytes
            stderr_files = $jobs.Count
            stderr_bytes = $stderrBytes
            result_files = $jobResults.Count
            result_bytes = $resultBytes
            artifacts = $artifactCount
            artifact_bytes = $artifactBytes
        }
        failures = @($script:ValidationFailures)
    }
} catch {
    $fatalError = $_.Exception.Message
    $endedAt = [DateTimeOffset]::UtcNow
    $report = [ordered]@{
        schema_version = 1
        verdict = "ERROR"
        started_at_utc = $startedAt.ToString("o")
        ended_at_utc = $endedAt.ToString("o")
        duration_seconds = [Math]::Round(($endedAt - $startedAt).TotalSeconds, 3)
        batch_id = if ($batch) { $batch.id } else { $null }
        fatal_error = $fatalError
        failures = @($script:ValidationFailures)
    }
}

$reportStamp = $startedAt.ToString("yyyyMMdd-HHmmss")
$reportPath = Join-Path $resolvedReportDirectory "v1-bulk-$reportStamp.json"
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Host "Qualification report: $reportPath"
Write-Host "Verdict: $($report.verdict)"

if ($report.verdict -ne "PASS") {
    if ($fatalError) { Write-Error $fatalError } else { Write-Error ($script:ValidationFailures -join "`n") }
    exit 1
}
