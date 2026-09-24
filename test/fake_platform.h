/* PC stand-in for the platform layer: scripted TCP, fake clock, in-memory
 * slots. Include from exactly one translation unit. */
#ifndef FAKE_PLATFORM_H
#define FAKE_PLATFORM_H

#include <string.h>
#include "tinc_core.h"

static uint32_t fk_now;
static int fk_tick; /* advance the clock 1 ms on every read, like a real 1 ms timer */
static uint32_t fk_heap;
static tinc_wifi_info fk_wifi;
static int fk_reconnects, fk_saves, fk_save_fail;

static uint8_t fk_tcp;
static char fk_host[64];
static uint16_t fk_port;
static int fk_opens;
static uint32_t fk_time;                   /* 0 = clock not set */
static uint8_t fk_tls_ret;                 /* what tls_start returns */
static uint8_t fk_tls_err, fk_tls_detail;  /* reported after TINC_TCP_ERR_TLS */
static int fk_tls_starts;
static char fk_tls_host[64];
static char fk_sent[4096];
static uint16_t fk_sent_len, fk_write_max;
static uint8_t fk_rx[8192];
static uint16_t fk_rx_len, fk_rx_pos, fk_read_max;
static char fk_log[8192];
static uint8_t fk_uin[4096], fk_uout[4096];
static uint16_t fk_uin_len, fk_uin_pos, fk_uout_len, fk_uart_room;

static void fk_reset(void)
{
    fk_now = 1000;
    fk_tick = 0;
    fk_heap = 28000;
    memset(&fk_wifi, 0, sizeof fk_wifi);
    fk_wifi.state = TINC_WIFI_CONNECTED;
    fk_reconnects = fk_saves = fk_save_fail = 0;
    fk_tcp = TINC_TCP_IDLE;
    fk_host[0] = 0;
    fk_opens = 0;
    fk_time = 1760000000u;
    fk_tls_ret = TINC_OK;
    fk_tls_err = TINC_ERR_TLS;
    fk_tls_detail = TINC_TLSR_OTHER;
    fk_tls_starts = 0;
    fk_tls_host[0] = 0;
    fk_sent_len = 0;
    fk_write_max = 0xFFFF;
    fk_rx_len = fk_rx_pos = 0;
    fk_read_max = 0xFFFF;
    fk_log[0] = 0;
    fk_uin_len = fk_uin_pos = fk_uout_len = 0;
    fk_uart_room = 0xFFFF;
}

/* CE -> ESP bytes */
static void fk_uart_in(const uint8_t *p, uint16_t n)
{
    memcpy(fk_uin + fk_uin_len, p, n);
    fk_uin_len = (uint16_t)(fk_uin_len + n);
}

uint16_t tinc_plat_uart_available(void) { return (uint16_t)(fk_uin_len - fk_uin_pos); }
uint8_t tinc_plat_uart_read(void) { return fk_uin[fk_uin_pos++]; }

uint16_t tinc_plat_uart_write(const uint8_t *p, uint16_t n)
{
    if (n > fk_uart_room)
        n = fk_uart_room;
    memcpy(fk_uout + fk_uout_len, p, n);
    fk_uout_len = (uint16_t)(fk_uout_len + n);
    return n;
}

/* server -> ESP bytes */
static void fk_serve(const char *s)
{
    uint16_t n = (uint16_t)strlen(s);
    memcpy(fk_rx + fk_rx_len, s, n);
    fk_rx_len = (uint16_t)(fk_rx_len + n);
}

uint32_t tinc_plat_millis(void) { return fk_tick ? fk_now++ : fk_now; }
uint32_t tinc_plat_free_heap(void) { return fk_heap; }
uint32_t tinc_plat_time(void) { return fk_time; }
void tinc_plat_wifi_info(tinc_wifi_info *out) { *out = fk_wifi; }
void tinc_plat_wifi_reconnect(void) { fk_reconnects++; }

int tinc_plat_slots_save(const tinc_slots *s)
{
    (void)s;
    fk_saves++;
    return fk_save_fail;
}

int tinc_plat_tcp_open(const char *host, uint16_t port)
{
    strncpy(fk_host, host, sizeof fk_host - 1);
    fk_port = port;
    fk_opens++;
    fk_tcp = TINC_TCP_BUSY;
    fk_sent_len = 0;
    fk_rx_len = fk_rx_pos = 0;
    return 0;
}

uint8_t tinc_plat_tcp_state(void) { return fk_tcp; }

uint16_t tinc_plat_tcp_write(const uint8_t *p, uint16_t n)
{
    if (n > fk_write_max)
        n = fk_write_max;
    memcpy(fk_sent + fk_sent_len, p, n);
    fk_sent_len = (uint16_t)(fk_sent_len + n);
    fk_sent[fk_sent_len] = 0;
    return n;
}

uint16_t tinc_plat_tcp_read(uint8_t *p, uint16_t n)
{
    uint16_t left = (uint16_t)(fk_rx_len - fk_rx_pos);
    if (n > left)
        n = left;
    if (n > fk_read_max)
        n = fk_read_max;
    memcpy(p, fk_rx + fk_rx_pos, n);
    fk_rx_pos = (uint16_t)(fk_rx_pos + n);
    return n;
}

void tinc_plat_tcp_close(void) { fk_tcp = TINC_TCP_IDLE; }

uint8_t tinc_plat_tls_start(const char *host)
{
    strncpy(fk_tls_host, host, sizeof fk_tls_host - 1);
    fk_tls_starts++;
    if (fk_tls_ret == TINC_OK)
        fk_tcp = TINC_TCP_TLS;
    return fk_tls_ret;
}

uint8_t tinc_plat_tls_err(uint8_t *detail)
{
    *detail = fk_tls_detail;
    return fk_tls_err;
}

void tinc_plat_log(const char *msg)
{
    if (strlen(fk_log) + strlen(msg) + 2 < sizeof fk_log) {
        strcat(fk_log, msg);
        strcat(fk_log, "\n");
    }
}

#endif
