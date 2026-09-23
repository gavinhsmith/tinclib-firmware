/*
 * Firmware core: dispatcher, request state machine, HTTP parsing,
 * transcoding and Wi-Fi ranking. Pure C, no Arduino calls; see AGENTS.md.
 */
#ifndef TINC_CORE_H
#define TINC_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"
#include "tinc_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tinc_slots {
    char ssid[TINC_WIFI_SLOTS][TINC_SSID_MAX + 1]; /* "" = empty slot */
    char pass[TINC_WIFI_SLOTS][TINC_PASS_MAX + 1]; /* write-only on the wire */
    uint8_t wflags[TINC_WIFI_SLOTS];               /* TINC_WF_*; last, so 0.1 slot files still load */
};

/* ---- entry points for a platform's main loop ---- */

/* The platform fills tinc_core_slots() from flash after init. */
void tinc_core_init(void);
tinc_slots *tinc_core_slots(void);

/* Once per loop: drain the UART, frame, dispatch, send the reply. */
void tinc_link_step(void);

/* Once per loop: advance the request one step, run the watchdogs. */
void tinc_poll(void);

/* Optional: called with every whole frame the link receives (from_ce = 1)
 * and every reply it sends (from_ce = 0). NULL turns tracing off. */
typedef void (*tinc_trace_fn)(int from_ce, const uint8_t *frame, uint16_t len);
void tinc_link_set_trace(tinc_trace_fn fn);

/* One-line, human-readable description of a frame, for traces. Never shows
 * header text, Wi-Fi passwords or URL query strings. */
void tinc_describe(const uint8_t *frame, uint16_t len, char *buf, size_t cap);

/* ---- dispatcher (used by the link; exposed for tests) ---- */

#define TINC_PENDING 0xFFFFu

void tinc_link_init(void); /* called by tinc_core_init */

/* Handle one parsed frame. Returns the reply frame length, written to
 * *reply, or TINC_PENDING while a BODY_READ is long-polling: call again
 * with the same frame (don't feed the parser meanwhile), passing final=1
 * as soon as any UART byte is waiting. */
uint16_t tinc_dispatch(uint8_t type, uint8_t seq, const uint8_t *pl,
                       uint16_t len, int final, const uint8_t **reply);

/* ---- request (used by the dispatcher; exposed for tests) ---- */

uint8_t tinc_req_begin(uint8_t flags, uint8_t timeout_s,
                       const char *url, uint16_t url_len,
                       const char *hdrs, uint16_t hdr_len);
void tinc_req_release(void);
void tinc_req_poll(void);
uint8_t tinc_req_state(void);
uint8_t tinc_req_err(void);
uint16_t tinc_req_http_status(void);
uint32_t tinc_req_content_len(void);
const char *tinc_req_ctype(void);
/* Fill up to cap decoded body bytes; *eof set once the body is complete. */
uint16_t tinc_req_read(uint8_t *out, uint16_t cap, int *eof);
void tinc_req_mark_done(void);

/* ---- ASCII transcoding ---- */

#define TINC_TC_MAX_OUT 4 /* most bytes one input byte can produce */

typedef struct {
    uint32_t cp;
    uint8_t need;
} tinc_tc;

int tinc_tc_applies(const char *ctype);
/* Returns bytes written to out (<= TINC_TC_MAX_OUT). */
uint8_t tinc_tc_byte(tinc_tc *t, uint8_t b, uint8_t *out);
uint8_t tinc_tc_flush(tinc_tc *t, uint8_t *out);

/* ---- Wi-Fi candidate ranking ---- */

#define TINC_RSSI_FLOOR (-85)
#define TINC_CAND_MAX 8

typedef struct {
    char ssid[TINC_SSID_MAX + 1];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t enterprise;
} tinc_scan;

typedef struct {
    uint8_t slot;
    uint8_t scan; /* index into the scan array, or TINC_CAND_DIRECT */
} tinc_cand;

#define TINC_CAND_DIRECT 0xFFu /* hidden slot not seen in the scan: join by SSID alone */
#define TINC_CAND_CAP (TINC_CAND_MAX + TINC_WIFI_SLOTS) /* size of the out array */

/* Saved-SSID matches above the RSSI floor, strongest BSSID first; then any
 * hidden slot the scan didn't show, to be tried directly. out holds
 * TINC_CAND_CAP entries. */
uint8_t tinc_wifi_rank(const tinc_slots *s, const tinc_scan *scan, uint8_t n,
                       tinc_cand *out);

#ifdef __cplusplus
}
#endif

#endif
