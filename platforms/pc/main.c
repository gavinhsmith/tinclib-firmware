/*
 * PC platform: the firmware as a desktop app. Plug the calculator into the
 * PC; tinclib then runs in USB device mode and shows up as a serial port,
 * and this app answers it using the PC's own network instead of an ESP.
 *
 *   tinclib-pc PORT        e.g. COM7 or /dev/ttyACM0
 *
 * "Wi-Fi" is the PC's own connection: the PC can't join another network,
 * so it has one Wi-Fi slot, "LAN" (TINC_SLOT_COUNT=1), locked
 * (protocol 0.2's Wi-Fi lock: WIFI_SET/WIFI_FORGET get ERR_LOCKED), and
 * every request goes out through the PC's networking stack.
 *
 * Every frame is printed to stdout as it passes, one line each:
 *     12.345  calc > #3 REQ_BEGIN GET http://example.com/ transcode
 *     12.347  calc < #3 REQ_BEGIN ok
 * Logs (request states, errors) go to stderr.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinc_core.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define NO_SOCK INVALID_SOCKET
#define sock_close closesocket
#define SOCK_WOULDBLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
#define SOCK_ERRNO() WSAGetLastError()
#define SEND_FLAGS 0
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
typedef int sock_t;
#define NO_SOCK (-1)
#define sock_close close
#define SOCK_WOULDBLOCK() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)
#define SOCK_ERRNO() errno
#define SEND_FLAGS MSG_NOSIGNAL
#endif

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- clock, heap, log ------------------------------------------------- */

uint32_t tinc_plat_millis(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
#endif
}

/* The core only uses this for its low-memory floor and in HELLO/STATUS. */
uint32_t tinc_plat_free_heap(void) { return 1u << 20; }

void tinc_plat_log(const char *msg) { say("%s", msg); }

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

/* ---- serial port to the calculator ----------------------------------- */

static int port_lost;

#ifdef _WIN32
static HANDLE port = INVALID_HANDLE_VALUE;

static int port_open(const char *name)
{
    char path[64];
    DCB d;
    COMMTIMEOUTS t = {MAXDWORD, 0, 0, 0, 20}; /* reads never wait; writes give up after 20 ms */

    snprintf(path, sizeof path, "\\\\.\\%s", name);
    port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (port == INVALID_HANDLE_VALUE)
        return -1;
    memset(&d, 0, sizeof d);
    d.DCBlength = sizeof d;
    GetCommState(port, &d);
    d.BaudRate = TINC_BAUD_DEFAULT;
    d.ByteSize = 8;
    d.Parity = NOPARITY;
    d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE;
    d.fOutxCtsFlow = d.fOutxDsrFlow = d.fOutX = d.fInX = FALSE;
    d.fDtrControl = DTR_CONTROL_ENABLE; /* CDC devices may not send until DTR is up */
    d.fRtsControl = RTS_CONTROL_ENABLE;
    SetCommState(port, &d);
    SetCommTimeouts(port, &t);
    port_lost = 0;
    return 0;
}

static void port_close(void)
{
    if (port != INVALID_HANDLE_VALUE)
        CloseHandle(port);
    port = INVALID_HANDLE_VALUE;
}

uint16_t tinc_plat_uart_available(void)
{
    COMSTAT st;
    DWORD errs;

    if (port_lost || !ClearCommError(port, &errs, &st)) {
        port_lost = 1;
        return 0;
    }
    return st.cbInQue > 0xFFFF ? 0xFFFF : (uint16_t)st.cbInQue;
}

uint8_t tinc_plat_uart_read(void)
{
    uint8_t b = 0;
    DWORD n;

    if (!port_lost && !ReadFile(port, &b, 1, &n, NULL))
        port_lost = 1;
    return b;
}

uint16_t tinc_plat_uart_write(const uint8_t *p, uint16_t n)
{
    DWORD w = 0;

    if (port_lost)
        return 0;
    if (!WriteFile(port, p, n, &w, NULL) && GetLastError() != ERROR_TIMEOUT)
        port_lost = 1;
    return (uint16_t)w;
}
#else
static int port = -1;

static int port_open(const char *name)
{
    struct termios t;

    port = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (port < 0)
        return -1;
    if (tcgetattr(port, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag |= CLOCAL | CREAD;
        tcsetattr(port, TCSANOW, &t);
    }
    port_lost = 0;
    return 0;
}

static void port_close(void)
{
    if (port >= 0)
        close(port);
    port = -1;
}

uint16_t tinc_plat_uart_available(void)
{
    int n = 0;

    if (port_lost || ioctl(port, FIONREAD, &n) < 0) {
        port_lost = 1;
        return 0;
    }
    return n > 0xFFFF ? 0xFFFF : (uint16_t)n;
}

uint8_t tinc_plat_uart_read(void)
{
    uint8_t b = 0;

    if (!port_lost && read(port, &b, 1) != 1)
        port_lost = 1;
    return b;
}

uint16_t tinc_plat_uart_write(const uint8_t *p, uint16_t n)
{
    ssize_t w;

    if (port_lost)
        return 0;
    w = write(port, p, n);
    if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        port_lost = 1;
    return w > 0 ? (uint16_t)w : 0;
}
#endif

/* ---- DNS on a thread (getaddrinfo blocks) ----------------------------- */

struct dns_job {
    char host[64];
    struct sockaddr_in addr;
    int ok, done, abandoned;
};

#ifdef _WIN32
static CRITICAL_SECTION dns_lock;
#define LOCK() EnterCriticalSection(&dns_lock)
#define UNLOCK() LeaveCriticalSection(&dns_lock)
#else
static pthread_mutex_t dns_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&dns_lock)
#define UNLOCK() pthread_mutex_unlock(&dns_lock)
#endif

/* ponytail: IPv4 only; add AF_UNSPEC + trying each address if a v6-only host matters */
static void dns_run(struct dns_job *j)
{
    struct addrinfo hints, *res = NULL;
    int ok, gone;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ok = getaddrinfo(j->host, NULL, &hints, &res) == 0 && res;
    LOCK();
    if (ok)
        memcpy(&j->addr, res->ai_addr, sizeof j->addr);
    j->ok = ok;
    j->done = 1;
    gone = j->abandoned;
    UNLOCK();
    if (res)
        freeaddrinfo(res);
    if (gone)
        free(j); /* the connection it was for was closed meanwhile */
}

#ifdef _WIN32
static DWORD WINAPI dns_thread(LPVOID arg) { dns_run(arg); return 0; }
#else
static void *dns_thread(void *arg) { dns_run(arg); return NULL; }
#endif

/* ---- TCP ------------------------------------------------------------- */

static sock_t sk = NO_SOCK;
static uint8_t tstate = TINC_TCP_IDLE;
static struct dns_job *job;
static uint16_t tport;
static int connecting;

static void set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
}

void tinc_plat_tcp_close(void)
{
    if (job) {
        LOCK();
        if (job->done) {
            UNLOCK();
            free(job);
        } else {
            job->abandoned = 1; /* the thread frees it */
            UNLOCK();
        }
        job = NULL;
    }
    if (sk != NO_SOCK)
        sock_close(sk);
    sk = NO_SOCK;
    connecting = 0;
    tstate = TINC_TCP_IDLE;
}

int tinc_plat_tcp_open(const char *host, uint16_t port)
{
    tinc_plat_tcp_close();
    job = calloc(1, sizeof *job);
    if (!job)
        return -1;
    snprintf(job->host, sizeof job->host, "%s", host);
    tport = port;
    tstate = TINC_TCP_BUSY;
#ifdef _WIN32
    {
        HANDLE t = CreateThread(NULL, 0, dns_thread, job, 0, NULL);
        if (!t)
            goto fail;
        CloseHandle(t);
    }
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, dns_thread, job) != 0)
            goto fail;
        pthread_detach(t);
    }
#endif
    return 0;
fail:
    free(job);
    job = NULL;
    tstate = TINC_TCP_IDLE;
    return -1;
}

static void start_connect(struct sockaddr_in *a)
{
    a->sin_port = htons(tport);
    sk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sk == NO_SOCK) {
        tstate = TINC_TCP_ERR_CONNECT;
        return;
    }
    set_nonblocking(sk);
    if (connect(sk, (struct sockaddr *)a, sizeof *a) == 0) {
        tstate = TINC_TCP_OPEN;
    } else if (SOCK_WOULDBLOCK()) {
        connecting = 1;
    } else {
        say("tcp: connect failed (%d)", SOCK_ERRNO());
        tstate = TINC_TCP_ERR_CONNECT;
    }
}

uint8_t tinc_plat_tcp_state(void)
{
    if (tstate == TINC_TCP_BUSY && job) {
        int done, ok;
        struct sockaddr_in a;
        LOCK();
        done = job->done;
        ok = job->ok;
        a = job->addr;
        UNLOCK();
        if (done) {
            free(job);
            job = NULL;
            if (ok)
                start_connect(&a);
            else
                tstate = TINC_TCP_ERR_DNS;
        }
    } else if (tstate == TINC_TCP_BUSY && connecting) {
        fd_set w, e;
        struct timeval zero = {0, 0};
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(sk, &w);
        FD_SET(sk, &e);
        if (select((int)sk + 1, NULL, &w, &e, &zero) > 0) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(sk, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            connecting = 0;
            tstate = err || FD_ISSET(sk, &e) ? TINC_TCP_ERR_CONNECT : TINC_TCP_OPEN;
            if (tstate != TINC_TCP_OPEN)
                say("tcp: connect failed (%d)", err);
        }
    }
    return tstate;
}

uint16_t tinc_plat_tcp_write(const uint8_t *p, uint16_t n)
{
    int w;

    if (tstate != TINC_TCP_OPEN)
        return 0;
    w = (int)send(sk, (const char *)p, n, SEND_FLAGS);
    if (w >= 0)
        return (uint16_t)w;
    if (!SOCK_WOULDBLOCK()) {
        say("tcp: send failed (%d)", SOCK_ERRNO());
        tstate = TINC_TCP_ERR_CONNECT;
    }
    return 0;
}

uint16_t tinc_plat_tcp_read(uint8_t *p, uint16_t n)
{
    int r;

    if (tstate != TINC_TCP_OPEN)
        return 0;
    r = (int)recv(sk, (char *)p, n, 0);
    if (r > 0)
        return (uint16_t)r;
    if (r == 0) {
        tstate = TINC_TCP_CLOSED;
    } else if (!SOCK_WOULDBLOCK()) {
        say("tcp: recv failed (%d)", SOCK_ERRNO());
        tstate = TINC_TCP_ERR_CONNECT;
    }
    return 0;
}

/* ---- "Wi-Fi": the PC's own connection -------------------------------- */

/* The address the PC would use to reach the internet; no packets are sent. */
static void local_ip(uint8_t ip[4])
{
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);

    memset(ip, 0, 4);
    if (s == NO_SOCK)
        return;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(s, (struct sockaddr *)&a, &len) == 0)
        memcpy(ip, &a.sin_addr, 4);
    sock_close(s);
}

#define PROFILE_NAME "LAN"

/* The one profile is the PC's own network. */
static void local_profiles(void)
{
    tinc_slots *s = tinc_core_slots();
    uint8_t i;

    memset(s, 0, sizeof *s);
    for (i = 0; i < TINC_SLOT_COUNT; i++)
        strcpy(s->ssid[i], PROFILE_NAME);
}

void tinc_plat_wifi_info(tinc_wifi_info *out)
{
    out->state = TINC_WIFI_CONNECTED;
    out->slot = 0; /* "LAN" */
    out->rssi = 0;
    local_ip(out->ip);
    out->locked = 1; /* the calculator can't change which network the PC is on */
}

/* Unreachable while locked; kept because the platform interface needs them. */
void tinc_plat_wifi_reconnect(void) {}
int tinc_plat_slots_save(const tinc_slots *s)
{
    (void)s;
    return 0;
}

/* ---- packet trace ----------------------------------------------------- */

static uint32_t started;

static void trace(int from_ce, const uint8_t *frame, uint16_t len)
{
    char line[512];
    uint32_t t = tinc_plat_millis() - started;

    tinc_describe(frame, len, line, sizeof line);
    printf("%6lu.%03lu  calc %c %s\n", (unsigned long)(t / 1000), (unsigned long)(t % 1000),
           from_ce ? '>' : '<', line);
    fflush(stdout);
}

/* ---- main loop -------------------------------------------------------- */

int main(int argc, char **argv)
{
    int connected = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s PORT   (e.g. COM7 or /dev/ttyACM0)\n"
                        "Stand in for the TINCLIB network board: plug the calculator into this PC\n"
                        "and give the serial port it shows up as.\n", argv[0]);
        return 2;
    }
#ifdef _WIN32
    {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        InitializeCriticalSection(&dns_lock);
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    setvbuf(stderr, NULL, _IONBF, 0);
    tinc_core_init();
    local_profiles();
    started = tinc_plat_millis();
    tinc_link_set_trace(trace);
    say("tinclib-pc: protocol v%d.%d, waiting for %s", TINC_PROTO_MAJOR, TINC_PROTO_MINOR, argv[1]);

    for (;;) {
        if (!connected) {
            if (port_open(argv[1]) != 0) {
                sleep_ms(500); /* the port only exists while the calculator is plugged in */
                continue;
            }
            say("%s: open", argv[1]);
            connected = 1;
        }
        tinc_link_step();
        tinc_poll();
        if (port_lost) {
            /* like a board reset: the calculator re-handshakes with HELLO */
            say("%s: lost; waiting for it to come back", argv[1]);
            port_close();
            tinc_plat_tcp_close();
            tinc_core_init();
            local_profiles();
            connected = 0;
            continue;
        }
        sleep_ms(1);
    }
}
