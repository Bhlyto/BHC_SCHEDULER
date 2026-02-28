#ifdef _WIN32
#include "platform.h"
#include "log.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

/*
 * service_win.c
 * Registers with the Windows Service Control Manager (SCM).
 * Install via:  sc create orchestrator binPath= "C:\path\orchestrator.exe"
 * Start via:    sc start orchestrator  (or run directly with --console)
 */

/* Forward declaration: implemented in main.c */
void orchestrator_run(void);

static volatile int              s_stop_requested = 0;
static SERVICE_STATUS            s_svc_status;
static SERVICE_STATUS_HANDLE     s_svc_handle;
static const char               *SERVICE_NAME = "orchestrator";

int  platform_stop_requested(void) { return s_stop_requested; }
void platform_request_stop(void)   { s_stop_requested = 1; }

static void set_service_status(DWORD state, DWORD exit_code, DWORD wait_hint)
{
    static DWORD checkpoint = 0;
    s_svc_status.dwCurrentState  = state;
    s_svc_status.dwWin32ExitCode = exit_code;
    s_svc_status.dwWaitHint      = wait_hint;
    if (state == SERVICE_START_PENDING)
        s_svc_status.dwControlsAccepted = 0;
    else
        s_svc_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
        s_svc_status.dwCheckPoint = 0;
    else
        s_svc_status.dwCheckPoint = ++checkpoint;
    SetServiceStatus(s_svc_handle, &s_svc_status);
}

static VOID WINAPI svc_ctrl_handler(DWORD ctrl)
{
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            log_info("platform", "SCM stop/shutdown received");
            set_service_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            s_stop_requested = 1;
            break;
        default:
            break;
    }
}

static VOID WINAPI svc_main(DWORD argc, LPSTR *argv)
{
    (void)argc; (void)argv;
    s_svc_handle = RegisterServiceCtrlHandlerA(SERVICE_NAME, svc_ctrl_handler);
    if (!s_svc_handle) return;

    s_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    set_service_status(SERVICE_START_PENDING, NO_ERROR, 3000);

    log_info("platform", "Windows service starting");
    set_service_status(SERVICE_RUNNING, NO_ERROR, 0);

    orchestrator_run();

    set_service_status(SERVICE_STOPPED, NO_ERROR, 0);
}

void platform_service_start(int argc, char **argv)
{
    /* Allow running in console mode with --console flag */
    int console_mode = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--console") == 0) { console_mode = 1; break; }

    if (console_mode) {
        log_info("platform", "Running in console mode (Ctrl+C to stop)");
        SetConsoleCtrlHandler(NULL, FALSE);
        orchestrator_run();
        return;
    }

    SERVICE_TABLE_ENTRYA dispatch_table[] = {
        { (LPSTR)SERVICE_NAME, svc_main },
        { NULL, NULL }
    };
    if (!StartServiceCtrlDispatcherA(dispatch_table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            /* Not launched as a service — fall back to console mode */
            log_info("platform", "Not running as service, falling back to console mode");
            orchestrator_run();
        } else {
            log_error("platform", "StartServiceCtrlDispatcher failed: %lu", err);
        }
    }
}
#endif /* _WIN32 */
