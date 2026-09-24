/*
 * PC platform: the firmware as a desktop app. Plug the calculator into the
 * PC; tinclib then runs in USB device mode and shows up as a serial port,
 * and this app answers it using the PC's own network instead of an ESP.
 *
 *   tinclib-pc PORT                    e.g. COM7 or /dev/ttyACM0
 *   tinclib-pc PORT --bridge BOARD     pass through to a real board instead
 *
 * "Wi-Fi" is the PC's own connection: the PC can't join another network,
 * so it has one Wi-Fi slot, "LAN" (TINC_SLOT_COUNT=1), locked
 * (the protocol's Wi-Fi lock: WIFI_SET/WIFI_FORGET get ERR_LOCKED), and
 * every request goes out through the PC's networking stack.
 *
 * Every frame is printed to stdout as it passes, one line each:
 *     12.345  calc > #3 REQ_BEGIN GET http://example.com/ transcode
 *     12.347  calc < #3 REQ_BEGIN ok
 * Logs (request states, errors) go to stderr.
 *
 * Bridge mode tests a real board (e.g. the ESP8266, powered from the PC's
 * USB) with the calculator plugged into the PC: bytes are passed through
 * unchanged between the two ports and traced the same way; the board, not
 * this app, answers the calculator.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tinc_core.h"
#include "tinc_frame.h"
#include "tinc_tls.h"

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

/* The PC's clock is already synced by the OS. */
uint32_t tinc_plat_time(void) { return (uint32_t)time(NULL); }

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

/* ---- serial ports: the calculator, and in bridge mode the board ------- */

/* Errors that just mean the port went away: the calculator's program ended
 * (tinclib shuts USB down) or the cable was pulled. Normal, so not logged. */
static int port_went_away(unsigned long code)
{
#ifdef _WIN32
    return code == ERROR_BAD_COMMAND || code == ERROR_DEVICE_NOT_CONNECTED ||
           code == ERROR_OPERATION_ABORTED || code == ERROR_ACCESS_DENIED ||
           code == ERROR_INVALID_HANDLE || code == ERROR_FILE_NOT_FOUND;
#else
    return code == 0 || code == EIO || code == ENXIO || code == ENODEV || code == EBADF;
#endif
}

#ifdef _WIN32
/* Overlapped I/O. A write is handed to the driver whole and allowed to finish
 * whenever the calculator is ready; it is never cancelled. The calculator
 * only services USB between its own work (e.g. while it isn't drawing), and a
 * write timeout that cancels a multi-packet transfer halfway wedges it: the
 * next writes fail with ERROR_GEN_FAILURE / ERROR_BAD_COMMAND. */
struct port {
    HANDLE h;
    const char *name;
    int lost; /* stop using it; the main loop waits for it to come back */
    OVERLAPPED rov, wov;
    uint8_t wbuf[TINC_PAYLOAD_LIMIT + TINC_OVERHEAD]; /* one whole frame */
    int writing;
};
#define PORT_INIT {.h = INVALID_HANDLE_VALUE}
#else
struct port {
    int fd;
    const char *name;
    int lost;
};
#define PORT_INIT {.fd = -1}
#endif

static struct port calc = PORT_INIT;

static void lose(struct port *p, const char *what, unsigned long code)
{
    if (!p->lost && !port_went_away(code))
        say("%s: %s failed (error %lu)", p->name, what, code);
    p->lost = 1;
}

#ifdef _WIN32
/* board: a USB-UART bridge whose DTR/RTS drive the ESP's GPIO0/EN; they stay
 * off so opening the port doesn't hold the chip in reset or the bootloader. */
static int port_open(struct port *p, const char *name, int board)
{
    char path[64];
    DCB d;
    COMMTIMEOUTS t = {MAXDWORD, 0, 0, 0, 0}; /* reads return at once; writes never time out */

    snprintf(path, sizeof path, "\\\\.\\%s", name);
    p->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED, NULL);
    if (p->h == INVALID_HANDLE_VALUE) {
        /* not plugged in yet is normal; anything else (in use, ...) is worth saying, once */
        static DWORD last;
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != last)
            say("%s: can't open (error %lu)", name, (unsigned long)e);
        last = e;
        return -1;
    }
    p->name = name;
    memset(&p->rov, 0, sizeof p->rov);
    memset(&p->wov, 0, sizeof p->wov);
    p->rov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    p->wov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    p->writing = 0;
    memset(&d, 0, sizeof d);
    d.DCBlength = sizeof d;
    GetCommState(p->h, &d);
    d.BaudRate = TINC_BAUD_DEFAULT;
    d.ByteSize = 8;
    d.Parity = NOPARITY;
    d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE;
    d.fOutxCtsFlow = d.fOutxDsrFlow = d.fOutX = d.fInX = FALSE;
    /* CDC devices (the calculator) may not send until DTR is up */
    d.fDtrControl = board ? DTR_CONTROL_DISABLE : DTR_CONTROL_ENABLE;
    d.fRtsControl = board ? RTS_CONTROL_DISABLE : RTS_CONTROL_ENABLE;
    SetCommState(p->h, &d);
    SetCommTimeouts(p->h, &t);
    p->lost = 0;
    return 0;
}

static void port_close(struct port *p)
{
    if (p->h != INVALID_HANDLE_VALUE)
        CloseHandle(p->h); /* cancels a write still in flight */
    p->h = INVALID_HANDLE_VALUE;
    if (p->rov.hEvent)
        CloseHandle(p->rov.hEvent);
    if (p->wov.hEvent)
        CloseHandle(p->wov.hEvent);
    p->rov.hEvent = p->wov.hEvent = NULL;
    p->writing = 0;
}

static uint16_t port_available(struct port *p)
{
    COMSTAT st;
    DWORD errs;

    if (p->lost)
        return 0;
    if (!ClearCommError(p->h, &errs, &st)) {
        lose(p, "status", GetLastError());
        return 0;
    }
    return st.cbInQue > 0xFFFF ? 0xFFFF : (uint16_t)st.cbInQue;
}

/* Only called for bytes port_available() says are waiting; with the read
 * timeouts above it completes at once. */
static uint16_t port_read(struct port *p, uint8_t *b, uint16_t n)
{
    DWORD got = 0;

    if (p->lost)
        return 0;
    if (!ReadFile(p->h, b, n, &got, &p->rov) &&
        (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(p->h, &p->rov, &got, TRUE)))
        lose(p, "read", GetLastError());
    return (uint16_t)got;
}

/* Takes nothing while the previous write is still in flight; otherwise copies
 * the bytes out and starts the write. */
static uint16_t port_write(struct port *p, const uint8_t *b, uint16_t n)
{
    DWORD w, e;

    if (p->lost)
        return 0;
    if (p->writing) {
        if (!GetOverlappedResult(p->h, &p->wov, &w, FALSE)) {
            e = GetLastError();
            if (e != ERROR_IO_INCOMPLETE)
                lose(p, "write", e);
            return 0;
        }
        p->writing = 0;
    }
    if (n > sizeof p->wbuf)
        n = sizeof p->wbuf;
    memcpy(p->wbuf, b, n);
    if (!WriteFile(p->h, p->wbuf, n, NULL, &p->wov)) {
        e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            lose(p, "write", e);
            return 0;
        }
        p->writing = 1;
    }
    return n;
}
#else
static int port_open(struct port *p, const char *name, int board)
{
    struct termios t;

    p->fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->fd < 0)
        return -1;
    p->name = name;
    if (tcgetattr(p->fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag |= CLOCAL | CREAD;
        tcsetattr(p->fd, TCSANOW, &t);
    }
    if (board) {
        /* DTR/RTS drive the ESP's GPIO0/EN on most dev boards: release them */
        int bits = TIOCM_DTR | TIOCM_RTS;
        ioctl(p->fd, TIOCMBIC, &bits);
    }
    p->lost = 0;
    return 0;
}

static void port_close(struct port *p)
{
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
}

static uint16_t port_available(struct port *p)
{
    int n = 0;

    if (p->lost)
        return 0;
    if (ioctl(p->fd, FIONREAD, &n) < 0) {
        lose(p, "status", (unsigned long)errno);
        return 0;
    }
    return n > 0xFFFF ? 0xFFFF : (uint16_t)n;
}

static uint16_t port_read(struct port *p, uint8_t *b, uint16_t n)
{
    ssize_t r;

    if (p->lost)
        return 0;
    r = read(p->fd, b, n);
    if (r <= 0)
        lose(p, "read", r < 0 ? (unsigned long)errno : 0);
    return r > 0 ? (uint16_t)r : 0;
}

static uint16_t port_write(struct port *p, const uint8_t *b, uint16_t n)
{
    ssize_t w;

    if (p->lost)
        return 0;
    w = write(p->fd, b, n);
    if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        lose(p, "write", (unsigned long)errno);
    return w > 0 ? (uint16_t)w : 0;
}
#endif

uint16_t tinc_plat_uart_available(void) { return port_available(&calc); }

uint8_t tinc_plat_uart_read(void)
{
    uint8_t b = 0;

    port_read(&calc, &b, 1);
    return b;
}

uint16_t tinc_plat_uart_write(const uint8_t *p, uint16_t n) { return port_write(&calc, p, n); }

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

/* ---- TCP (lib/tinc_tls adds https on top) ---------------------------- */

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

void tinc_raw_close(void)
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

int tinc_raw_open(const char *host, uint16_t port)
{
    tinc_raw_close();
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

/* Reads go straight to the socket, so nothing is ever left buffered here. */
int tinc_raw_pending(void) { return 0; }

uint8_t tinc_raw_state(void)
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

uint16_t tinc_raw_write(const uint8_t *p, uint16_t n)
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

uint16_t tinc_raw_read(uint8_t *p, uint16_t n)
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

/* ---- bridge mode ------------------------------------------------------ */

/* One direction of the pass-through: bytes go on unchanged, and a parser
 * alongside picks out the frames to trace. */
struct pipe {
    struct port *from, *to;
    int from_ce;
    uint8_t buf[512];
    uint16_t len, off;
    tinc_parser ps;
    uint8_t frame[TINC_FRAME_BUF(TINC_PAYLOAD_LIMIT)];
    uint32_t last_byte_at;
};

static void pipe_init(struct pipe *d, struct port *from, struct port *to, int from_ce)
{
    d->from = from;
    d->to = to;
    d->from_ce = from_ce;
    d->len = d->off = 0;
    tinc_parser_init(&d->ps, d->frame, sizeof d->frame);
}

static void pipe_step(struct pipe *d)
{
    uint16_t i, n;
    uint32_t now;

    if (d->off == d->len) {
        n = port_available(d->from);
        if (!n)
            return;
        d->len = port_read(d->from, d->buf, n < sizeof d->buf ? n : sizeof d->buf);
        d->off = 0;
        now = tinc_plat_millis();
        if (now - d->last_byte_at > TINC_INTERBYTE_RESET_MS)
            tinc_parser_reset(&d->ps);
        d->last_byte_at = now;
        for (i = 0; i < d->len; i++)
            if (tinc_parser_feed(&d->ps, d->buf[i]) == TINC_PARSE_FRAME)
                trace(d->from_ce, d->frame, (uint16_t)(TINC_OVERHEAD + d->ps.len));
    }
    d->off = (uint16_t)(d->off + port_write(d->to, d->buf + d->off, (uint16_t)(d->len - d->off)));
}

/* Either port may come and go; the calculator re-handshakes with HELLO. */
static void bridge(const char *calc_name, const char *board_name)
{
    static struct port board = PORT_INIT;
    static struct pipe up, down;
    int calc_open = 0, board_open = 0;

    say("tinclib-pc: bridging %s (calculator) <-> %s (board)", calc_name, board_name);
    pipe_init(&up, &calc, &board, 1);
    pipe_init(&down, &board, &calc, 0);
    for (;;) {
        if (!board_open && port_open(&board, board_name, 1) == 0) {
            say("%s: open", board_name);
            board_open = 1;
        }
        if (!calc_open && port_open(&calc, calc_name, 0) == 0) {
            say("%s: open", calc_name);
            calc_open = 1;
        }
        if (!board_open || !calc_open) {
            sleep_ms(20); /* the calculator gives the port ~600 ms to answer HELLO once it appears */
            continue;
        }
        pipe_step(&up);
        pipe_step(&down);
        if (calc.lost || board.lost) {
            struct port *p = calc.lost ? &calc : &board;
            say("%s: disconnected; waiting for it to come back", p->name);
            port_close(p);
            *(p == &calc ? &calc_open : &board_open) = 0;
            pipe_init(&up, &calc, &board, 1); /* drop whatever was half passed on */
            pipe_init(&down, &board, &calc, 0);
            continue;
        }
        sleep_ms(1);
    }
}

/* ---- main loop -------------------------------------------------------- */

int main(int argc, char **argv)
{
    int connected = 0;

    if (argc != 2 && !(argc == 4 && strcmp(argv[2], "--bridge") == 0)) {
        fprintf(stderr, "usage: %s PORT [--bridge BOARD_PORT]   (e.g. COM7 or /dev/ttyACM0)\n"
                        "Stand in for the TINCLIB network board: plug the calculator into this PC\n"
                        "and give the serial port it shows up as. With --bridge, pass everything\n"
                        "through to a real board on BOARD_PORT instead, traced the same way.\n",
                argv[0]);
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
    started = tinc_plat_millis();
    if (argc == 4) {
        bridge(argv[1], argv[3]); /* runs until killed */
        return 0;
    }
    tinc_core_init();
    local_profiles();
    tinc_link_set_trace(trace);
    say("tinclib-pc: protocol v%d.%d, waiting for %s", TINC_PROTO_MAJOR, TINC_PROTO_MINOR, argv[1]);

    for (;;) {
        if (!connected) {
            if (port_open(&calc, argv[1], 0) != 0) {
                sleep_ms(20); /* the port appears ~600 ms before the calculator gives up on HELLO */
                continue;
            }
            say("%s: open", argv[1]);
            connected = 1;
        }
        tinc_link_step();
        tinc_poll();
        if (calc.lost) {
            /* like a board reset: the calculator re-handshakes with HELLO */
            say("%s: calculator disconnected; waiting for it to come back", argv[1]);
            port_close(&calc);
            tinc_plat_tcp_close();
            tinc_core_init();
            local_profiles();
            connected = 0;
            continue;
        }
        sleep_ms(1);
    }
}
