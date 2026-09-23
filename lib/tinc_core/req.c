/*
 * HTTP request state machine. One step per tinc_req_poll(); the body is
 * decoded (Content-Length / chunked / close-delimited) only as fast as the
 * CE pulls it with BODY_READ, so unread data backs up into TCP flow control.
 */
#include <string.h>
#include "tinc_core.h"

#define RQ_MAX    TINC_PAYLOAD_LIMIT /* url + user headers, as sent in REQ_BEGIN */
#define LINE_MAX  256                /* response header line */
#define HOST_MAX  64
#define STAGE_LEN 128
/* ponytail: rough floor for lwIP + our buffers on plain TCP; revisit with TLS. */
#define MIN_HEAP  6000u

enum { B_LEN, B_CHUNK_SIZE, B_CHUNK_DATA, B_CHUNK_CRLF, B_TRAILER, B_CLOSE, B_DONE };
enum { SEGS = 8 };

static struct {
    uint8_t state, err, flags, redirects, transcode;
    uint32_t timeout_ms, phase_at;

    char rq[RQ_MAX]; /* url, then user headers */
    uint16_t url_len, hdr_len;
    char host[HOST_MAX];
    uint16_t port;

    const char *seg[SEGS];
    uint16_t seg_len[SEGS];
    uint8_t seg_i;
    uint16_t seg_off;

    char line[LINE_MAX];
    uint16_t line_len;
    uint8_t line_over, got_len, chunked, got_loc;
    uint16_t status;
    uint32_t clen;
    char ctype[TINC_CTYPE_MAX + 1];

    uint8_t body, cz_digits, cz_ext;
    uint32_t remain;
    uint16_t tr_len;

    uint8_t stage[STAGE_LEN];
    uint16_t st_pos, st_len;
    tinc_tc tc;
} r;

static void log_num(const char *what, uint8_t v)
{
    char buf[24];
    size_t n = strlen(what);

    memcpy(buf, what, n);
    buf[n++] = ' ';
    buf[n++] = "0123456789ABCDEF"[v >> 4];
    buf[n++] = "0123456789ABCDEF"[v & 15];
    buf[n] = 0;
    tinc_plat_log(buf);
}

static void set_state(uint8_t s)
{
    r.state = s;
    r.phase_at = tinc_plat_millis();
    log_num("req state", s);
}

static void fail(uint8_t err)
{
    tinc_plat_tcp_close();
    r.err = err;
    set_state(TINC_RS_ERROR);
    log_num("req err", err);
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Case-insensitive prefix match. */
static int iprefix(const char *s, uint16_t n, const char *pre)
{
    uint16_t i;

    for (i = 0; pre[i]; i++)
        if (i >= n || lower((unsigned char)s[i]) != pre[i])
            return 0;
    return 1;
}

/* No spaces or control chars: they would inject into our request. */
static int clean(const char *s, uint16_t n)
{
    for (; n--; s++)
        if ((unsigned char)*s <= ' ' || *s == 0x7F)
            return 0;
    return 1;
}

/* Authority (host[:port]) of an http:// url of length n. */
static uint16_t authority(const char *u, uint16_t n)
{
    uint16_t i = 7;

    while (i < n && u[i] != '/' && u[i] != '?' && u[i] != '#')
        i++;
    return (uint16_t)(i - 7);
}

static uint8_t parse_url(void)
{
    const char *u = r.rq, *a, *path;
    uint16_t n = r.url_len, alen, hlen, plen, i;
    uint32_t port = 0;

    if (iprefix(u, n, "https://"))
        return TINC_ERR_UNSUPPORTED_SCHEME;
    if (!iprefix(u, n, "http://") || !clean(u, n))
        return TINC_ERR_BAD_ARG;

    a = u + 7;
    alen = authority(u, n);
    for (hlen = 0; hlen < alen && a[hlen] != ':'; hlen++)
        if (a[hlen] == '@')
            return TINC_ERR_BAD_ARG; /* no userinfo */
    if (hlen == 0 || hlen >= HOST_MAX)
        return TINC_ERR_BAD_ARG;
    if (hlen < alen) {
        if (hlen + 1 == alen)
            return TINC_ERR_BAD_ARG;
        for (i = (uint16_t)(hlen + 1); i < alen; i++) {
            if (a[i] < '0' || a[i] > '9' || (port = port * 10 + (uint32_t)(a[i] - '0')) > 65535)
                return TINC_ERR_BAD_ARG;
        }
        if (port == 0)
            return TINC_ERR_BAD_ARG;
    } else {
        port = 80;
    }
    memcpy(r.host, a, hlen);
    r.host[hlen] = 0;
    r.port = (uint16_t)port;

    path = a + alen;
    for (plen = 0; path + plen < u + n && path[plen] != '#'; plen++)
        ;

    r.seg[0] = "GET ";
    r.seg[1] = (plen == 0 || path[0] == '?') ? "/" : "";
    r.seg[2] = path;
    r.seg_len[2] = plen;
    r.seg[3] = " HTTP/1.1\r\nHost: ";
    r.seg[4] = a;
    r.seg_len[4] = alen;
    r.seg[5] = "\r\nAccept-Encoding: identity\r\nConnection: close\r\n"
               "User-Agent: tinclib-firmware\r\n";
    r.seg[6] = r.rq + r.url_len;
    r.seg_len[6] = r.hdr_len;
    r.seg[7] = "\r\n";
    for (i = 0; i < SEGS; i++)
        if (i != 2 && i != 4 && i != 6)
            r.seg_len[i] = (uint16_t)strlen(r.seg[i]);
    return TINC_OK;
}

static void start_connect(void)
{
    uint8_t e;

    tinc_plat_tcp_close();
    if ((e = parse_url()) != TINC_OK) {
        fail(e == TINC_ERR_UNSUPPORTED_SCHEME ? e : TINC_ERR_HTTP_PROTO);
        return;
    }
    if (tinc_plat_free_heap() < MIN_HEAP) {
        fail(TINC_ERR_NO_MEM);
        return;
    }
    r.seg_i = 0;
    r.seg_off = 0;
    r.line_len = 0;
    r.line_over = r.got_len = r.chunked = r.got_loc = 0;
    r.status = 0;
    r.clen = 0;
    r.ctype[0] = 0;
    r.st_pos = r.st_len = 0;
    memset(&r.tc, 0, sizeof r.tc);
    if (tinc_plat_tcp_open(r.host, r.port) != 0) {
        fail(TINC_ERR_CONNECT);
        return;
    }
    set_state(TINC_RS_CONNECTING);
}

void tinc_req_release(void)
{
    if (r.state != TINC_RS_IDLE)
        tinc_plat_tcp_close();
    r.state = TINC_RS_IDLE;
    r.err = TINC_OK;
    r.status = 0;
    r.ctype[0] = 0;
}

uint8_t tinc_req_begin(uint8_t flags, uint8_t timeout_s,
                       const char *url, uint16_t url_len,
                       const char *hdrs, uint16_t hdr_len)
{
    tinc_wifi_info w;
    uint8_t e;

    if (r.state != TINC_RS_IDLE && r.state != TINC_RS_DONE && r.state != TINC_RS_ERROR)
        return TINC_ERR_BUSY;
    tinc_req_release();
    if ((uint32_t)url_len + hdr_len > RQ_MAX)
        return TINC_ERR_BAD_LEN;
    /* each user header line must be CRLF-terminated */
    if (hdr_len && (hdr_len < 2 || hdrs[hdr_len - 2] != '\r' || hdrs[hdr_len - 1] != '\n'))
        return TINC_ERR_BAD_ARG;
    memcpy(r.rq, url, url_len);
    memcpy(r.rq + url_len, hdrs, hdr_len);
    r.url_len = url_len;
    r.hdr_len = hdr_len;
    if ((e = parse_url()) != TINC_OK)
        return e;
    tinc_plat_wifi_info(&w);
    if (w.state != TINC_WIFI_CONNECTED)
        return TINC_ERR_WIFI_DOWN;

    r.flags = flags;
    r.redirects = 0;
    r.timeout_ms = (uint32_t)(timeout_s ? timeout_s : TINC_TIMEOUT_S_DEFAULT) * 1000u;
    start_connect();
    return TINC_OK;
}

/* Rewrite the url from a Location header; 0 on success, else an error. */
static uint8_t set_location(const char *v, uint16_t n)
{
    char tmp[LINE_MAX + 80]; /* location + "http://" + host:port */
    uint16_t len;

    if (iprefix(v, n, "https://"))
        return TINC_ERR_UNSUPPORTED_SCHEME;
    if (!clean(v, n))
        return TINC_ERR_HTTP_PROTO;
    if (iprefix(v, n, "http://")) {
        memcpy(tmp, v, n);
        len = n;
    } else if (n >= 2 && v[0] == '/' && v[1] == '/') {
        memcpy(tmp, "http:", 5);
        memcpy(tmp + 5, v, n);
        len = (uint16_t)(n + 5);
    } else if (n >= 1 && v[0] == '/') {
        len = (uint16_t)(7 + authority(r.rq, r.url_len));
        memcpy(tmp, r.rq, len);
        memcpy(tmp + len, v, n);
        len = (uint16_t)(len + n);
    } else {
        /* ponytail: path-relative Location ("foo/bar") isn't resolved; rare in practice */
        return TINC_ERR_HTTP_PROTO;
    }
    if ((uint32_t)len + r.hdr_len > RQ_MAX)
        return TINC_ERR_HTTP_PROTO;
    memmove(r.rq + len, r.rq + r.url_len, r.hdr_len);
    memcpy(r.rq, tmp, len);
    r.url_len = len;
    return TINC_OK;
}

static uint32_t parse_dec(const char *s)
{
    uint32_t v = 0;

    while (*s >= '0' && *s <= '9' && v < 0x0FFFFFFFu)
        v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

/* One complete header line (CR stripped). Returns 1 at end of headers. */
static int header_line(void)
{
    char *l = r.line, *v;
    uint16_t n = r.line_len, name;
    uint8_t e;

    while (n && (l[n - 1] == ' ' || l[n - 1] == '\t'))
        l[--n] = 0;
    if (r.status == 0) {
        if (!iprefix(l, n, "http/1.") || n < 12 || l[8] != ' ' ||
            l[9] < '1' || l[9] > '5' || l[10] < '0' || l[10] > '9' ||
            l[11] < '0' || l[11] > '9') {
            fail(TINC_ERR_HTTP_PROTO);
            return 0;
        }
        r.status = (uint16_t)parse_dec(l + 9);
        return 0;
    }
    if (n == 0) {
        if (r.status < 200) { /* 1xx interim response: headers restart */
            r.status = 0;
            return 0;
        }
        return 1;
    }
    for (name = 0; name < n && l[name] != ':'; name++)
        ;
    if (name == n)
        return 0; /* not a header; ignore */
    v = l + name + 1;
    while (*v == ' ' || *v == '\t')
        v++;

    if (name == 14 && iprefix(l, n, "content-length")) {
        r.clen = parse_dec(v);
        r.got_len = 1;
    } else if (name == 17 && iprefix(l, n, "transfer-encoding")) {
        for (; *v; v++)
            if (iprefix(v, (uint16_t)strlen(v), "chunked"))
                r.chunked = 1;
    } else if (name == 12 && iprefix(l, n, "content-type")) {
        strncpy(r.ctype, v, TINC_CTYPE_MAX);
        r.ctype[TINC_CTYPE_MAX] = 0;
    } else if (name == 8 && iprefix(l, n, "location") && r.status >= 300 && r.status < 400) {
        e = r.line_over ? TINC_ERR_HTTP_PROTO : set_location(v, (uint16_t)strlen(v));
        if (e != TINC_OK) {
            fail(e);
            return 0;
        }
        r.got_loc = 1;
    }
    return 0;
}

static void headers_done(void)
{
    if (r.got_loc) {
        if (r.redirects >= TINC_REDIRECT_MAX) {
            fail(TINC_ERR_TOO_MANY_REDIRECTS);
            return;
        }
        r.redirects++;
        start_connect();
        return;
    }
    r.remain = 0;
    r.cz_digits = r.cz_ext = 0;
    r.tr_len = 0;
    if (r.status == 204 || r.status == 304)
        r.body = B_DONE;
    else if (r.chunked)
        r.body = B_CHUNK_SIZE;
    else if (r.got_len)
        r.body = (r.remain = r.clen) ? B_LEN : B_DONE;
    else
        r.body = B_CLOSE;
    r.transcode = (r.flags & TINC_REQF_TRANSCODE) && tinc_tc_applies(r.ctype);
    set_state(TINC_RS_BODY);
}

static int next_byte(void)
{
    if (r.st_pos == r.st_len) {
        r.st_pos = 0;
        r.st_len = tinc_plat_tcp_read(r.stage, STAGE_LEN);
        if (!r.st_len)
            return -1;
    }
    return r.stage[r.st_pos++];
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Returns a body data byte, -1 for a framing byte, -2 on a framing error. */
static int body_byte(int c)
{
    int h;

    switch (r.body) {
    case B_LEN:
        if (!--r.remain)
            r.body = B_DONE;
        return c;
    case B_CLOSE:
        return c;
    case B_CHUNK_SIZE:
        if (c == '\n') {
            if (!r.cz_digits)
                return -2;
            r.body = r.remain ? B_CHUNK_DATA : B_TRAILER;
            r.cz_digits = r.cz_ext = 0;
        } else if (r.cz_ext || c == '\r') {
        } else if (c == ';') {
            r.cz_ext = 1;
        } else if ((h = hexval(c)) >= 0 && r.remain < 0x08000000u) {
            r.remain = r.remain * 16 + (uint32_t)h;
            r.cz_digits = 1;
        } else if (c != ' ' && c != '\t') {
            return -2;
        }
        return -1;
    case B_CHUNK_DATA:
        if (!--r.remain)
            r.body = B_CHUNK_CRLF;
        return c;
    case B_CHUNK_CRLF:
        if (c == '\n')
            r.body = B_CHUNK_SIZE;
        else if (c != '\r')
            return -2;
        return -1;
    case B_TRAILER:
        if (c == '\n') {
            if (!r.tr_len)
                r.body = B_DONE;
            r.tr_len = 0;
        } else if (c != '\r') {
            r.tr_len++;
        }
        return -1;
    }
    return -1;
}

uint16_t tinc_req_read(uint8_t *out, uint16_t cap, int *eof)
{
    uint16_t n = 0, need = r.transcode ? TINC_TC_MAX_OUT : 1;
    uint8_t t;
    int c;

    *eof = 0;
    if (r.state != TINC_RS_BODY)
        return 0;
    while (r.body != B_DONE && n + need <= cap) {
        if ((c = next_byte()) < 0) {
            t = tinc_plat_tcp_state();
            if (t == TINC_TCP_CLOSED && r.body == B_CLOSE)
                r.body = B_DONE;
            else if (n == 0 && t == TINC_TCP_CLOSED)
                fail(TINC_ERR_HTTP_PROTO); /* truncated body */
            else if (n == 0 && t != TINC_TCP_OPEN)
                fail(TINC_ERR_CONNECT);
            break; /* deliver what we have; any error shows on the next read */
        }
        if ((c = body_byte(c)) == -2) {
            fail(TINC_ERR_HTTP_PROTO);
            break;
        }
        if (c < 0)
            continue;
        if (r.transcode)
            n = (uint16_t)(n + tinc_tc_byte(&r.tc, (uint8_t)c, out + n));
        else
            out[n++] = (uint8_t)c;
    }
    if (r.state != TINC_RS_BODY)
        return 0;
    if (r.body == B_DONE && (!r.tc.need || n < cap)) {
        n = (uint16_t)(n + tinc_tc_flush(&r.tc, out + n));
        *eof = 1;
    }
    return n;
}

void tinc_req_mark_done(void)
{
    tinc_plat_tcp_close();
    set_state(TINC_RS_DONE);
}

static void send_some(void)
{
    while (r.seg_i < SEGS) {
        uint16_t left = (uint16_t)(r.seg_len[r.seg_i] - r.seg_off), w;

        if (left) {
            w = tinc_plat_tcp_write((const uint8_t *)r.seg[r.seg_i] + r.seg_off, left);
            r.seg_off = (uint16_t)(r.seg_off + w);
            if (w < left)
                return; /* send buffer full; continue next poll */
        }
        r.seg_i++;
        r.seg_off = 0;
    }
    set_state(TINC_RS_WAIT_HEADERS);
}

void tinc_req_poll(void)
{
    uint32_t now = tinc_plat_millis();
    tinc_wifi_info w;
    uint8_t t;
    int c;

    if (r.state == TINC_RS_IDLE || r.state == TINC_RS_ERROR)
        return;
    if (r.state == TINC_RS_DONE) {
        if (now - r.phase_at >= TINC_DONE_LINGER_MS)
            tinc_req_release();
        return;
    }
    tinc_plat_wifi_info(&w);
    if (w.state != TINC_WIFI_CONNECTED) {
        fail(TINC_ERR_WIFI_DOWN);
        return;
    }
    t = tinc_plat_tcp_state();
    if (r.state == TINC_RS_BODY && r.body == B_DONE)
        return; /* fully received; waiting for the CE to pull the rest */
    if (t == TINC_TCP_ERR_DNS) {
        fail(TINC_ERR_DNS);
        return;
    }
    if (t == TINC_TCP_ERR_CONNECT || t == TINC_TCP_IDLE ||
        (t == TINC_TCP_CLOSED && r.state <= TINC_RS_SENDING)) {
        fail(TINC_ERR_CONNECT);
        return;
    }

    switch (r.state) {
    case TINC_RS_CONNECTING:
        if (t == TINC_TCP_OPEN)
            set_state(TINC_RS_SENDING);
        break;
    case TINC_RS_SENDING:
        send_some();
        break;
    case TINC_RS_WAIT_HEADERS:
        /* ponytail: headers parse to completion in one poll if already
         * buffered; bounded by what lwIP holds, so still short. */
        while (r.state == TINC_RS_WAIT_HEADERS && (c = next_byte()) >= 0) {
            r.phase_at = now;
            if (c == '\n') {
                if (r.line_len && r.line[r.line_len - 1] == '\r')
                    r.line_len--;
                r.line[r.line_len] = 0;
                if (header_line() && r.state == TINC_RS_WAIT_HEADERS) {
                    headers_done();
                    return;
                }
                r.line_len = 0;
                r.line_over = 0;
            } else if (r.line_len < LINE_MAX - 1) {
                r.line[r.line_len++] = (char)c;
            } else {
                r.line_over = 1;
            }
        }
        if (r.state == TINC_RS_WAIT_HEADERS && t == TINC_TCP_CLOSED && r.st_pos == r.st_len) {
            fail(TINC_ERR_HTTP_PROTO);
            return;
        }
        break;
    case TINC_RS_BODY:
        /* The gap timer only runs while we're actually waiting on the server. */
        if (r.body == B_DONE || r.st_pos < r.st_len || t == TINC_TCP_CLOSED) {
            r.phase_at = now;
        } else {
            r.st_pos = 0;
            r.st_len = tinc_plat_tcp_read(r.stage, STAGE_LEN);
            if (r.st_len)
                r.phase_at = now;
        }
        break;
    }
    if (r.state >= TINC_RS_CONNECTING && r.state <= TINC_RS_BODY &&
        now - r.phase_at > r.timeout_ms)
        fail(TINC_ERR_TIMEOUT);
}

uint8_t tinc_req_state(void) { return r.state; }
uint8_t tinc_req_err(void) { return r.err; }
uint16_t tinc_req_http_status(void) { return r.status; }
const char *tinc_req_ctype(void) { return r.ctype; }

uint32_t tinc_req_content_len(void)
{
    if (r.state < TINC_RS_BODY || r.transcode || r.chunked || !r.got_len)
        return TINC_LEN_UNKNOWN;
    return r.clen;
}
