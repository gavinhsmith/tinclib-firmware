/*
 * What the core needs from a chip. Each platforms/<chip>/ implements these;
 * test/fake_platform.h implements them on a PC. Nothing here may block.
 */
#ifndef TINC_PLATFORM_H
#define TINC_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tinc_slots tinc_slots;

typedef struct {
    uint8_t state;   /* TINC_WIFI_* */
    uint8_t slot;    /* TINC_SLOT_NONE when not connected */
    int8_t rssi;
    uint8_t ip[4];
} tinc_wifi_info;

enum {
    TINC_TCP_IDLE = 0,
    TINC_TCP_BUSY,        /* resolving or connecting */
    TINC_TCP_OPEN,
    TINC_TCP_CLOSED,      /* peer closed; buffered bytes are still readable */
    TINC_TCP_ERR_DNS,
    TINC_TCP_ERR_CONNECT  /* refused, reset or aborted */
};

uint32_t tinc_plat_millis(void);
uint32_t tinc_plat_free_heap(void);

/* Link UART to the CE. write takes what fits in the TX buffer right now
 * and returns that count. */
uint16_t tinc_plat_uart_available(void);
uint8_t tinc_plat_uart_read(void);
uint16_t tinc_plat_uart_write(const uint8_t *p, uint16_t n);

void tinc_plat_wifi_info(tinc_wifi_info *out);
void tinc_plat_wifi_reconnect(void);
int tinc_plat_slots_save(const tinc_slots *s); /* 0 on success */

/* One connection at a time. tcp_open starts DNS + connect and returns at
 * once (nonzero if it could not even start). read/write never block and
 * return the byte count moved. */
int tinc_plat_tcp_open(const char *host, uint16_t port);
uint8_t tinc_plat_tcp_state(void);
uint16_t tinc_plat_tcp_write(const uint8_t *p, uint16_t n);
uint16_t tinc_plat_tcp_read(uint8_t *p, uint16_t n);
void tinc_plat_tcp_close(void);

/* Debug line. The core never passes request headers here. */
void tinc_plat_log(const char *msg);

#ifdef __cplusplus
}
#endif

#endif
