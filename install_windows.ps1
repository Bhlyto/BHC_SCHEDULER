# install_windows.ps1
# Run as Administrator to install the orchestrator as a Windows service.

param(
    [string]$BinPath    = "C:\orch\orchestrator.exe",
    [string]$ConfPath   = "C:\orch\orchestrator.conf",
    [string]$ServiceName = "orchestrator"
)

Write-Host "Installing Orchestrator service..."

# Create config directory
New-Item -ItemType Directory -Force -Path (Split-Path $BinPath) | Out-Null
New-Item -ItemType Directory -Force -Path "C:\orch\jobs"        | Out-Null

# Copy binary and config if not already at destination
$srcBin  = Join-Path $PSScriptRoot "build\bin\Release\orchestrator.exe"
$srcConf = Join-Path $PSScriptRoot "config\orchestrator.conf"
$srcProv = Join-Path $PSScriptRoot "config\provisioning.json"

if (Test-Path $srcBin)  { Copy-Item $srcBin  $BinPath -Force }
if (Test-Path $srcConf) { Copy-Item $srcConf (Split-Path $BinPath | Join-Path -ChildPath "orchestrator.conf") -Force }
if (Test-Path $srcProv) { Copy-Item $srcProv (Split-Path $BinPath | Join-Path -ChildPath "provisioning.json") -Force }

# Register service
sc.exe create $ServiceName binPath= "`"$BinPath`" --conf `"$ConfPath`"" start= auto
sc.exe description $ServiceName "Orchestrator - cross-platform job scheduler with HTTP API"

Write-Host "Service '$ServiceName' created."
Write-Host "Generate an API key first:"
Write-Host "  $BinPath keygen --label my-app --conf $ConfPath"
Write-Host "Then start the service:"
Write-Host "  sc start $ServiceName"
