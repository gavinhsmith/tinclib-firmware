/* Native tests for the core layer: pio test -e native */
#include <stdio.h>
#include <unity.h>
#include "../fake_platform.h"
#include "tinc_frame.h"
#include "test_vectors/vectors.h"

static const uint8_t *rep;
static uint16_t rep_len;
static uint8_t seqn;

#define RPL (rep + TINC_HDR_LEN)
#define RPL_LEN ((uint16_t)(rep_len - TINC_OVERHEAD))

static uint16_t send_final(uint8_t type, uint8_t seq, const uint8_t *pl, uint16_t len, int final)
{
    rep_len = tinc_dispatch(type, seq, pl, len, final, &rep);
    return rep_len;
}

static uint16_t send(uint8_t type, const uint8_t *pl, uint16_t len)
{
    return send_final(type, ++seqn, pl, len, 1);
}

static void send_vec(const uint8_t *v, uint16_t n)
{
    static uint8_t buf[TINC_FRAME_BUF(TINC_PAYLOAD_LIMIT)];
    tinc_parser p;
    uint16_t i;
    uint8_t r = 0;

    tinc_parser_init(&p, buf, sizeof buf);
    for (i = 0; i < n; i++)
        r = tinc_parser_feed(&p, v[i]);
    TEST_ASSERT_EQUAL(TINC_PARSE_FRAME, r);
    send_final(p.type, p.seq, p.payload, p.len, 1);
}

static void expect_vec(const uint8_t *v, uint16_t n)
{
    TEST_ASSERT_EQUAL_UINT16(n, rep_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v, rep, n);
}

static void expect_ok(void)
{
    TEST_ASSERT_EQUAL_HEX8(TINC_FLAG_RESP, rep[1]);
}

static void expect_err(uint8_t e)
{
    TEST_ASSERT_EQUAL_HEX8(TINC_FLAG_RESP | TINC_FLAG_ERR, rep[1]);
    TEST_ASSERT_EQUAL_HEX8(e, RPL[0]);
}

static void hello(void)
{
    uint8_t pl[TINC_HELLO_REQ_LEN] = {TINC_PROTO_MAJOR, TINC_PROTO_MINOR, 0, 0};
    tinc_put_u16(pl + TINC_HELLO_MAX_PAYLOAD, TINC_PAYLOAD_LIMIT);
    send(TINC_T_HELLO, pl, sizeof pl);
    expect_ok();
}

static void begin(const char *url, const char *hdrs, uint8_t flags)
{
    static uint8_t pl[TINC_PAYLOAD_LIMIT];
    uint16_t ul = (uint16_t)strlen(url), hl = (uint16_t)strlen(hdrs);

    memset(pl, 0, TINC_BEGIN_URL);
    pl[TINC_BEGIN_METHOD] = TINC_METHOD_GET;
    pl[TINC_BEGIN_FLAGS] = flags;
    tinc_put_u16(pl + TINC_BEGIN_URL_LEN, ul);
    tinc_put_u16(pl + TINC_BEGIN_HDR_LEN, hl);
    memcpy(pl + TINC_BEGIN_URL, url, ul);
    memcpy(pl + TINC_BEGIN_URL + ul, hdrs, hl);
    send(TINC_T_REQ_BEGIN, pl, (uint16_t)(TINC_BEGIN_URL + ul + hl));
}

static void pump(int n)
{
    while (n--)
        tinc_poll();
}

/* Begin, connect, send, and serve a response; ends in BODY (or wherever the
 * response leads). */
static void fetch(const char *url, uint8_t flags, const char *resp)
{
    begin(url, "", flags);
    expect_ok();
    fk_tcp = TINC_TCP_OPEN;
    pump(3);
    TEST_ASSERT_EQUAL(TINC_RS_WAIT_HEADERS, tinc_req_state());
    fk_serve(resp);
    pump(2);
}

static uint16_t body_read_at(uint8_t seq, uint32_t off, uint16_t max, uint8_t wait, int final)
{
    uint8_t pl[TINC_READ_REQ_LEN];
    tinc_put_u32(pl + TINC_READ_OFFSET, off);
    tinc_put_u16(pl + TINC_READ_MAX_LEN, max);
    pl[TINC_READ_WAIT_MS] = wait;
    return send_final(TINC_T_BODY_READ, seq, pl, sizeof pl, final);
}

static uint16_t body_read(uint32_t off, uint16_t max, uint8_t wait, int final)
{
    return body_read_at(++seqn, off, max, wait, final);
}

static void assert_prefix(const char *exp, const char *s)
{
    TEST_ASSERT_EQUAL_STRING_LEN(exp, s, strlen(exp));
}

/* Pull the whole body with small reads; returns its length. */
static uint16_t read_all(char *out, uint16_t max)
{
    uint32_t off = 0;
    int guard = 0;

    for (;;) {
        TEST_ASSERT_TRUE_MESSAGE(++guard < 1000, "body never reached EOF");
        body_read(off, max, 0, 1);
        expect_ok();
        memcpy(out + off, RPL + TINC_READ_DATA, RPL_LEN - TINC_READ_DATA);
        off += RPL_LEN - TINC_READ_DATA;
        if (RPL[TINC_READ_FLAGS] & TINC_READF_EOF)
            break;
        pump(1);
    }
    out[off] = 0;
    return (uint16_t)off;
}

void setUp(void)
{
    fk_reset();
    tinc_core_init();
    seqn = 100;
}

void tearDown(void) {}

/* ---- golden vectors from tinclib-protocol ---- */

static void test_golden_session(void)
{
    const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 17\r\n\r\n";
    uint8_t pl[TINC_READ_REQ_LEN];

    send_final(TINC_T_STATUS, 12, NULL, 0, 1);
    expect_vec(tv_err_no_hello, sizeof tv_err_no_hello);

    send_vec(tv_hello_req, sizeof tv_hello_req);
    expect_vec(tv_hello_resp, sizeof tv_hello_resp);

    fk_heap = 27500;
    fk_wifi.rssi = -61;
    memcpy(fk_wifi.ip, "\xC0\xA8\x01\x2A", 4);
    send_vec(tv_status_req, sizeof tv_status_req);
    expect_vec(tv_status_resp, sizeof tv_status_resp);

    send_vec(tv_req_begin_req, sizeof tv_req_begin_req);
    expect_vec(tv_req_begin_resp, sizeof tv_req_begin_resp);
    TEST_ASSERT_EQUAL_STRING("example.com", fk_host);
    TEST_ASSERT_EQUAL(80, fk_port);

    fk_tcp = TINC_TCP_OPEN;
    pump(3);
    TEST_ASSERT_EQUAL(TINC_RS_WAIT_HEADERS, tinc_req_state());
    assert_prefix("GET /api?q=1 HTTP/1.1\r\nHost: example.com\r\n", fk_sent);
    TEST_ASSERT_NOT_NULL(strstr(fk_sent, "\r\nAccept: application/json\r\n\r\n"));

    fk_serve(hdr);
    fk_serve("{\"ok\":true,");
    pump(2);
    TEST_ASSERT_EQUAL(TINC_RS_BODY, tinc_req_state());
    send_vec(tv_req_status_req, sizeof tv_req_status_req);
    expect_vec(tv_req_status_resp, sizeof tv_req_status_resp);

    send_vec(tv_body_read_req, sizeof tv_body_read_req);
    expect_vec(tv_body_read_resp, sizeof tv_body_read_resp);

    fk_serve("\"n\":1}");
    tinc_put_u32(pl + TINC_READ_OFFSET, 11);
    tinc_put_u16(pl + TINC_READ_MAX_LEN, 128);
    pl[TINC_READ_WAIT_MS] = 0;
    send_final(TINC_T_BODY_READ, 6, pl, sizeof pl, 1);
    expect_vec(tv_body_read_resp_eof, sizeof tv_body_read_resp_eof);
    TEST_ASSERT_EQUAL(TINC_RS_DONE, tinc_req_state());

    tinc_put_u32(pl + TINC_READ_OFFSET, 999);
    send_final(TINC_T_BODY_READ, 15, pl, sizeof pl, 1);
    expect_vec(tv_err_bad_offset, sizeof tv_err_bad_offset);

    send_vec(tv_req_abort_req, sizeof tv_req_abort_req);
    expect_vec(tv_req_abort_resp, sizeof tv_req_abort_resp);
    TEST_ASSERT_EQUAL(TINC_RS_IDLE, tinc_req_state());
}

static void test_golden_errors(void)
{
    static const char https[] = "https://example.com/";
    uint8_t pl[TINC_BEGIN_URL + sizeof https] = {TINC_METHOD_GET};
    uint8_t rd[TINC_READ_REQ_LEN] = {0};

    hello();
    send_final(0x13, 14, NULL, 0, 1);
    expect_vec(tv_err_unsupported, sizeof tv_err_unsupported);

    tinc_put_u16(pl + TINC_BEGIN_URL_LEN, sizeof https - 1);
    memcpy(pl + TINC_BEGIN_URL, https, sizeof https - 1);
    send_final(TINC_T_REQ_BEGIN, 16, pl, (uint16_t)(TINC_BEGIN_URL + sizeof https - 1), 1);
    expect_vec(tv_err_scheme, sizeof tv_err_scheme);

    begin("http://nosuch.example/", "", 0);
    expect_ok();
    fk_tcp = TINC_TCP_ERR_DNS;
    pump(1);
    send_final(TINC_T_BODY_READ, 17, rd, sizeof rd, 1);
    expect_vec(tv_err_request_dns, sizeof tv_err_request_dns);
}

static void test_golden_wifi(void)
{
    uint8_t home[] = {0, 7, 'H', 'o', 'm', 'e', 'N', 'e', 't', 0, 0};
    uint8_t phone[] = {2, 5, 'P', 'h', 'o', 'n', 'e', 8, 'h', 'u', 'n', 't', 'e', 'r', '2', '2', TINC_WF_HIDDEN};

    hello();
    send(TINC_T_WIFI_SET, home, sizeof home);
    expect_ok();
    send(TINC_T_WIFI_SET, phone, sizeof phone);
    expect_ok();
    send_vec(tv_wifi_list_req, sizeof tv_wifi_list_req);
    expect_vec(tv_wifi_list_resp, sizeof tv_wifi_list_resp);

    send_vec(tv_wifi_set_req, sizeof tv_wifi_set_req);
    expect_vec(tv_wifi_set_resp, sizeof tv_wifi_set_resp);
    TEST_ASSERT_EQUAL_STRING("hunter22", tinc_core_slots()->pass[1]);
    TEST_ASSERT_EQUAL(TINC_WF_HIDDEN, tinc_core_slots()->wflags[1]);
    send_vec(tv_wifi_forget_req, sizeof tv_wifi_forget_req);
    expect_vec(tv_wifi_forget_resp, sizeof tv_wifi_forget_resp);
    TEST_ASSERT_EQUAL_STRING("", tinc_core_slots()->ssid[1]);
    TEST_ASSERT_EQUAL_STRING("", tinc_core_slots()->pass[1]);
    TEST_ASSERT_EQUAL(0, tinc_core_slots()->wflags[1]);
}

/* 0.2 Wi-Fi lock: reported in STATUS; WIFI_SET/FORGET refused, LIST still works */
static void test_golden_wifi_lock(void)
{
    uint8_t set[] = {1, 5, 'P', 'h', 'o', 'n', 'e', 0, 0};
    uint8_t slot = 0;

    send_vec(tv_hello_req, sizeof tv_hello_req);
    strcpy(tinc_core_slots()->ssid[0], "Kept");
    fk_wifi.locked = 1;
    fk_heap = 27500;
    fk_wifi.rssi = -61;
    memcpy(fk_wifi.ip, "\xC0\xA8\x01\x2A", 4);
    send_vec(tv_status_req, sizeof tv_status_req);
    expect_vec(tv_status_resp_locked, sizeof tv_status_resp_locked);

    send_final(TINC_T_WIFI_SET, 16, set, sizeof set, 1);
    expect_vec(tv_err_locked, sizeof tv_err_locked);
    send(TINC_T_WIFI_FORGET, &slot, 1);
    expect_err(TINC_ERR_LOCKED);
    TEST_ASSERT_EQUAL_STRING("Kept", tinc_core_slots()->ssid[0]);
    TEST_ASSERT_EQUAL_STRING("", tinc_core_slots()->ssid[1]);
    TEST_ASSERT_EQUAL(0, fk_saves);
    TEST_ASSERT_EQUAL(0, fk_reconnects);

    send(TINC_T_WIFI_LIST, NULL, 0);
    expect_ok();
    TEST_ASSERT_EQUAL_HEX8_ARRAY("\x04Kept\x00\x00\x00\x00\x00", RPL, 10);
}

/* ---- dispatcher ---- */

static void test_version_mismatch(void)
{
    uint8_t pl[TINC_HELLO_REQ_LEN] = {TINC_PROTO_MAJOR, TINC_PROTO_MINOR + 1, 0, 0, 0, 1};

    send(TINC_T_HELLO, pl, sizeof pl);
    expect_err(TINC_ERR_VERSION);
    TEST_ASSERT_EQUAL(3, RPL_LEN);
    TEST_ASSERT_EQUAL(TINC_PROTO_MAJOR, RPL[1]);
    TEST_ASSERT_EQUAL(TINC_PROTO_MINOR, RPL[2]);
    send(TINC_T_STATUS, NULL, 0);
    expect_err(TINC_ERR_NO_HELLO);
}

static void test_bad_len(void)
{
    uint8_t pl[TINC_BEGIN_URL + 4] = {TINC_METHOD_GET};

    hello();
    send(TINC_T_HELLO, pl, 3);
    expect_err(TINC_ERR_BAD_LEN);
    hello();
    send(TINC_T_REQ_BEGIN, pl, 5);
    expect_err(TINC_ERR_BAD_LEN);
    tinc_put_u16(pl + TINC_BEGIN_URL_LEN, 50); /* url runs past the payload */
    send(TINC_T_REQ_BEGIN, pl, sizeof pl);
    expect_err(TINC_ERR_BAD_LEN);
    send(TINC_T_BODY_READ, pl, 3);
    expect_err(TINC_ERR_BAD_LEN);
    send(TINC_T_WIFI_SET, pl, 1);
    expect_err(TINC_ERR_BAD_LEN);
}

static void test_seq_replay_does_not_rerun(void)
{
    uint8_t first[64];
    uint16_t n;

    hello();
    begin("http://a.example/", "", 0);
    expect_ok();
    n = rep_len;
    memcpy(first, rep, n);
    TEST_ASSERT_EQUAL(1, fk_opens);

    send_final(TINC_T_REQ_BEGIN, seqn, NULL, 0, 1); /* retry: same TYPE+SEQ */
    TEST_ASSERT_EQUAL_UINT16(n, rep_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(first, rep, n);
    TEST_ASSERT_EQUAL(1, fk_opens);

    begin("http://b.example/", "", 0); /* new SEQ while active */
    expect_err(TINC_ERR_BUSY);
    TEST_ASSERT_EQUAL(1, fk_opens);
}

static void test_begin_validation(void)
{
    uint8_t pl[TINC_BEGIN_URL] = {2};

    hello();
    send(TINC_T_REQ_BEGIN, pl, sizeof pl); /* method 2 */
    expect_err(TINC_ERR_BAD_ARG);
    begin("ftp://x/", "", 0);
    expect_err(TINC_ERR_BAD_ARG);
    begin("http://x y/", "", 0);
    expect_err(TINC_ERR_BAD_ARG);
    begin("http://user@x/", "", 0);
    expect_err(TINC_ERR_BAD_ARG);
    begin("http://x:99999/", "", 0);
    expect_err(TINC_ERR_BAD_ARG);
    begin("http://x/", "X-A: b", 0); /* not CRLF-terminated */
    expect_err(TINC_ERR_BAD_ARG);
    fk_wifi.state = TINC_WIFI_CONNECTING;
    begin("http://x/", "", 0);
    expect_err(TINC_ERR_WIFI_DOWN);
    TEST_ASSERT_EQUAL(0, fk_opens);
}

static void test_begin_replaces_finished_request(void)
{
    char body[64];

    hello();
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    read_all(body, 64);
    TEST_ASSERT_EQUAL(TINC_RS_DONE, tinc_req_state());
    begin("http://y:8080/p", "", 0);
    expect_ok();
    TEST_ASSERT_EQUAL_STRING("y", fk_host);
    TEST_ASSERT_EQUAL(8080, fk_port);
    TEST_ASSERT_EQUAL(TINC_RS_CONNECTING, tinc_req_state());
}

static void test_headers_never_logged(void)
{
    hello();
    begin("http://x/", "Authorization: Bearer s3cret\r\n", 0);
    expect_ok();
    fk_tcp = TINC_TCP_OPEN;
    pump(3);
    TEST_ASSERT_NOT_NULL(strstr(fk_sent, "Authorization: Bearer s3cret\r\n\r\n"));
    TEST_ASSERT_NULL(strstr(fk_log, "s3cret"));
    TEST_ASSERT_NULL(strstr(fk_log, "Authorization"));
}

static void test_partial_writes(void)
{
    hello();
    begin("http://x/long/path?q=1", "A: b\r\n", 0);
    fk_tcp = TINC_TCP_OPEN;
    fk_write_max = 3;
    pump(200);
    TEST_ASSERT_EQUAL(TINC_RS_WAIT_HEADERS, tinc_req_state());
    assert_prefix("GET /long/path?q=1 HTTP/1.1\r\nHost: x\r\n", fk_sent);
    TEST_ASSERT_EQUAL_STRING("A: b\r\n\r\n", fk_sent + fk_sent_len - 8);
}

/* ---- HTTP parsing ---- */

static void test_content_length(void)
{
    char body[64];

    hello();
    fetch("http://x/", 0, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nhello world");
    TEST_ASSERT_EQUAL(TINC_RS_BODY, tinc_req_state());
    TEST_ASSERT_EQUAL(200, tinc_req_http_status());
    TEST_ASSERT_EQUAL_UINT32(11, tinc_req_content_len());
    TEST_ASSERT_EQUAL(11, read_all(body, 4));
    TEST_ASSERT_EQUAL_STRING("hello world", body);
}

static void test_chunked_byte_at_a_time(void)
{
    char body[64];

    hello();
    fk_read_max = 1;
    begin("http://x/", "", 0);
    fk_tcp = TINC_TCP_OPEN;
    pump(3);
    fk_serve("HTTP/1.1 100 Continue\r\n\r\n"
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
             "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-Trailer: y\r\n\r\n");
    pump(200);
    TEST_ASSERT_EQUAL(TINC_RS_BODY, tinc_req_state());
    TEST_ASSERT_EQUAL(200, tinc_req_http_status());
    TEST_ASSERT_EQUAL_UINT32(TINC_LEN_UNKNOWN, tinc_req_content_len());
    TEST_ASSERT_EQUAL(11, read_all(body, 7));
    TEST_ASSERT_EQUAL_STRING("hello world", body);
}

static void test_bad_chunk_size(void)
{
    hello();
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");
    body_read(0, 64, 0, 1);
    expect_err(TINC_ERR_HTTP_PROTO);
}

static void test_close_delimited(void)
{
    char body[64];

    hello();
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\n\r\nuntil close");
    fk_tcp = TINC_TCP_CLOSED;
    TEST_ASSERT_EQUAL(11, read_all(body, 64));
    TEST_ASSERT_EQUAL_STRING("until close", body);
}

static void test_truncated_body(void)
{
    hello();
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort");
    fk_tcp = TINC_TCP_CLOSED;
    body_read(0, 64, 0, 1);
    expect_ok(); /* the bytes that did arrive */
    body_read(5, 64, 0, 1);
    expect_err(TINC_ERR_HTTP_PROTO);
}

static void test_malformed_status(void)
{
    hello();
    fetch("http://x/", 0, "HTP/1.1 200 OK\r\n\r\n");
    TEST_ASSERT_EQUAL(TINC_RS_ERROR, tinc_req_state());
    TEST_ASSERT_EQUAL(TINC_ERR_HTTP_PROTO, tinc_req_err());
}

static void test_close_before_headers(void)
{
    hello();
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-");
    fk_tcp = TINC_TCP_CLOSED;
    pump(2);
    TEST_ASSERT_EQUAL(TINC_ERR_HTTP_PROTO, tinc_req_err());
}

static void test_redirects(void)
{
    char body[64];

    hello();
    fetch("http://x/old", 0, "HTTP/1.1 301 Moved\r\nLocation: /new?a=1 \r\n\r\n");
    TEST_ASSERT_EQUAL(TINC_RS_CONNECTING, tinc_req_state());
    TEST_ASSERT_EQUAL(2, fk_opens);
    fk_tcp = TINC_TCP_OPEN;
    pump(3);
    assert_prefix("GET /new?a=1 HTTP/1.1\r\nHost: x\r\n", fk_sent);

    fk_serve("HTTP/1.1 302 Found\r\nLocation: http://other:81/z\r\n\r\n");
    pump(2);
    TEST_ASSERT_EQUAL_STRING("other", fk_host);
    TEST_ASSERT_EQUAL(81, fk_port);
    fk_tcp = TINC_TCP_OPEN;
    pump(3);
    fk_serve("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    pump(2);
    TEST_ASSERT_EQUAL(2, read_all(body, 64));
    TEST_ASSERT_EQUAL_STRING("ok", body);
}

static void test_redirect_to_https(void)
{
    hello();
    fetch("http://x/", 0, "HTTP/1.1 301 Moved\r\nLocation: https://x/\r\n\r\n");
    TEST_ASSERT_EQUAL(TINC_RS_ERROR, tinc_req_state());
    TEST_ASSERT_EQUAL(TINC_ERR_UNSUPPORTED_SCHEME, tinc_req_err());
}

static void test_redirect_limit(void)
{
    unsigned i;

    hello();
    begin("http://x/", "", 0);
    for (i = 0; i <= TINC_REDIRECT_MAX; i++) {
        fk_tcp = TINC_TCP_OPEN;
        pump(3);
        fk_serve("HTTP/1.1 302 Found\r\nLocation: /again\r\n\r\n");
        pump(2);
    }
    TEST_ASSERT_EQUAL(TINC_RS_ERROR, tinc_req_state());
    TEST_ASSERT_EQUAL(TINC_ERR_TOO_MANY_REDIRECTS, tinc_req_err());
}

static void test_no_content(void)
{
    hello();
    fetch("http://x/", 0, "HTTP/1.1 204 No Content\r\n\r\n");
    body_read(0, 64, 0, 1);
    expect_ok();
    TEST_ASSERT_EQUAL(TINC_READ_DATA, RPL_LEN);
    TEST_ASSERT_EQUAL(TINC_READF_EOF, RPL[TINC_READ_FLAGS]);
}

/* ---- BODY_READ offsets, long-poll, linger ---- */

static void test_body_read_offsets(void)
{
    hello();
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789");
    body_read(0, 4, 0, 1);
    TEST_ASSERT_EQUAL_STRING_LEN("0123", RPL + TINC_READ_DATA, 4);
    body_read(0, 4, 0, 1); /* retry with a new SEQ: same chunk */
    TEST_ASSERT_EQUAL(TINC_READ_DATA + 4, RPL_LEN);
    TEST_ASSERT_EQUAL_STRING_LEN("0123", RPL + TINC_READ_DATA, 4);
    body_read(2, 4, 0, 1);
    expect_err(TINC_ERR_BAD_OFFSET);
    body_read(4, 4, 0, 1);
    TEST_ASSERT_EQUAL_STRING_LEN("4567", RPL + TINC_READ_DATA, 4);
    body_read(0, 4, 0, 1); /* old chunk is gone */
    expect_err(TINC_ERR_BAD_OFFSET);
}

static void test_body_read_bad_state(void)
{
    hello();
    body_read(0, 4, 0, 1);
    expect_err(TINC_ERR_BAD_STATE);
    begin("http://x/", "", 0);
    body_read(0, 4, 0, 1);
    expect_err(TINC_ERR_BAD_STATE);
}

static void test_long_poll(void)
{
    uint8_t s;

    hello();
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n");
    TEST_ASSERT_EQUAL_UINT16(TINC_PENDING, body_read(0, 64, 250, 0)); /* clamped to 100 */
    s = seqn;
    fk_now += 60;
    pump(1);
    TEST_ASSERT_EQUAL_UINT16(TINC_PENDING, body_read_at(s, 0, 64, 250, 0));
    fk_now += 41;
    body_read_at(s, 0, 64, 250, 0);
    expect_ok();
    TEST_ASSERT_EQUAL(TINC_READ_DATA, RPL_LEN); /* empty, not EOF */
    TEST_ASSERT_EQUAL(0, RPL[TINC_READ_FLAGS]);

    /* a waiting read answers as soon as data shows up */
    TEST_ASSERT_EQUAL_UINT16(TINC_PENDING, body_read(0, 64, 100, 0));
    s = seqn;
    fk_serve("data");
    pump(1);
    body_read_at(s, 0, 64, 100, 0);
    expect_ok();
    TEST_ASSERT_EQUAL_STRING_LEN("data", RPL + TINC_READ_DATA, 4);
    TEST_ASSERT_EQUAL(TINC_READF_EOF, RPL[TINC_READ_FLAGS]);

    /* final (another frame waiting on the UART) ends the wait at once */
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n");
    body_read(0, 64, 100, 1);
    expect_ok();
    TEST_ASSERT_EQUAL(TINC_READ_DATA, RPL_LEN);
}

static void test_done_linger(void)
{
    char body[8];

    hello();
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    read_all(body, 8);
    TEST_ASSERT_EQUAL(TINC_RS_DONE, tinc_req_state());
    body_read(0, 8, 0, 1); /* retried final read still works */
    TEST_ASSERT_EQUAL_STRING_LEN("hi", RPL + TINC_READ_DATA, 2);
    fk_now += TINC_DONE_LINGER_MS;
    pump(1);
    TEST_ASSERT_EQUAL(TINC_RS_IDLE, tinc_req_state());
    body_read(2, 8, 0, 1);
    expect_err(TINC_ERR_BAD_STATE);
}

/* ---- timeouts, watchdog, Wi-Fi, heap ---- */

static void test_connect_timeout(void)
{
    hello();
    begin("http://x/", "", 0);
    fk_now += TINC_TIMEOUT_S_DEFAULT * 1000u;
    pump(1);
    TEST_ASSERT_EQUAL(TINC_RS_CONNECTING, tinc_req_state());
    fk_now += 1;
    pump(1);
    TEST_ASSERT_EQUAL(TINC_ERR_TIMEOUT, tinc_req_err());
}

/* A state change mid-poll stamps the phase with a later millisecond than the
 * poll's own start time; the timeout check must not see that as ~49 days. */
static void test_clock_ticking_during_poll(void)
{
    char body[64];

    hello();
    fk_tick = 1;
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    TEST_ASSERT_EQUAL(TINC_RS_BODY, tinc_req_state());
    TEST_ASSERT_EQUAL(5, read_all(body, 64));
    TEST_ASSERT_EQUAL_STRING("hello", body);
}

static void test_body_gap_timeout(void)
{
    hello();
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc");
    /* data waiting for the CE: the server isn't the one being slow */
    fk_now += 20000;
    pump(1);
    TEST_ASSERT_EQUAL(TINC_RS_BODY, tinc_req_state());
    body_read(0, 64, 0, 1);
    fk_now += 10001;
    pump(1);
    TEST_ASSERT_EQUAL(TINC_ERR_TIMEOUT, tinc_req_err());
}

static void test_watchdog(void)
{
    hello();
    begin("http://x/", "", 0);
    fk_tcp = TINC_TCP_OPEN;
    fk_now += TINC_WATCHDOG_MS + 1;
    pump(1);
    TEST_ASSERT_EQUAL(TINC_RS_IDLE, tinc_req_state());
    TEST_ASSERT_EQUAL(TINC_TCP_IDLE, fk_tcp);
}

static void test_wifi_drop(void)
{
    hello();
    begin("http://x/", "", 0);
    fk_wifi.state = TINC_WIFI_CONNECTING;
    pump(1);
    TEST_ASSERT_EQUAL(TINC_ERR_WIFI_DOWN, tinc_req_err());
}

static void test_low_heap(void)
{
    hello();
    fk_heap = 1000;
    begin("http://x/", "", 0);
    expect_ok();
    TEST_ASSERT_EQUAL(TINC_RS_ERROR, tinc_req_state());
    TEST_ASSERT_EQUAL(TINC_ERR_NO_MEM, tinc_req_err());
    TEST_ASSERT_EQUAL(0, fk_opens);
}

static void test_connect_refused(void)
{
    hello();
    begin("http://x/", "", 0);
    fk_tcp = TINC_TCP_ERR_CONNECT;
    pump(1);
    TEST_ASSERT_EQUAL(TINC_ERR_CONNECT, tinc_req_err());
}

static void test_wifi_set_validation(void)
{
    uint8_t bad_slot[] = {3, 1, 'a', 0};
    uint8_t short_pw[] = {0, 1, 'a', 3, 'x', 'y', 'z'};
    uint8_t ok_open[] = {0, 1, 'a', 0};
    uint8_t forget_other = 2;

    hello();
    send(TINC_T_WIFI_SET, bad_slot, sizeof bad_slot);
    expect_err(TINC_ERR_BAD_ARG);
    send(TINC_T_WIFI_SET, short_pw, sizeof short_pw);
    expect_err(TINC_ERR_BAD_ARG);
    TEST_ASSERT_EQUAL(0, fk_saves);

    fk_save_fail = 1;
    send(TINC_T_WIFI_SET, ok_open, sizeof ok_open);
    expect_err(TINC_ERR_NO_MEM);
    fk_save_fail = 0;
    send(TINC_T_WIFI_SET, ok_open, sizeof ok_open);
    expect_ok();
    TEST_ASSERT_EQUAL(1, fk_reconnects);

    /* forgetting a slot we're not on keeps the link */
    fk_wifi.slot = 0;
    send(TINC_T_WIFI_FORGET, &forget_other, 1);
    expect_ok();
    TEST_ASSERT_EQUAL(1, fk_reconnects);
}

static void test_wifi_set_wflags(void)
{
    uint8_t no_flags[] = {0, 1, 'a', 0};             /* a 0.1-style payload: wflags missing */
    uint8_t odd_flags[] = {1, 1, 'b', 0, 0xFF};      /* unknown bits are dropped */

    hello();
    send(TINC_T_WIFI_SET, no_flags, sizeof no_flags);
    expect_ok();
    TEST_ASSERT_EQUAL(0, tinc_core_slots()->wflags[0]);
    send(TINC_T_WIFI_SET, odd_flags, sizeof odd_flags);
    expect_ok();
    TEST_ASSERT_EQUAL(TINC_WF_HIDDEN, tinc_core_slots()->wflags[1]);
    send(TINC_T_WIFI_LIST, NULL, 0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY("\x01" "a" "\x01" "b" "\x00" "\x00\x01\x00", RPL, 8);
}

static void test_wifi_list_hides_passwords(void)
{
    uint8_t set[] = {0, 2, 'n', 'w', 9, 'p', 'a', 's', 's', 'w', 'o', 'r', 'd', '!'};

    hello();
    send(TINC_T_WIFI_SET, set, sizeof set);
    send(TINC_T_WIFI_LIST, NULL, 0);
    expect_ok();
    TEST_ASSERT_EQUAL(8, RPL_LEN); /* 2+"nw", 0, 0, then 3 wflags */
    TEST_ASSERT_NULL(memchr(RPL, 'p', RPL_LEN));
}

/* ---- transcoding ---- */

static void test_transcode(void)
{
    char body[128];

    hello();
    fk_read_max = 3; /* split multi-byte sequences across reads */
    fetch("http://x/", TINC_REQF_TRANSCODE,
          "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 24\r\n\r\n"
          "\xE2\x80\x9CHi\xE2\x80\x9D \xE2\x80\x94 caf\xC3\xA9\xE2\x80\xA6 \xC2\xA9");
    pump(50);
    TEST_ASSERT_EQUAL_UINT32(TINC_LEN_UNKNOWN, tinc_req_content_len());
    read_all(body, 16);
    TEST_ASSERT_EQUAL_STRING("\"Hi\" - cafe... (c)", body);
}

static void test_transcode_skips_binary(void)
{
    char body[16];

    hello();
    fetch("http://x/", TINC_REQF_TRANSCODE,
          "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: 2\r\n\r\n\xC3\xA9");
    TEST_ASSERT_EQUAL_UINT32(2, tinc_req_content_len());
    read_all(body, 16);
    TEST_ASSERT_EQUAL_STRING("\xC3\xA9", body);
}

static void test_transcode_unit(void)
{
    tinc_tc t = {0, 0};
    uint8_t out[TINC_TC_MAX_OUT];

    TEST_ASSERT_EQUAL(0, tinc_tc_byte(&t, 0xE2, out));
    TEST_ASSERT_EQUAL(2, tinc_tc_byte(&t, 'A', out)); /* truncated sequence */
    TEST_ASSERT_EQUAL_STRING_LEN("?A", out, 2);
    TEST_ASSERT_EQUAL(1, tinc_tc_byte(&t, 0xFF, out));
    TEST_ASSERT_EQUAL('?', out[0]);
    tinc_tc_byte(&t, 0xF0, out);
    tinc_tc_byte(&t, 0x9F, out);
    TEST_ASSERT_EQUAL(1, tinc_tc_flush(&t, out)); /* partial at EOF */
    TEST_ASSERT_TRUE(tinc_tc_applies("application/json"));
    TEST_ASSERT_TRUE(tinc_tc_applies("application/atom+xml"));
    TEST_ASSERT_FALSE(tinc_tc_applies("application/octet-stream"));
}

/* ---- Wi-Fi ranking ---- */

static void test_wifi_rank(void)
{
    tinc_slots s;
    tinc_scan scan[5];
    tinc_cand c[TINC_CAND_CAP];

    memset(&s, 0, sizeof s);
    memset(scan, 0, sizeof scan);
    strcpy(s.ssid[0], "Home");
    strcpy(s.ssid[2], "Mesh");
    strcpy(scan[0].ssid, "Home"); scan[0].rssi = -70;
    strcpy(scan[1].ssid, "Mesh"); scan[1].rssi = -60;
    strcpy(scan[2].ssid, "Mesh"); scan[2].rssi = -40; /* stronger AP, same SSID */
    strcpy(scan[3].ssid, "Home"); scan[3].rssi = -90; /* below floor */
    strcpy(scan[4].ssid, "Corp"); scan[4].rssi = -30;

    TEST_ASSERT_EQUAL(3, tinc_wifi_rank(&s, scan, 5, c));
    TEST_ASSERT_EQUAL(2, c[0].scan);
    TEST_ASSERT_EQUAL(2, c[0].slot);
    TEST_ASSERT_EQUAL(1, c[1].scan);
    TEST_ASSERT_EQUAL(0, c[2].scan);

    scan[2].enterprise = 1;
    TEST_ASSERT_EQUAL(2, tinc_wifi_rank(&s, scan, 5, c));
    TEST_ASSERT_EQUAL(1, c[0].scan);
}

static void test_wifi_rank_keeps_strongest_when_full(void)
{
    tinc_slots s;
    tinc_scan scan[TINC_CAND_MAX + 2];
    tinc_cand c[TINC_CAND_CAP];
    uint8_t i;

    memset(&s, 0, sizeof s);
    memset(scan, 0, sizeof scan);
    strcpy(s.ssid[1], "N");
    for (i = 0; i < TINC_CAND_MAX + 2; i++) {
        strcpy(scan[i].ssid, "N");
        scan[i].rssi = (int8_t)(-80 + i);
    }
    TEST_ASSERT_EQUAL(TINC_CAND_MAX, tinc_wifi_rank(&s, scan, TINC_CAND_MAX + 2, c));
    TEST_ASSERT_EQUAL(TINC_CAND_MAX + 1, c[0].scan);
    TEST_ASSERT_EQUAL(2, c[TINC_CAND_MAX - 1].scan);

    /* a hidden slot still gets its direct try when visible APs fill the list */
    strcpy(s.ssid[0], "Secret");
    s.wflags[0] = TINC_WF_HIDDEN;
    TEST_ASSERT_EQUAL(TINC_CAND_MAX + 1, tinc_wifi_rank(&s, scan, TINC_CAND_MAX + 2, c));
    TEST_ASSERT_EQUAL(0, c[TINC_CAND_MAX].slot);
    TEST_ASSERT_EQUAL(TINC_CAND_DIRECT, c[TINC_CAND_MAX].scan);
}

static void test_wifi_rank_hidden(void)
{
    tinc_slots s;
    tinc_scan scan[2];
    tinc_cand c[TINC_CAND_CAP];

    memset(&s, 0, sizeof s);
    memset(scan, 0, sizeof scan);
    strcpy(s.ssid[0], "Hidden");
    s.wflags[0] = TINC_WF_HIDDEN;
    strcpy(s.ssid[1], "Open");
    strcpy(scan[0].ssid, "Open"); scan[0].rssi = -50;

    /* visible matches first, then the hidden slot to be tried directly */
    TEST_ASSERT_EQUAL(2, tinc_wifi_rank(&s, scan, 1, c));
    TEST_ASSERT_EQUAL(1, c[0].slot);
    TEST_ASSERT_EQUAL(0, c[0].scan);
    TEST_ASSERT_EQUAL(0, c[1].slot);
    TEST_ASSERT_EQUAL(TINC_CAND_DIRECT, c[1].scan);

    /* nothing seen at all: the hidden slot is still a candidate */
    TEST_ASSERT_EQUAL(1, tinc_wifi_rank(&s, scan, 0, c));
    TEST_ASSERT_EQUAL(TINC_CAND_DIRECT, c[0].scan);

    /* a "hidden" network that does show up is ranked normally, not twice */
    strcpy(scan[1].ssid, "Hidden"); scan[1].rssi = -40;
    TEST_ASSERT_EQUAL(2, tinc_wifi_rank(&s, scan, 2, c));
    TEST_ASSERT_EQUAL(0, c[0].slot);
    TEST_ASSERT_EQUAL(1, c[0].scan);
    TEST_ASSERT_EQUAL(1, c[1].slot);
}

/* ---- UART link ---- */

static void uart_frame(uint8_t type, uint8_t seq, const uint8_t *pl, uint16_t len)
{
    static uint8_t f[TINC_FRAME_BUF(TINC_PAYLOAD_LIMIT)];
    fk_uart_in(f, tinc_frame_encode(f, 0, type, seq, pl, len));
}

/* Parse everything the ESP wrote; returns the frame count, last one in *out. */
static int uart_replies(tinc_parser *out)
{
    static uint8_t buf[TINC_FRAME_BUF(TINC_PAYLOAD_LIMIT)];
    tinc_parser p;
    int n = 0;
    uint16_t i;

    tinc_parser_init(&p, buf, sizeof buf);
    for (i = 0; i < fk_uout_len; i++)
        if (tinc_parser_feed(&p, fk_uout[i]) == TINC_PARSE_FRAME) {
            *out = p;
            n++;
        }
    return n;
}

static void link_steps(int n)
{
    while (n--)
        tinc_link_step();
}

static const uint8_t hello_pl[] = {TINC_PROTO_MAJOR, TINC_PROTO_MINOR, 0, 0, 0, 4};

static void test_link_roundtrip(void)
{
    static const uint8_t boot_noise[] = {0x00, 0xFF, 0x13, 0x37}; /* 74880-baud garbage */
    tinc_parser r;

    fk_uart_in(boot_noise, sizeof boot_noise);
    uart_frame(TINC_T_HELLO, 1, hello_pl, sizeof hello_pl);
    uart_frame(TINC_T_STATUS, 2, NULL, 0);
    link_steps(1);
    TEST_ASSERT_EQUAL(1, uart_replies(&r)); /* one frame per step */
    link_steps(1);
    TEST_ASSERT_EQUAL(2, uart_replies(&r));
    TEST_ASSERT_EQUAL_HEX8(TINC_T_STATUS, r.type);
    TEST_ASSERT_EQUAL_HEX8(TINC_FLAG_RESP, r.flags);
    TEST_ASSERT_EQUAL(TINC_STATUS_RESP_LEN, r.len);
}

static void test_link_interbyte_gap(void)
{
    static uint8_t f[32];
    uint16_t n = tinc_frame_encode(f, 0, TINC_T_HELLO, 1, hello_pl, sizeof hello_pl);
    tinc_parser r;

    fk_uart_in(f, 5); /* a frame cut off mid-header */
    link_steps(1);
    fk_now += TINC_INTERBYTE_RESET_MS + 1;
    fk_uart_in(f, n);
    link_steps(1);
    TEST_ASSERT_EQUAL(1, uart_replies(&r));
    TEST_ASSERT_EQUAL_HEX8(TINC_T_HELLO, r.type);
}

static void test_link_partial_tx(void)
{
    tinc_parser r;

    fk_uart_room = 3;
    uart_frame(TINC_T_HELLO, 1, hello_pl, sizeof hello_pl);
    uart_frame(TINC_T_STATUS, 2, NULL, 0);
    link_steps(1);
    TEST_ASSERT_EQUAL(3, fk_uout_len);
    link_steps(3); /* the HELLO reply (18 bytes) is still draining */
    TEST_ASSERT_EQUAL(0, uart_replies(&r));
    TEST_ASSERT_EQUAL(1, fk_uin_len - fk_uin_pos > 0);
    link_steps(20);
    TEST_ASSERT_EQUAL(2, uart_replies(&r));
    TEST_ASSERT_EQUAL_HEX8(TINC_T_STATUS, r.type);
}

static void test_link_long_poll(void)
{
    uint8_t rd[TINC_READ_REQ_LEN] = {0, 0, 0, 0, 64, 0, 100};
    tinc_parser r;

    hello();
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n");
    uart_frame(TINC_T_BODY_READ, 50, rd, sizeof rd);
    link_steps(3);
    TEST_ASSERT_EQUAL(0, uart_replies(&r)); /* held */

    /* data arrives while held: answered on the next step */
    fk_serve("abcd");
    tinc_poll();
    link_steps(1);
    TEST_ASSERT_EQUAL(1, uart_replies(&r));
    TEST_ASSERT_EQUAL(TINC_READ_DATA + 4, r.len);

    /* a held read ends as soon as another frame is waiting */
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n");
    uart_frame(TINC_T_BODY_READ, 51, rd, sizeof rd);
    link_steps(1);
    uart_frame(TINC_T_REQ_ABORT, 52, NULL, 0);
    link_steps(1);
    TEST_ASSERT_EQUAL(2, uart_replies(&r));
    TEST_ASSERT_EQUAL(TINC_READ_DATA, r.len); /* empty, not EOF */
    link_steps(1);
    TEST_ASSERT_EQUAL(3, uart_replies(&r));
    TEST_ASSERT_EQUAL_HEX8(TINC_T_REQ_ABORT, r.type);
}

/* ---- trace: tinc_describe and the link hook ---- */

static const char *describe(const uint8_t *f, uint16_t n)
{
    static char line[512];
    tinc_describe(f, n, line, sizeof line);
    return line;
}

static void test_describe_golden(void)
{
    int i;

    TEST_ASSERT_EQUAL_STRING("#1 HELLO v0.2 max_payload=256", describe(tv_hello_req, sizeof tv_hello_req));
    TEST_ASSERT_EQUAL_STRING("#1 HELLO ok v0.2 max_payload=1024 heap=28000",
                             describe(tv_hello_resp, sizeof tv_hello_resp));
    TEST_ASSERT_EQUAL_STRING("#2 STATUS wifi=CONNECTED slot=0 rssi=-61 ip=192.168.1.42 heap=27500 req=IDLE wifi-locked",
                             describe(tv_status_resp_locked, sizeof tv_status_resp_locked));
    TEST_ASSERT_EQUAL_STRING("#3 REQ_BEGIN GET http://example.com/api?... transcode (headers: 26 bytes, not shown)",
                             describe(tv_req_begin_req, sizeof tv_req_begin_req));
    TEST_ASSERT_EQUAL_STRING("#4 REQ_STATUS BODY http=200 len=unknown type=application/json",
                             describe(tv_req_status_resp, sizeof tv_req_status_resp));
    TEST_ASSERT_EQUAL_STRING("#5 BODY_READ @0 max=128 wait=50ms", describe(tv_body_read_req, sizeof tv_body_read_req));
    TEST_ASSERT_EQUAL_STRING("#6 BODY_READ @11 6 bytes EOF: \"\\\"n\\\":1}\"",
                             describe(tv_body_read_resp_eof, sizeof tv_body_read_resp_eof));
    TEST_ASSERT_EQUAL_STRING("#9 WIFI_LIST 0=\"HomeNet\" 1=\"\" 2=\"Phone\" (2 hidden)",
                             describe(tv_wifi_list_resp, sizeof tv_wifi_list_resp));
    TEST_ASSERT_EQUAL_STRING("#10 WIFI_SET slot 1 ssid=\"Phone\" (password not shown) hidden",
                             describe(tv_wifi_set_req, sizeof tv_wifi_set_req));
    TEST_ASSERT_EQUAL_STRING("#12 STATUS -> error NO_HELLO", describe(tv_err_no_hello, sizeof tv_err_no_hello));
    TEST_ASSERT_EQUAL_STRING("#13 HELLO -> error VERSION (ESP is v0.3)", describe(tv_err_version, sizeof tv_err_version));
    TEST_ASSERT_EQUAL_STRING("#16 WIFI_SET -> error LOCKED", describe(tv_err_locked, sizeof tv_err_locked));
    TEST_ASSERT_EQUAL_STRING("#14 type 0x13 -> error UNSUPPORTED",
                             describe(tv_err_unsupported, sizeof tv_err_unsupported));

    /* every valid vector: something sensible, and never a secret */
    for (i = 0; i < TINC_VALID_COUNT; i++) {
        const char *s = describe(tinc_valid_vectors[i].data, tinc_valid_vectors[i].len);
        TEST_ASSERT_TRUE_MESSAGE(s[0] == '#', tinc_valid_vectors[i].name);
        TEST_ASSERT_NULL_MESSAGE(strstr(s, "hunter22"), tinc_valid_vectors[i].name);
        TEST_ASSERT_NULL_MESSAGE(strstr(s, "Accept"), tinc_valid_vectors[i].name);
        TEST_ASSERT_NULL_MESSAGE(strstr(s, "q=1"), tinc_valid_vectors[i].name);
    }
    /* truncated input never reads past len */
    for (i = 0; i < (int)sizeof tv_req_begin_req; i++)
        describe(tv_req_begin_req, (uint16_t)i);
    TEST_ASSERT_EQUAL_STRING("(3 bytes, not a frame)", describe(tv_hello_req, 3));
}

static int traced_in, traced_out;
static char traced_last[128];

static void capture_trace(int from_ce, const uint8_t *f, uint16_t n)
{
    if (from_ce)
        traced_in++;
    else
        traced_out++;
    tinc_describe(f, n, traced_last, sizeof traced_last);
}

static void test_link_trace(void)
{
    uint8_t rd[TINC_READ_REQ_LEN] = {0, 0, 0, 0, 64, 0, 100};

    traced_in = traced_out = 0;
    tinc_link_set_trace(capture_trace);
    uart_frame(TINC_T_HELLO, 1, hello_pl, sizeof hello_pl);
    link_steps(1);
    TEST_ASSERT_EQUAL(1, traced_in);
    TEST_ASSERT_EQUAL(1, traced_out);
    TEST_ASSERT_EQUAL_STRING_LEN("#1 HELLO ok", traced_last, 11);

    /* a held BODY_READ is traced once in, once out, however long it waits */
    fetch("http://x/", 0, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n");
    uart_frame(TINC_T_BODY_READ, 2, rd, sizeof rd);
    link_steps(5);
    TEST_ASSERT_EQUAL(2, traced_in);
    TEST_ASSERT_EQUAL(1, traced_out);
    fk_serve("abcd");
    tinc_poll();
    link_steps(1);
    TEST_ASSERT_EQUAL(2, traced_out);
    TEST_ASSERT_EQUAL_STRING("#2 BODY_READ @0 4 bytes EOF: \"abcd\"", traced_last);
    tinc_link_set_trace(NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_session);
    RUN_TEST(test_golden_errors);
    RUN_TEST(test_golden_wifi);
    RUN_TEST(test_golden_wifi_lock);
    RUN_TEST(test_wifi_set_wflags);
    RUN_TEST(test_wifi_rank_hidden);
    RUN_TEST(test_version_mismatch);
    RUN_TEST(test_bad_len);
    RUN_TEST(test_seq_replay_does_not_rerun);
    RUN_TEST(test_begin_validation);
    RUN_TEST(test_begin_replaces_finished_request);
    RUN_TEST(test_headers_never_logged);
    RUN_TEST(test_partial_writes);
    RUN_TEST(test_content_length);
    RUN_TEST(test_chunked_byte_at_a_time);
    RUN_TEST(test_bad_chunk_size);
    RUN_TEST(test_close_delimited);
    RUN_TEST(test_truncated_body);
    RUN_TEST(test_malformed_status);
    RUN_TEST(test_close_before_headers);
    RUN_TEST(test_redirects);
    RUN_TEST(test_redirect_to_https);
    RUN_TEST(test_redirect_limit);
    RUN_TEST(test_no_content);
    RUN_TEST(test_body_read_offsets);
    RUN_TEST(test_body_read_bad_state);
    RUN_TEST(test_long_poll);
    RUN_TEST(test_done_linger);
    RUN_TEST(test_connect_timeout);
    RUN_TEST(test_clock_ticking_during_poll);
    RUN_TEST(test_body_gap_timeout);
    RUN_TEST(test_watchdog);
    RUN_TEST(test_wifi_drop);
    RUN_TEST(test_low_heap);
    RUN_TEST(test_connect_refused);
    RUN_TEST(test_wifi_set_validation);
    RUN_TEST(test_wifi_list_hides_passwords);
    RUN_TEST(test_transcode);
    RUN_TEST(test_transcode_skips_binary);
    RUN_TEST(test_transcode_unit);
    RUN_TEST(test_wifi_rank);
    RUN_TEST(test_wifi_rank_keeps_strongest_when_full);
    RUN_TEST(test_link_roundtrip);
    RUN_TEST(test_link_interbyte_gap);
    RUN_TEST(test_link_partial_tx);
    RUN_TEST(test_link_long_poll);
    RUN_TEST(test_describe_golden);
    RUN_TEST(test_link_trace);
    return UNITY_END();
}
