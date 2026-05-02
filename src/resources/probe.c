#include "resources.h"
#include "config.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/*
 * probe.c
 * Machine availability checking (PING, TCP, SSH) + background probe thread
 * + Wake-on-LAN magic packet.
 */

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
   static HANDLE  s_probe_thread = NULL;
   static volatile int s_probe_running = 0;
   static CRITICAL_SECTION s_probe_cs;
   static int s_probe_cs_init = 0;
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/wait.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <poll.h>
   static pthread_t s_probe_thread;
   static volatile int s_probe_running = 0;
#endif

/* ── TCP connect probe ─────────────────────────────────────────── */

static int probe_tcp(const char *host, int port, int timeout_ms)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return 0;

#ifdef _WIN32
    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) { freeaddrinfo(res); return 0; }

    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
    connect(sock, res->ai_addr, (int)res->ai_addrlen);

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(sock, &wset);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ok = (select(0, NULL, &wset, NULL, &tv) > 0);
    closesocket(sock);
#else
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return 0; }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    connect(sock, res->ai_addr, res->ai_addrlen);

    struct pollfd pfd = { .fd = sock, .events = POLLOUT };
    int ok = 0;
    if (poll(&pfd, 1, timeout_ms) > 0) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
        ok = (err == 0);
    }
    close(sock);
#endif
    freeaddrinfo(res);
    return ok;
}

/* ── Ping probe (uses system ping command) ─────────────────────── */

static int probe_ping(const char *host, int timeout_ms)
{
    /* Validate host: alphanumeric, dots, hyphens, colons only */
    for (const char *p = host; *p; p++) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == ':'))
            return 0;
    }

    char cmd[256];
    int timeout_s = (timeout_ms + 999) / 1000;
    if (timeout_s < 1) timeout_s = 1;
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "ping -n 1 -w %d %s >NUL 2>&1",
             timeout_ms, host);
#else
    snprintf(cmd, sizeof(cmd), "ping -c 1 -W %d %s >/dev/null 2>&1",
             timeout_s, host);
#endif
    return (system(cmd) == 0) ? 1 : 0;
}

/* ── Public: machine_is_reachable ──────────────────────────────── */

int machine_is_reachable(const char *host, ProbeMethod method,
                         int port, int timeout_ms)
{
    if (!host || !host[0]) return 0;

    switch (method) {
    case PROBE_TCP:
    case PROBE_SSH:
        return probe_tcp(host, (method == PROBE_SSH) ? 22 : port, timeout_ms);
    case PROBE_PING:
    default:
        return probe_ping(host, timeout_ms);
    }
}

/* ── probe_refresh_all: check all static machines ─────────────── */

void probe_refresh_all(ProbeMethod method, int port, int timeout_ms, int retries)
{
    int count = 0;
    Machine *machines = registry_all(&count);
    if (!machines || count == 0) return;

    for (int i = 0; i < count; i++) {
        Machine *m = &machines[i];
        if (m->type != MACHINE_TYPE_STATIC) continue;
        if (!m->enabled) continue;

        /* Skip localhost/loopback — they are always treated as reachable */
        const char *host = m->hostname[0] ? m->hostname : m->ip;
        if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0 ||
            strcmp(m->ip, "127.0.0.1") == 0) {
            m->probe_status = MACHINE_ONLINE;
            m->last_probe_time = time(NULL);
            continue;
        }

        m->probe_status = MACHINE_PROBING;
        int ok = 0;
        for (int r = 0; r <= retries && !ok; r++) {
            ok = machine_is_reachable(m->hostname[0] ? m->hostname : m->ip,
                                      method, port, timeout_ms);
        }
        m->probe_status    = ok ? MACHINE_ONLINE : MACHINE_OFFLINE;
        m->last_probe_time = time(NULL);
        if (!ok)
            m->probe_fail_count++;
        else
            m->probe_fail_count = 0;

        log_debug("probe", "%s -> %s", m->id, ok ? "ONLINE" : "OFFLINE");
    }
}

/* ── Background probe thread ──────────────────────────────────── */

typedef struct {
    int interval_ms;
    ProbeMethod method;
    int port;
    int timeout_ms;
    int retries;
} ProbeConfig;

static ProbeConfig s_probe_cfg;

#ifdef _WIN32
static DWORD WINAPI probe_thread_fn(LPVOID arg)
{
    (void)arg;
    /* Sleep first so machines start as ONLINE; probe fires after first interval */
    Sleep(s_probe_cfg.interval_ms);
    while (s_probe_running) {
        probe_refresh_all(s_probe_cfg.method, s_probe_cfg.port,
                          s_probe_cfg.timeout_ms, s_probe_cfg.retries);
        Sleep(s_probe_cfg.interval_ms);
    }
    return 0;
}
#else
static void *probe_thread_fn(void *arg)
{
    (void)arg;
    /* Sleep first so machines start as ONLINE; probe fires after first interval */
    usleep((useconds_t)s_probe_cfg.interval_ms * 1000);
    while (s_probe_running) {
        probe_refresh_all(s_probe_cfg.method, s_probe_cfg.port,
                          s_probe_cfg.timeout_ms, s_probe_cfg.retries);
        usleep((useconds_t)s_probe_cfg.interval_ms * 1000);
    }
    return NULL;
}
#endif

void probe_start_background(int interval_ms, ProbeMethod method,
                            int port, int timeout_ms, int retries)
{
    if (s_probe_running) return;

    s_probe_cfg.interval_ms = interval_ms > 0 ? interval_ms : 60000;
    s_probe_cfg.method      = method;
    s_probe_cfg.port        = port > 0 ? port : 22;
    s_probe_cfg.timeout_ms  = timeout_ms > 0 ? timeout_ms : 3000;
    s_probe_cfg.retries     = retries >= 0 ? retries : 2;
    s_probe_running         = 1;

    log_info("probe", "Starting background probe (interval=%dms, method=%d, port=%d)",
             interval_ms, method, port);

#ifdef _WIN32
    s_probe_thread = CreateThread(NULL, 0, probe_thread_fn, NULL, 0, NULL);
#else
    pthread_create(&s_probe_thread, NULL, probe_thread_fn, NULL);
#endif
}

void probe_stop_background(void)
{
    if (!s_probe_running) return;
    s_probe_running = 0;

#ifdef _WIN32
    if (s_probe_thread) {
        WaitForSingleObject(s_probe_thread, 5000);
        CloseHandle(s_probe_thread);
        s_probe_thread = NULL;
    }
#else
    pthread_join(s_probe_thread, NULL);
#endif
    log_info("probe", "Background probe stopped");
}

/* ── Legacy stub ────────────────────────────────────────────────── */

void probe_refresh(void)
{
    extern Config g_config;
    ProbeMethod m = PROBE_TCP;
    if (strcmp(g_config.probe_method, "ping") == 0) m = PROBE_PING;
    else if (strcmp(g_config.probe_method, "ssh") == 0) m = PROBE_SSH;
    probe_refresh_all(m, g_config.probe_port,
                      g_config.probe_timeout_ms, g_config.probe_retries);
}

/* ── Wake-on-LAN ────────────────────────────────────────────────── */

static int parse_mac(const char *mac, unsigned char out[6])
{
    unsigned int v[6];
    if (sscanf(mac, "%02x:%02x:%02x:%02x:%02x:%02x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) out[i] = (unsigned char)v[i];
    return 0;
}

int wol_send(const char *mac_address, const char *broadcast_ip)
{
    if (!mac_address || !mac_address[0]) return -1;

    unsigned char mac[6];
    if (parse_mac(mac_address, mac) != 0) {
        log_error("wol", "Invalid MAC address: %s", mac_address);
        return -1;
    }

    /* Build 102-byte magic packet: 6x 0xFF + 16x MAC */
    unsigned char packet[102];
    memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++)
        memcpy(packet + 6 + i * 6, mac, 6);

    const char *bcast = (broadcast_ip && broadcast_ip[0]) ? broadcast_ip : "255.255.255.255";

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return -1;
#else
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
#endif

    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(9);  /* WoL port */
    addr.sin_addr.s_addr = inet_addr(bcast);

    int rc = sendto(sock, (const char *)packet, 102, 0,
                    (struct sockaddr *)&addr, sizeof(addr));

#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif

    if (rc < 0) {
        log_error("wol", "Failed to send WoL to %s via %s", mac_address, bcast);
        return -1;
    }
    log_info("wol", "WoL sent to %s (broadcast %s)", mac_address, bcast);
    return 0;
}
