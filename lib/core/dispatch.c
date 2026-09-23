/*
 * Command dispatcher: HELLO gate, SEQ reply cache, per-type handlers.
 * Replies are built in place in the cache buffer, so a repeated SEQ is
 * answered from it verbatim without re-running the command.
 */
#include <string.h>
#include "core.h"
#include "tinc_frame.h"

#define CHUNK_MAX (TINC_PAYLOAD_LIMIT - TINC_READ_DATA)
#define PL (reply + TINC_HDR_LEN)

static tinc_slots slots;
static uint8_t hello_ok;
static uint16_t peer_max;
static uint32_t last_rx;

static uint8_t reply[TINC_FRAME_BUF(TINC_PAYLOAD_LIMIT)];
static uint8_t cache_ok, cache_type, cache_seq;

/* BODY_READ: the last non-empty chunk, for offset-based retry */
static uint8_t chunk[CHUNK_MAX];
static uint32_t last_off;
static uint16_t last_len;
static uint8_t have_chunk, chunk_eof;
static uint8_t waiting;
static uint32_t wait_until;

static void reset_body(void)
{
    last_off = 0;
    last_len = 0;
    have_chunk = 0;
    waiting = 0;
}

void tinc_core_init(void)
{
    memset(&slots, 0, sizeof slots);
    hello_ok = 0;
    peer_max = TINC_PAYLOAD_MIN;
    cache_ok = 0;
    tinc_req_release();
    reset_body();
    last_rx = tinc_plat_millis();
}

tinc_slots *tinc_core_slots(void)
{
    return &slots;
}

static uint16_t ok(uint8_t type, uint8_t seq, uint16_t len)
{
    return tinc_frame_encode(reply, TINC_FLAG_RESP, type, seq, PL, len);
}

static uint16_t err(uint8_t type, uint8_t seq, uint8_t e)
{
    PL[0] = e;
    return tinc_frame_encode(reply, TINC_FLAG_RESP | TINC_FLAG_ERR, type, seq, PL, 1);
}

static uint16_t hello(uint8_t seq, const uint8_t *pl, uint16_t len)
{
    uint16_t mp;

    cache_ok = 0; /* the reply buffer is about to be overwritten */
    if (len < TINC_HELLO_REQ_LEN)
        return err(TINC_T_HELLO, seq, TINC_ERR_BAD_LEN);
    /* pre-1.0 needs an exact MAJOR.MINOR match */
    if (pl[TINC_HELLO_MAJOR] != TINC_PROTO_MAJOR ||
        (TINC_PROTO_MAJOR == 0 && pl[TINC_HELLO_MINOR] != TINC_PROTO_MINOR)) {
        PL[0] = TINC_ERR_VERSION;
        PL[1] = TINC_PROTO_MAJOR;
        PL[2] = TINC_PROTO_MINOR;
        return tinc_frame_encode(reply, TINC_FLAG_RESP | TINC_FLAG_ERR, TINC_T_HELLO, seq, PL, 3);
    }
    mp = tinc_get_u16(pl + TINC_HELLO_MAX_PAYLOAD);
    peer_max = mp < TINC_PAYLOAD_MIN ? TINC_PAYLOAD_MIN : mp > TINC_PAYLOAD_LIMIT ? TINC_PAYLOAD_LIMIT : mp;
    hello_ok = 1;
    /* a fresh HELLO means the CE lost its state; drop whatever it left running */
    tinc_req_release();
    reset_body();

    PL[TINC_HELLO_MAJOR] = TINC_PROTO_MAJOR;
    PL[TINC_HELLO_MINOR] = TINC_PROTO_MINOR;
    tinc_put_u16(PL + TINC_HELLO_CAPS, 0);
    tinc_put_u16(PL + TINC_HELLO_MAX_PAYLOAD, TINC_PAYLOAD_LIMIT);
    tinc_put_u32(PL + TINC_HELLO_FREE_HEAP, tinc_plat_free_heap());
    return ok(TINC_T_HELLO, seq, TINC_HELLO_RESP_LEN);
}

static uint16_t status(uint8_t seq)
{
    tinc_wifi_info w;

    tinc_plat_wifi_info(&w);
    PL[TINC_STATUS_WIFI_STATE] = w.state;
    PL[TINC_STATUS_SLOT] = w.slot;
    PL[TINC_STATUS_RSSI] = (uint8_t)w.rssi;
    memcpy(PL + TINC_STATUS_IP, w.ip, 4);
    tinc_put_u32(PL + TINC_STATUS_FREE_HEAP, tinc_plat_free_heap());
    PL[TINC_STATUS_REQ_STATE] = tinc_req_state();
    return ok(TINC_T_STATUS, seq, TINC_STATUS_RESP_LEN);
}

static uint16_t req_begin(uint8_t seq, const uint8_t *pl, uint16_t len)
{
    uint16_t ul, hl;
    uint8_t e;

    if (len < TINC_BEGIN_URL)
        return err(TINC_T_REQ_BEGIN, seq, TINC_ERR_BAD_LEN);
    ul = tinc_get_u16(pl + TINC_BEGIN_URL_LEN);
    hl = tinc_get_u16(pl + TINC_BEGIN_HDR_LEN);
    if ((uint32_t)TINC_BEGIN_URL + ul + hl > len)
        return err(TINC_T_REQ_BEGIN, seq, TINC_ERR_BAD_LEN);
    if (pl[TINC_BEGIN_METHOD] != TINC_METHOD_GET || tinc_get_u32(pl + TINC_BEGIN_CONTENT_LEN) != 0)
        return err(TINC_T_REQ_BEGIN, seq, TINC_ERR_BAD_ARG);
    e = tinc_req_begin(pl[TINC_BEGIN_FLAGS], pl[TINC_BEGIN_TIMEOUT_S],
                       (const char *)pl + TINC_BEGIN_URL, ul,
                       (const char *)pl + TINC_BEGIN_URL + ul, hl);
    if (e == TINC_ERR_BUSY)
        return err(TINC_T_REQ_BEGIN, seq, e);
    reset_body();
    return e ? err(TINC_T_REQ_BEGIN, seq, e) : ok(TINC_T_REQ_BEGIN, seq, 0);
}

static uint16_t req_status(uint8_t seq)
{
    const char *ct = tinc_req_ctype();
    uint16_t n = (uint16_t)strlen(ct), room = (uint16_t)(peer_max - TINC_RSTAT_CTYPE);

    if (n > TINC_CTYPE_MAX)
        n = TINC_CTYPE_MAX;
    if (n > room)
        n = room;
    PL[TINC_RSTAT_STATE] = tinc_req_state();
    PL[TINC_RSTAT_ERR] = tinc_req_err();
    tinc_put_u16(PL + TINC_RSTAT_HTTP_STATUS, tinc_req_http_status());
    tinc_put_u32(PL + TINC_RSTAT_CONTENT_LEN, tinc_req_content_len());
    PL[TINC_RSTAT_CTYPE_LEN] = (uint8_t)n;
    memcpy(PL + TINC_RSTAT_CTYPE, ct, n);
    return ok(TINC_T_REQ_STATUS, seq, (uint16_t)(TINC_RSTAT_CTYPE + n));
}

static uint16_t body_reply(uint8_t seq, uint32_t off, uint16_t n, int eof)
{
    tinc_put_u32(PL + TINC_READ_OFFSET, off);
    PL[TINC_READ_FLAGS] = eof ? TINC_READF_EOF : 0;
    memcpy(PL + TINC_READ_DATA, chunk, n);
    return ok(TINC_T_BODY_READ, seq, (uint16_t)(TINC_READ_DATA + n));
}

static uint16_t body_read(uint8_t seq, const uint8_t *pl, uint16_t len, int final)
{
    uint8_t st = tinc_req_state(), wait;
    uint32_t off, now;
    uint16_t cap, n;
    int eof;

    if (len < TINC_READ_REQ_LEN)
        return err(TINC_T_BODY_READ, seq, TINC_ERR_BAD_LEN);
    if (st == TINC_RS_ERROR)
        return err(TINC_T_BODY_READ, seq, tinc_req_err());
    if (st != TINC_RS_BODY && st != TINC_RS_DONE)
        return err(TINC_T_BODY_READ, seq, TINC_ERR_BAD_STATE);

    off = tinc_get_u32(pl + TINC_READ_OFFSET);
    if (have_chunk && off == last_off)
        return body_reply(seq, off, last_len, chunk_eof);
    if (off != last_off + last_len)
        return err(TINC_T_BODY_READ, seq, TINC_ERR_BAD_OFFSET);

    cap = tinc_get_u16(pl + TINC_READ_MAX_LEN);
    if (cap > peer_max - TINC_READ_DATA)
        cap = (uint16_t)(peer_max - TINC_READ_DATA);
    eof = 1;
    n = st == TINC_RS_DONE ? 0 : tinc_req_read(chunk, cap, &eof);
    if (tinc_req_state() == TINC_RS_ERROR) {
        waiting = 0;
        return err(TINC_T_BODY_READ, seq, tinc_req_err());
    }
    if (!n && !eof) {
        now = tinc_plat_millis();
        if (!waiting) {
            wait = pl[TINC_READ_WAIT_MS];
            wait_until = now + (wait > TINC_WAIT_MS_MAX ? TINC_WAIT_MS_MAX : wait);
            waiting = 1;
        }
        if (!final && (int32_t)(wait_until - now) > 0)
            return TINC_PENDING;
    }
    waiting = 0;
    if (eof && st == TINC_RS_BODY)
        tinc_req_mark_done();
    last_off = off;
    last_len = n;
    have_chunk = n > 0; /* an empty chunk is never cached */
    chunk_eof = (uint8_t)eof;
    return body_reply(seq, off, n, eof);
}

static uint16_t wifi_list(uint8_t seq)
{
    uint16_t n = 0;
    uint8_t i, l;

    /* ponytail: 3 x 33 bytes can exceed a 64-byte peer max_payload; spec gap */
    for (i = 0; i < TINC_WIFI_SLOTS; i++) {
        l = (uint8_t)strlen(slots.ssid[i]);
        PL[n++] = l;
        memcpy(PL + n, slots.ssid[i], l);
        n = (uint16_t)(n + l);
    }
    return ok(TINC_T_WIFI_LIST, seq, n);
}

static uint16_t wifi_set(uint8_t seq, const uint8_t *pl, uint16_t len)
{
    uint8_t slot, sl, pw;

    if (len < TINC_WSET_SSID + 1u || len < TINC_WSET_SSID + 1u + pl[TINC_WSET_SSID_LEN])
        return err(TINC_T_WIFI_SET, seq, TINC_ERR_BAD_LEN);
    slot = pl[TINC_WSET_SLOT];
    sl = pl[TINC_WSET_SSID_LEN];
    pw = pl[TINC_WSET_SSID + sl];
    if (len < TINC_WSET_SSID + 1u + sl + pw)
        return err(TINC_T_WIFI_SET, seq, TINC_ERR_BAD_LEN);
    /* open network (no password) or WPA 8..64 */
    if (slot >= TINC_WIFI_SLOTS || sl == 0 || sl > TINC_SSID_MAX ||
        pw > TINC_PASS_MAX || (pw && pw < 8))
        return err(TINC_T_WIFI_SET, seq, TINC_ERR_BAD_ARG);

    memset(slots.ssid[slot], 0, sizeof slots.ssid[slot]);
    memset(slots.pass[slot], 0, sizeof slots.pass[slot]);
    memcpy(slots.ssid[slot], pl + TINC_WSET_SSID, sl);
    memcpy(slots.pass[slot], pl + TINC_WSET_SSID + sl + 1, pw);
    /* ponytail: 0.1 has no storage error code; NO_MEM is the closest */
    if (tinc_plat_slots_save(&slots) != 0)
        return err(TINC_T_WIFI_SET, seq, TINC_ERR_NO_MEM);
    tinc_plat_wifi_reconnect();
    return ok(TINC_T_WIFI_SET, seq, 0);
}

static uint16_t wifi_forget(uint8_t seq, const uint8_t *pl, uint16_t len)
{
    tinc_wifi_info w;
    uint8_t slot;

    if (len < 1)
        return err(TINC_T_WIFI_FORGET, seq, TINC_ERR_BAD_LEN);
    slot = pl[TINC_WFORGET_SLOT];
    if (slot >= TINC_WIFI_SLOTS)
        return err(TINC_T_WIFI_FORGET, seq, TINC_ERR_BAD_ARG);
    memset(slots.ssid[slot], 0, sizeof slots.ssid[slot]);
    memset(slots.pass[slot], 0, sizeof slots.pass[slot]);
    if (tinc_plat_slots_save(&slots) != 0)
        return err(TINC_T_WIFI_FORGET, seq, TINC_ERR_NO_MEM);
    tinc_plat_wifi_info(&w);
    if (w.slot == slot)
        tinc_plat_wifi_reconnect();
    return ok(TINC_T_WIFI_FORGET, seq, 0);
}

uint16_t tinc_dispatch(uint8_t type, uint8_t seq, const uint8_t *pl,
                       uint16_t len, int final, const uint8_t **out)
{
    uint16_t n;

    *out = reply;
    last_rx = tinc_plat_millis();
    if (type == TINC_T_HELLO)
        return hello(seq, pl, len); /* always executed, never cached */
    if (!hello_ok)
        return err(type, seq, TINC_ERR_NO_HELLO);
    if (cache_ok && type == cache_type && seq == cache_seq)
        return (uint16_t)(TINC_OVERHEAD + tinc_get_u16(reply + 4));

    switch (type) {
    case TINC_T_STATUS:      n = status(seq); break;
    case TINC_T_REQ_BEGIN:   n = req_begin(seq, pl, len); break;
    case TINC_T_REQ_STATUS:  n = req_status(seq); break;
    case TINC_T_REQ_ABORT:   tinc_req_release(); reset_body(); n = ok(type, seq, 0); break;
    case TINC_T_BODY_READ:   n = body_read(seq, pl, len, final); break;
    case TINC_T_WIFI_LIST:   n = wifi_list(seq); break;
    case TINC_T_WIFI_SET:    n = wifi_set(seq, pl, len); break;
    case TINC_T_WIFI_FORGET: n = wifi_forget(seq, pl, len); break;
    default:                 n = err(type, seq, TINC_ERR_UNSUPPORTED); break;
    }
    if (n != TINC_PENDING) {
        cache_ok = 1;
        cache_type = type;
        cache_seq = seq;
    }
    return n;
}

void tinc_poll(void)
{
    tinc_req_poll();
    if (tinc_req_state() != TINC_RS_IDLE &&
        tinc_plat_millis() - last_rx > TINC_WATCHDOG_MS) {
        tinc_plat_log("watchdog: CE silent, request freed");
        tinc_req_release();
        reset_body();
    }
}
