/*
 * https for the platforms that use BearSSL (ESP8266, PC): implements the
 * tinc_plat_tcp_* and tinc_plat_tls_* functions of tinc_platform.h on top of
 * a platform's plain TCP, which it provides as tinc_raw_* below (same
 * contract as the tinc_plat_tcp_* functions they stand in for).
 *
 * Needs the generated CA table (tools/gen_roots.py as a pre: extra_script).
 */
#ifndef TINC_TLS_H
#define TINC_TLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int tinc_raw_open(const char *host, uint16_t port);
uint8_t tinc_raw_state(void);
uint16_t tinc_raw_write(const uint8_t *p, uint16_t n);
uint16_t tinc_raw_read(uint8_t *p, uint16_t n);
/* Bytes received but not read yet (they may still hold TLS records). */
int tinc_raw_pending(void);
void tinc_raw_close(void);

#ifdef __cplusplus
}
#endif

#endif
