/*
 * HTTP request state machine. One step per tinc_req_poll(); the body is
 * decoded (Content-Length / chunked / close-delimited) only as fast as the
 * CE pulls it with BODY_READ, so unread data backs up into TCP flow control.
 * A request body goes the other way: BODY_WRITE hands it straight to TCP,
 * taking only what fits, so nothing is buffered here.
 */
#include <stdio.h>
#include <string.h>
#include "tinc_core.h"

#define RQ_MAX    TINC_PAYLOAD_LIMIT /* url + user headers, as sent in REQ_BEGIN */
#define LINE_MAX  256                /* response header line */
#define HOST_MAX  64
#define STAGE_LEN 128
/* Response headers kept for HDR_GET; Location has its own buffer so it is
 * never lost to an overflow. */
#define HSTORE_MAX 512
/* ponytail: rough floor for lwIP + our buffers on plain TCP. TLS needs far
 * more; the platform checks that in tinc_plat_tls_start. */
#define MIN_HEAP  6000u

enum { B_LEN, B_CHUNK_SIZE, B_CHUNK_DATA, B_CHUNK_CRLF, B_TRAILER, B_CLOSE, B_DONE };
enum { SEGS = 9 };

static struct {
    uint8_t state, err, err_detail, flags, redirects, transcode;
    uint8_t method;
    uint8_t tls, tls_on; /* url is https; handshake started */
    uint32_t timeout_ms, phase_at;

    char rq[RQ_MAX]; /* url, then user headers */
    uint16_t url_len, hdr_len;
    char host[HOST_MAX];
    uint16_t port;

    const char *seg[SEGS];
    uint16_t seg_len[SEGS];
    uint8_t seg_i;
    uint16_t seg_off;
    char clen_hdr[32]; /* "Content-Length: N\r\n" */

    uint32_t up_len, up_sent; /* request body: content_len, bytes taken */
    uint8_t responded;        /* the server answered before the upload finished */

    char line[LINE_MAX];
    uint16_t line_len;
    uint8_t line_over, got_len, chunked, got_loc;
    uint16_t status;
    uint32_t clen;
    char ctype[TINC_CTYPE_MAX + 1];

    /* final response's headers: [name_len u8][name][value_len u16][value]... */
    uint8_t hstore[HSTORE_MAX];
    uint16_t hstore_len;
    uint8_t htrunc;
    char loc[LINE_MAX];
    uint16_t loc_len;
    uint8_t has_loc;

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

static void fail_detail(uint8_t err, uint8_t detail)
{
    tinc_plat_tcp_close();
    r.err = err;
    r.err_detail = detail;
    set_state(TINC_RS_ERROR);
    log_num("req err", err);
}

static void fail(uint8_t err)
{
    fail_detail(err, 0);
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

/* Length of an "http://" or "https://" prefix, else 0. */
static uint16_t scheme_len(const char *u, uint16_t n)
{
    return iprefix(u, n, "https://") ? 8 : iprefix(u, n, "http://") ? 7 : 0;
}

/* Starts with some other "scheme://" (RFC 3986 scheme characters). */
static int other_scheme(const char *u, uint16_t n)
{
    uint16_t i = 0;
    int c;

    for (; i < n; i++) {
        c = lower((unsigned char)u[i]);
        if (!(c >= 'a' && c <= 'z') &&
            !(i && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')))
            break;
    }
    return i && iprefix(u + i, (uint16_t)(n - i), "://");
}

static const char *method_name(uint8_t m)
{
    switch (m) {
    case TINC_METHOD_POST:   return "POST";
    case TINC_METHOD_PUT:    return "PUT";
    case TINC_METHOD_DELETE: return "DELETE";
    case TINC_METHOD_PATCH:  return "PATCH";
    case TINC_METHOD_HEAD:   return "HEAD";
    }
    return "GET";
}

/* An app header the ESP must generate (or refuse) itself. Every line is
 * checked, split at a bare LF too, so none can hide behind another. */
static int reserved_header(const char *h, uint16_t n)
{
    static const char *const names[] = {"host", "content-length", "transfer-encoding", "expect"};
    uint16_t i = 0, name, end;
    size_t k;

    while (i < n) {
        for (name = i; name < n && h[name] != ':' && h[name] != '\n'; name++)
            ;
        for (end = name; end > i && (h[end - 1] == ' ' || h[end - 1] == '\t'); end--)
            ; /* "Host : x" is still Host to some servers */
        for (k = 0; k < sizeof names / sizeof names[0]; k++)
            if ((size_t)(end - i) == strlen(names[k]) && iprefix(h + i, (uint16_t)(end - i), names[k]))
                return 1;
        while (i < n && h[i] != '\n')
            i++;
        i++;
    }
    return 0;
}

/* Authority (host[:port]) of an http(s):// url of length n. */
static uint16_t authority(const char *u, uint16_t n)
{
    uint16_t s = scheme_len(u, n), i = s;

    while (i < n && u[i] != '/' && u[i] != '?' && u[i] != '#')
        i++;
    return (uint16_t)(i - s);
}

static uint8_t parse_url(void)
{
    const char *u = r.rq, *a, *path;
    uint16_t n = r.url_len, s = scheme_len(u, n), alen, hlen, plen, i;
    uint32_t port = 0;

    if (!s)
        return other_scheme(u, n) ? TINC_ERR_UNSUPPORTED_SCHEME : TINC_ERR_BAD_ARG;
    if (!clean(u, n))
        return TINC_ERR_BAD_ARG;

    a = u + s;
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
        port = s == 8 ? 443 : 80;
    }
    memcpy(r.host, a, hlen);
    r.host[hlen] = 0;
    r.port = (uint16_t)port;
    r.tls = s == 8;

    path = a + alen;
    for (plen = 0; path + plen < u + n && path[plen] != '#'; plen++)
        ;

    r.seg[0] = method_name(r.method);
    r.seg[1] = (plen == 0 || path[0] == '?') ? " /" : " ";
    r.seg[2] = path;
    r.seg_len[2] = plen;
    r.seg[3] = " HTTP/1.1\r\nHost: ";
    r.seg[4] = a;
    r.seg_len[4] = alen;
    r.seg[5] = "\r\nAccept-Encoding: identity\r\nConnection: close\r\n"
               "User-Agent: tinclib-firmware\r\n";
    /* a body, or a method that usually has one (servers may want the 0) */
    r.clen_hdr[0] = 0;
    if (r.up_len || r.method == TINC_METHOD_POST || r.method == TINC_METHOD_PUT ||
        r.method == TINC_METHOD_PATCH)
        snprintf(r.clen_hdr, sizeof r.clen_hdr, "Content-Length: %lu\r\n", (unsigned long)r.up_len);
    r.seg[6] = r.clen_hdr;
    r.seg[7] = r.rq + r.url_len;
    r.seg_len[7] = r.hdr_len;
    r.seg[8] = "\r\n";
    for (i = 0; i < SEGS; i++)
        if (i != 2 && i != 4 && i != 7)
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
    r.up_sent = 0;
    r.responded = 0;
    r.line_len = 0;
    r.line_over = r.got_len = r.chunked = r.got_loc = 0;
    r.status = 0;
    r.clen = 0;
    r.ctype[0] = 0;
    r.hstore_len = 0;
    r.htrunc = r.has_loc = 0;
    r.st_pos = r.st_len = 0;
    r.tls_on = 0;
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
    r.err_detail = 0;
    r.status = 0;
    r.ctype[0] = 0;
    r.hstore_len = 0;
    r.htrunc = r.has_loc = 0;
    r.responded = 0;
    r.up_len = r.up_sent = 0;
}

uint8_t tinc_req_begin(uint8_t method, uint8_t flags, uint8_t timeout_s, uint32_t content_len,
                       const char *url, uint16_t url_len,
                       const char *hdrs, uint16_t hdr_len)
{
    tinc_wifi_info w;
    uint8_t e;

    if (r.state != TINC_RS_IDLE && r.state != TINC_RS_DONE && r.state != TINC_RS_ERROR)
        return TINC_ERR_BUSY;
    tinc_req_release();
    if (method < TINC_METHOD_GET || method > TINC_METHOD_HEAD || content_len == TINC_LEN_UNKNOWN ||
        (content_len && (method == TINC_METHOD_GET || method == TINC_METHOD_HEAD)))
        return TINC_ERR_BAD_ARG;
    if ((uint32_t)url_len + hdr_len > RQ_MAX)
        return TINC_ERR_BAD_LEN;
    /* each user header line must be CRLF-terminated */
    if (hdr_len && (hdr_len < 2 || hdrs[hdr_len - 2] != '\r' || hdrs[hdr_len - 1] != '\n'))
        return TINC_ERR_BAD_ARG;
    if (reserved_header(hdrs, hdr_len))
        return TINC_ERR_BAD_ARG;
    memcpy(r.rq, url, url_len);
    memcpy(r.rq + url_len, hdrs, hdr_len);
    r.url_len = url_len;
    r.hdr_len = hdr_len;
    r.method = method;
    r.up_len = content_len;
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

/* The host of url u (length n) is r.host, ignoring case. */
static int same_host(const char *u, uint16_t n)
{
    uint16_t s = scheme_len(u, n), a = authority(u, n), i;

    for (i = 0; i < a && u[s + i] != ':'; i++)
        if (!r.host[i] || lower((unsigned char)u[s + i]) != lower((unsigned char)r.host[i]))
            return 0;
    return !r.host[i];
}

/* Rewrite the url from a Location header; 0 on success, else an error. */
static uint8_t set_location(const char *v, uint16_t n)
{
    char tmp[LINE_MAX + 80]; /* location + "https://" + host:port */
    uint16_t len, s = scheme_len(v, n), cur = scheme_len(r.rq, r.url_len);

    if (!clean(v, n))
        return TINC_ERR_HTTP_PROTO;
    if (s) {
        /* https -> http would resend the app's headers in clear */
        if (cur == 8 && s == 7)
            return TINC_ERR_REDIRECT_DOWNGRADE;
        memcpy(tmp, v, n);
        len = n;
    } else if (n >= 2 && v[0] == '/' && v[1] == '/') {
        len = (uint16_t)(cur - 2); /* "http:" or "https:" */
        memcpy(tmp, r.rq, len);
        memcpy(tmp + len, v, n);
        len = (uint16_t)(len + n);
    } else if (n >= 1 && v[0] == '/') {
        len = (uint16_t)(cur + authority(r.rq, r.url_len));
        memcpy(tmp, r.rq, len);
        memcpy(tmp + len, v, n);
        len = (uint16_t)(len + n);
    } else if (other_scheme(v, n)) {
        return TINC_ERR_UNSUPPORTED_SCHEME;
    } else {
        /* ponytail: path-relative Location ("foo/bar") isn't resolved; rare in practice */
        return TINC_ERR_HTTP_PROTO;
    }
    /* The app never named this host, so none of its headers go there:
     * credentials can hide under any header name. */
    if (!same_host(tmp, len))
        r.hdr_len = 0;
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
    uint16_t n = r.line_len, name, vlen;
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
        /* HDR_GET reads this response's headers, not a 1xx's before it */
        r.hstore_len = 0;
        r.htrunc = r.has_loc = 0;
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
    vlen = (uint16_t)strlen(v);

    if (name == 8 && iprefix(l, n, "location")) {
        /* ponytail: a Location line longer than LINE_MAX fails the request
         * rather than be kept cut short; raise LINE_MAX if one shows up */
        if (r.line_over) {
            fail(TINC_ERR_HTTP_PROTO);
            return 0;
        }
        memcpy(r.loc, v, vlen);
        r.loc_len = vlen;
        r.has_loc = 1;
    } else if (r.line_over || name > 255 ||
               (uint32_t)r.hstore_len + 3 + name + vlen > HSTORE_MAX) {
        r.htrunc = 1; /* dropped whole rather than kept cut short */
    } else {
        r.hstore[r.hstore_len++] = (uint8_t)name;
        memcpy(r.hstore + r.hstore_len, l, name);
        r.hstore_len = (uint16_t)(r.hstore_len + name);
        tinc_put_u16(r.hstore + r.hstore_len, vlen);
        memcpy(r.hstore + r.hstore_len + 2, v, vlen);
        r.hstore_len = (uint16_t)(r.hstore_len + 2 + vlen);
    }

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
    } else if (name == 8 && iprefix(l, n, "location") && r.status >= 300 && r.status < 400 &&
               (r.method == TINC_METHOD_GET || r.method == TINC_METHOD_HEAD)) {
        /* Other methods get the 3xx as the response (HDR_GET reads
         * Location): following it would mean resending the body. */
        e = set_location(v, vlen);
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
    if (r.status == 204 || r.status == 304 || r.method == TINC_METHOD_HEAD)
        r.body = B_DONE; /* HEAD: Content-Length describes the GET's body */
    else if (r.chunked)
        r.body = B_CHUNK_SIZE;
    else if (r.got_len)
        r.body = (r.remain = r.clen) ? B_LEN : B_DONE;
    else
        r.body = B_CLOSE;
    r.transcode = (r.flags & TINC_REQF_TRANSCODE) && r.method != TINC_METHOD_HEAD &&
                  tinc_tc_applies(r.ctype);
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

/* Request line and headers; 1 once they're all out. */
static int send_headers(void)
{
    while (r.seg_i < SEGS) {
        uint16_t left = (uint16_t)(r.seg_len[r.seg_i] - r.seg_off), w;

        if (left) {
            w = tinc_plat_tcp_write((const uint8_t *)r.seg[r.seg_i] + r.seg_off, left);
            r.seg_off = (uint16_t)(r.seg_off + w);
            if (w < left)
                return 0; /* send buffer full; continue next poll */
        }
        r.seg_i++;
        r.seg_off = 0;
    }
    return 1;
}

static void send_some(void)
{
    if (send_headers() && r.up_sent == r.up_len)
        set_state(TINC_RS_WAIT_HEADERS);
}

/* Mid-upload: has the server already answered (401, 413, ...)? Then stop
 * sending and read the response. */
static int early_response(void)
{
    if (r.seg_i < SEGS || r.up_sent == r.up_len)
        return 0;
    if (r.st_pos == r.st_len) {
        r.st_pos = 0;
        r.st_len = tinc_plat_tcp_read(r.stage, STAGE_LEN);
    }
    if (r.st_pos == r.st_len)
        return 0;
    r.responded = 1;
    set_state(TINC_RS_WAIT_HEADERS);
    return 1;
}

uint16_t tinc_req_write(const uint8_t *p, uint16_t n)
{
    uint16_t w;

    if (r.state != TINC_RS_SENDING || !send_headers())
        return 0;
    if (n > r.up_len - r.up_sent)
        n = (uint16_t)(r.up_len - r.up_sent);
    w = tinc_plat_tcp_write(p, n);
    r.up_sent += w;
    if (w)
        r.phase_at = tinc_plat_millis(); /* SENDING times out on no progress */
    if (r.up_sent == r.up_len)
        set_state(TINC_RS_WAIT_HEADERS);
    return w;
}

void tinc_req_poll(void)
{
    uint32_t now = tinc_plat_millis();
    tinc_wifi_info w;
    uint8_t t, e, d;
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
    if (t == TINC_TCP_ERR_TLS) {
        e = tinc_plat_tls_err(&d);
        fail_detail(e, d);
        return;
    }
    if (r.state == TINC_RS_TLS && r.tls_on && t != TINC_TCP_TLS && t != TINC_TCP_OPEN) {
        fail_detail(TINC_ERR_TLS, TINC_TLSR_OTHER); /* server hung up mid-handshake */
        return;
    }
    /* a server that answers early may close at once; its reply still counts */
    if (r.state == TINC_RS_SENDING && (t == TINC_TCP_OPEN || t == TINC_TCP_CLOSED) && early_response())
        return;
    if (t == TINC_TCP_ERR_CONNECT || t == TINC_TCP_IDLE ||
        (t == TINC_TCP_CLOSED && r.state <= TINC_RS_SENDING)) {
        fail(TINC_ERR_CONNECT);
        return;
    }

    switch (r.state) {
    case TINC_RS_CONNECTING:
        if (t == TINC_TCP_OPEN)
            set_state(r.tls ? TINC_RS_TLS : TINC_RS_SENDING);
        break;
    case TINC_RS_TLS:
        if (r.tls_on) {
            if (t == TINC_TCP_OPEN)
                set_state(TINC_RS_SENDING);
        } else if (tinc_plat_time()) { /* certificates can't be checked without a clock */
            if ((e = tinc_plat_tls_start(r.host)) != TINC_OK) {
                fail(e);
                return;
            }
            r.tls_on = 1;
        }
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
    /* fresh clock: a state change above may have set phase_at after `now` */
    if (r.state >= TINC_RS_CONNECTING && r.state <= TINC_RS_BODY &&
        tinc_plat_millis() - r.phase_at > r.timeout_ms)
        fail(r.state == TINC_RS_TLS && !r.tls_on ? TINC_ERR_TIME : TINC_ERR_TIMEOUT);
}

uint8_t tinc_req_state(void) { return r.state; }
uint8_t tinc_req_err(void) { return r.err; }
uint8_t tinc_req_err_detail(void) { return r.err_detail; }
uint16_t tinc_req_http_status(void) { return r.status; }
const char *tinc_req_ctype(void) { return r.ctype; }
uint32_t tinc_req_body_len(void) { return r.up_len; }
uint32_t tinc_req_body_sent(void) { return r.up_sent; }
int tinc_req_responded(void) { return r.responded; }
int tinc_req_hdr_trunc(void) { return r.htrunc; }

int tinc_req_header(const char *name, uint8_t name_len, uint8_t index,
                    const char **val, uint16_t *val_len)
{
    uint16_t i = 0, nl, vl;

    if (name_len == 8 && iprefix(name, 8, "location")) {
        if (!r.has_loc || index)
            return 0;
        *val = r.loc;
        *val_len = r.loc_len;
        return 1;
    }
    while (i < r.hstore_len) {
        nl = r.hstore[i];
        vl = tinc_get_u16(r.hstore + i + 1 + nl);
        if (nl == name_len) {
            uint16_t k;
            for (k = 0; k < nl && lower((unsigned char)r.hstore[i + 1 + k]) == lower((unsigned char)name[k]); k++)
                ;
            if (k == nl && index-- == 0) {
                *val = (const char *)r.hstore + i + 3 + nl;
                *val_len = vl;
                return 1;
            }
        }
        i = (uint16_t)(i + 3 + nl + vl);
    }
    return 0;
}

uint32_t tinc_req_content_len(void)
{
    if (r.state < TINC_RS_BODY || r.transcode || r.chunked || !r.got_len)
        return TINC_LEN_UNKNOWN;
    return r.clen;
}
