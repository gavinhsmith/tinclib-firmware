/*
 * UART link: bytes -> frames -> dispatcher -> reply, without blocking.
 * One frame is handled per step; the reply buffer is in use until the
 * reply has fully left, so nothing is dispatched while it drains.
 */
#include "tinc_core.h"
#include "tinc_frame.h"

static uint8_t rxbuf[TINC_FRAME_BUF(TINC_PAYLOAD_LIMIT)];
static tinc_parser parser;
static uint32_t last_byte_at;
static uint8_t pending;     /* BODY_READ long-poll in progress */
static const uint8_t *tx;
static uint16_t tx_left;
static tinc_trace_fn trace; /* survives tinc_link_init: set once by the platform */

static void flush(void)
{
    uint16_t w = tinc_plat_uart_write(tx, tx_left);

    tx += w;
    tx_left = (uint16_t)(tx_left - w);
}

static void dispatch(void)
{
    uint16_t n = tinc_dispatch(parser.type, parser.seq, parser.payload, parser.len,
                               tinc_plat_uart_available() > 0, &tx);

    pending = n == TINC_PENDING;
    tx_left = pending ? 0 : n;
    if (tx_left && trace)
        trace(0, tx, tx_left);
    if (tx_left)
        flush();
}

void tinc_link_set_trace(tinc_trace_fn fn)
{
    trace = fn;
}

void tinc_link_init(void)
{
    tinc_parser_init(&parser, rxbuf, sizeof rxbuf);
    pending = 0;
    tx_left = 0;
    last_byte_at = tinc_plat_millis();
}

void tinc_link_step(void)
{
    uint32_t now;

    if (tx_left) {
        flush();
        if (tx_left)
            return; /* the reply buffer is in use until it's sent */
    }
    if (pending) {
        dispatch(); /* the frame is still in the parser: we don't feed it meanwhile */
        return;
    }
    while (tinc_plat_uart_available()) {
        now = tinc_plat_millis();
        if (now - last_byte_at > TINC_INTERBYTE_RESET_MS)
            tinc_parser_reset(&parser);
        last_byte_at = now;
        if (tinc_parser_feed(&parser, tinc_plat_uart_read()) == TINC_PARSE_FRAME) {
            if (trace)
                trace(1, rxbuf, (uint16_t)(TINC_OVERHEAD + parser.len));
            dispatch();
            return; /* the rest wait in the UART buffer */
        }
    }
}
