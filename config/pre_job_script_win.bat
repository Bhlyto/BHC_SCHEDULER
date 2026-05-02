@echo off
setlocal EnableDelayedExpansion

rem ============================================================
rem  BHC Scheduler — Windows pre-job script
rem
rem  Environment variables injected by the orchestrator:
rem    ORCH_JOB_ID        UUID of the job
rem    ORCH_INPUT_DIR     Local path to the input directory
rem    ORCH_OUTPUT_DIR    Local path to the output directory
rem    ORCH_MACHINE_IDS   Comma-separated list of target machine IDs
rem    ORCH_MACHINE_COUNT Number of machines allocated
rem
rem  Configuration (edit the block below):
rem    SSH_USER           Remote user account
rem    SSH_KEY            Path to the private key (no passphrase)
rem    REMOTE_WORK_DIR    Base work directory on remote machines
rem ============================================================

rem ── User configuration ───────────────────────────────────────
set SSH_USER=deploy
set SSH_KEY=%USERPROFILE%\.ssh\orch_key
set REMOTE_WORK_DIR=/tmp/orch
rem SSH options: BatchMode + PasswordAuthentication=no ensures immediate failure
rem if the key is missing/wrong — never hangs waiting for a password
set SSH_OPTS=-i "%SSH_KEY%" -o BatchMode=yes -o PasswordAuthentication=no -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o GlobalKnownHostsFile=NUL -o LogLevel=ERROR -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=2
rem ─────────────────────────────────────────────────────────────

rem Validate SSH key exists before even trying to connect
if not exist "%SSH_KEY%" (
    echo [pre_job] ERROR: SSH key not found at %SSH_KEY%
    echo [pre_job] Generate with: ssh-keygen -t ed25519 -f "%SSH_KEY%" -N ""
    echo [pre_job] Then copy with: ssh-copy-id -i "%SSH_KEY%.pub" %SSH_USER%@^<host^>
    exit /b 2
)

echo [pre_job] Job       : %ORCH_JOB_ID%
echo [pre_job] Machines  : %ORCH_MACHINE_IDS%
echo [pre_job] Input dir : %ORCH_INPUT_DIR%

rem Validate required variables
if "%ORCH_JOB_ID%"==""        ( echo [pre_job] ERROR: ORCH_JOB_ID not set  & exit /b 1 )
if "%ORCH_MACHINE_IDS%"==""   ( echo [pre_job] ERROR: ORCH_MACHINE_IDS not set & exit /b 1 )

rem ── Iterate over comma-separated machine IDs ─────────────────
set MACHINES=%ORCH_MACHINE_IDS%
set MACHINES=%MACHINES:,= %

for %%M in (%MACHINES%) do (
    echo [pre_job] Preparing machine: %%M

    rem 1. Verify connectivity
    ssh %SSH_OPTS% %SSH_USER%@%%M "echo ok" >nul 2>&1
    if errorlevel 1 (
        echo [pre_job] ERROR: Cannot reach %%M via SSH
        exit /b 1
    )

    rem 2. Create remote work directories
    ssh %SSH_OPTS% %SSH_USER%@%%M ^
        "mkdir -p %REMOTE_WORK_DIR%/%ORCH_JOB_ID%/input %REMOTE_WORK_DIR%/%ORCH_JOB_ID%/output"
    if errorlevel 1 (
        echo [pre_job] ERROR: Failed to create remote directories on %%M
        exit /b 1
    )

    rem 3. Copy input files (skip if input dir is empty)
    if exist "%ORCH_INPUT_DIR%\*" (
        scp %SSH_OPTS% -r "%ORCH_INPUT_DIR%\*" ^
            %SSH_USER%@%%M:%REMOTE_WORK_DIR%/%ORCH_JOB_ID%/input/
        if errorlevel 1 (
            echo [pre_job] ERROR: Failed to copy input files to %%M
            exit /b 1
        )
        echo [pre_job] Input files copied to %%M:%REMOTE_WORK_DIR%/%ORCH_JOB_ID%/input/
    ) else (
        echo [pre_job] No input files to copy for job %ORCH_JOB_ID%
    )

    echo [pre_job] Machine %%M ready
)

echo [pre_job] All machines ready — job %ORCH_JOB_ID% can start
exit /b 0
