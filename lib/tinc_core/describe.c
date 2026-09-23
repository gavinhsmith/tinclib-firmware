/*
 * One-line, human-readable description of a TINCLIB frame, for traces.
 * Never shows request header text, Wi-Fi passwords or URL query strings
 * (they can carry API keys); lengths are shown instead.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "tinc_core.h"

#define PREVIEW 48 /* body bytes shown per BODY_READ reply */

struct out {
    char *p;
    size_t cap, len;
};

static void put(struct out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->len + 1 >= o->cap)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        o->len = o->len + (size_t)n < o->cap ? o->len + (size_t)n : o->cap - 1;
}

/* Printable text, escaped, in quotes. */
static void put_text(struct out *o, const uint8_t *p, size_t n)
{
    size_t i;

    put(o, "\"");
    for (i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == '"' || c == '\\')
            put(o, "\\%c", c);
        else if (c == '\n')
            put(o, "\\n");
        else if (c == '\r')
            put(o, "\\r");
        else if (c >= 0x20 && c < 0x7F)
            put(o, "%c", c);
        else
            put(o, "\\x%02X", c);
    }
    put(o, "\"");
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case TINC_T_HELLO:       return "HELLO";
    case TINC_T_STATUS:      return "STATUS";
    case TINC_T_REQ_BEGIN:   return "REQ_BEGIN";
    case TINC_T_REQ_STATUS:  return "REQ_STATUS";
    case TINC_T_REQ_ABORT:   return "REQ_ABORT";
    case TINC_T_BODY_READ:   return "BODY_READ";
    case TINC_T_WIFI_GET:    return "WIFI_GET";
    case TINC_T_WIFI_SET:    return "WIFI_SET";
    case TINC_T_WIFI_FORGET: return "WIFI_FORGET";
    }
    return NULL;
}

static const char *err_name(uint8_t e)
{
    switch (e) {
    case TINC_OK:                     return "OK";
    case TINC_ERR_UNSUPPORTED:        return "UNSUPPORTED";
    case TINC_ERR_NO_HELLO:           return "NO_HELLO";
    case TINC_ERR_VERSION:            return "VERSION";
    case TINC_ERR_BAD_LEN:            return "BAD_LEN";
    case TINC_ERR_BUSY:               return "BUSY";
    case TINC_ERR_BAD_STATE:          return "BAD_STATE";
    case TINC_ERR_BAD_OFFSET:         return "BAD_OFFSET";
    case TINC_ERR_BAD_ARG:            return "BAD_ARG";
    case TINC_ERR_UNSUPPORTED_SCHEME: return "UNSUPPORTED_SCHEME";
    case TINC_ERR_LOCKED:             return "LOCKED";
    case TINC_ERR_WIFI_DOWN:          return "WIFI_DOWN";
    case TINC_ERR_DNS:                return "DNS";
    case TINC_ERR_CONNECT:            return "CONNECT";
    case TINC_ERR_TIMEOUT:            return "TIMEOUT";
    case TINC_ERR_HTTP_PROTO:         return "HTTP_PROTO";
    case TINC_ERR_TOO_MANY_REDIRECTS: return "TOO_MANY_REDIRECTS";
    case TINC_ERR_NO_MEM:             return "NO_MEM";
    }
    return "?";
}

static const char *const req_states[] = {
    "IDLE", "CONNECTING", "TLS", "SENDING", "WAIT_HEADERS", "BODY", "DONE", "ERROR"
};
static const char *const wifi_states[] = {"NO_CREDS", "CONNECTING", "CONNECTED", "FAILED"};

#define NAME(tab, i) ((i) < sizeof tab / sizeof tab[0] ? tab[i] : "?")

static void hello(struct out *o, int resp, const uint8_t *p, uint16_t n)
{
    if (n < TINC_HELLO_REQ_LEN)
        return;
    put(o, "%s v%u.%u max_payload=%u", resp ? " ok" : "", p[TINC_HELLO_MAJOR], p[TINC_HELLO_MINOR],
        tinc_get_u16(p + TINC_HELLO_MAX_PAYLOAD));
    if (resp && n >= TINC_HELLO_RESP_LEN)
        put(o, " heap=%lu wifi_slots=%u", (unsigned long)tinc_get_u32(p + TINC_HELLO_FREE_HEAP),
            p[TINC_HELLO_WIFI_SLOTS]);
}

static void status(struct out *o, const uint8_t *p, uint16_t n)
{
    if (n < TINC_STATUS_REQ_STATE + 1)
        return;
    put(o, " wifi=%s", NAME(wifi_states, p[TINC_STATUS_WIFI_STATE]));
    if (p[TINC_STATUS_SLOT] == TINC_SLOT_NONE)
        put(o, " slot=none");
    else
        put(o, " slot=%u", p[TINC_STATUS_SLOT]);
    put(o, " rssi=%d ip=%u.%u.%u.%u heap=%lu req=%s", (int)(int8_t)p[TINC_STATUS_RSSI],
        p[TINC_STATUS_IP], p[TINC_STATUS_IP + 1], p[TINC_STATUS_IP + 2], p[TINC_STATUS_IP + 3],
        (unsigned long)tinc_get_u32(p + TINC_STATUS_FREE_HEAP), NAME(req_states, p[TINC_STATUS_REQ_STATE]));
    if (n > TINC_STATUS_FLAGS && (p[TINC_STATUS_FLAGS] & TINC_STATUSF_WIFI_LOCKED))
        put(o, " wifi-locked");
}

static void req_begin(struct out *o, const uint8_t *p, uint16_t n)
{
    uint16_t ul, hl, shown;
    uint32_t clen;

    if (n < TINC_BEGIN_URL)
        return;
    ul = tinc_get_u16(p + TINC_BEGIN_URL_LEN);
    hl = tinc_get_u16(p + TINC_BEGIN_HDR_LEN);
    if ((uint32_t)TINC_BEGIN_URL + ul > n)
        ul = (uint16_t)(n - TINC_BEGIN_URL);
    /* show the URL up to its query string; a query often carries a key */
    for (shown = 0; shown < ul && p[TINC_BEGIN_URL + shown] != '?'; shown++)
        ;
    put(o, " %s ", p[TINC_BEGIN_METHOD] == TINC_METHOD_GET ? "GET" : "method?");
    put(o, "%.*s%s", (int)shown, (const char *)p + TINC_BEGIN_URL, shown < ul ? "?..." : "");
    if (p[TINC_BEGIN_FLAGS] & TINC_REQF_TRANSCODE)
        put(o, " transcode");
    if (p[TINC_BEGIN_TIMEOUT_S])
        put(o, " timeout=%us", p[TINC_BEGIN_TIMEOUT_S]);
    clen = tinc_get_u32(p + TINC_BEGIN_CONTENT_LEN);
    if (clen)
        put(o, " content_len=%lu", (unsigned long)clen);
    if (hl)
        put(o, " (headers: %u bytes, not shown)", hl);
}

static void req_status(struct out *o, const uint8_t *p, uint16_t n)
{
    uint32_t clen;
    uint8_t cl;

    if (n < TINC_RSTAT_CTYPE)
        return;
    put(o, " %s", NAME(req_states, p[TINC_RSTAT_STATE]));
    if (p[TINC_RSTAT_ERR])
        put(o, " err=%s", err_name(p[TINC_RSTAT_ERR]));
    if (tinc_get_u16(p + TINC_RSTAT_HTTP_STATUS))
        put(o, " http=%u", tinc_get_u16(p + TINC_RSTAT_HTTP_STATUS));
    clen = tinc_get_u32(p + TINC_RSTAT_CONTENT_LEN);
    if (clen == TINC_LEN_UNKNOWN)
        put(o, " len=unknown");
    else
        put(o, " len=%lu", (unsigned long)clen);
    cl = p[TINC_RSTAT_CTYPE_LEN];
    if (cl && TINC_RSTAT_CTYPE + cl <= n)
        put(o, " type=%.*s", (int)cl, (const char *)p + TINC_RSTAT_CTYPE);
}

static void body(struct out *o, int resp, const uint8_t *p, uint16_t n)
{
    uint16_t dl;

    if (!resp) {
        if (n >= TINC_READ_REQ_LEN)
            put(o, " @%lu max=%u wait=%ums", (unsigned long)tinc_get_u32(p + TINC_READ_OFFSET),
                tinc_get_u16(p + TINC_READ_MAX_LEN), p[TINC_READ_WAIT_MS]);
        return;
    }
    if (n < TINC_READ_DATA)
        return;
    dl = (uint16_t)(n - TINC_READ_DATA);
    put(o, " @%lu %u bytes%s", (unsigned long)tinc_get_u32(p + TINC_READ_OFFSET), dl,
        (p[TINC_READ_FLAGS] & TINC_READF_EOF) ? " EOF" : "");
    if (dl) {
        put(o, ": ");
        put_text(o, p + TINC_READ_DATA, dl > PREVIEW ? PREVIEW : dl);
        if (dl > PREVIEW)
            put(o, "...");
    }
}

static void wifi_get(struct out *o, int resp, const uint8_t *p, uint16_t n)
{
    uint8_t l;

    if (!resp) {
        if (n)
            put(o, " slot %u", p[TINC_WGET_SLOT]);
        return;
    }
    if (n < 1 || TINC_WGET_SSID + p[TINC_WGET_SSID_LEN] > n)
        return;
    l = p[TINC_WGET_SSID_LEN];
    put(o, " ssid=");
    put_text(o, p + TINC_WGET_SSID, l);
    if (TINC_WGET_SSID + l < n && (p[TINC_WGET_SSID + l] & TINC_WF_HIDDEN))
        put(o, " hidden");
}

static void wifi_set(struct out *o, const uint8_t *p, uint16_t n)
{
    uint8_t sl, pw;

    if (n < TINC_WSET_SSID + 1u || n < TINC_WSET_SSID + 1u + p[TINC_WSET_SSID_LEN])
        return;
    sl = p[TINC_WSET_SSID_LEN];
    put(o, " slot %u ssid=", p[TINC_WSET_SLOT]);
    put_text(o, p + TINC_WSET_SSID, sl);
    pw = p[TINC_WSET_SSID + sl];
    put(o, pw ? " (password not shown)" : " (open)");
    if (n > TINC_WSET_SSID + 1u + sl + pw && (p[TINC_WSET_SSID + 1 + sl + pw] & TINC_WF_HIDDEN))
        put(o, " hidden");
}

void tinc_describe(const uint8_t *f, uint16_t len, char *buf, size_t cap)
{
    struct out o;
    uint8_t flags, type, seq;
    uint16_t n;
    const uint8_t *p;
    const char *name;
    int resp;

    o.p = buf;
    o.cap = cap;
    o.len = 0;
    if (!cap)
        return;
    buf[0] = 0;
    if (len < TINC_OVERHEAD) {
        put(&o, "(%u bytes, not a frame)", len);
        return;
    }
    flags = f[1];
    type = f[2];
    seq = f[3];
    n = tinc_get_u16(f + 4);
    if ((uint32_t)TINC_OVERHEAD + n > len)
        n = (uint16_t)(len - TINC_OVERHEAD);
    p = f + TINC_HDR_LEN;
    resp = flags & TINC_FLAG_RESP;
    name = type_name(type);

    put(&o, "#%u ", seq);
    if (name)
        put(&o, "%s", name);
    else
        put(&o, "type 0x%02X", type);

    if (flags & TINC_FLAG_ERR) {
        put(&o, " -> error %s", n ? err_name(p[0]) : "?");
        if (n >= 3 && p[0] == TINC_ERR_VERSION)
            put(&o, " (ESP is v%u.%u)", p[1], p[2]);
        return;
    }
    if (!name) {
        put(&o, " (%u bytes)", n);
        return;
    }
    switch (type) {
    case TINC_T_HELLO:
        hello(&o, resp, p, n);
        return;
    case TINC_T_STATUS:
        if (resp)
            status(&o, p, n);
        else
            put(&o, "?");
        return;
    case TINC_T_REQ_BEGIN:
        if (resp)
            put(&o, " ok");
        else
            req_begin(&o, p, n);
        return;
    case TINC_T_REQ_STATUS:
        if (resp)
            req_status(&o, p, n);
        else
            put(&o, "?");
        return;
    case TINC_T_BODY_READ:
        body(&o, resp, p, n);
        return;
    case TINC_T_WIFI_GET:
        wifi_get(&o, resp, p, n);
        return;
    case TINC_T_WIFI_SET:
        if (resp)
            put(&o, " ok");
        else
            wifi_set(&o, p, n);
        return;
    case TINC_T_WIFI_FORGET:
        if (resp)
            put(&o, " ok");
        else if (n)
            put(&o, " slot %u", p[TINC_WFORGET_SLOT]);
        return;
    default: /* REQ_ABORT */
        if (resp)
            put(&o, " ok");
        return;
    }
}
