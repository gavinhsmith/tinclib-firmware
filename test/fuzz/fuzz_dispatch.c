/* libFuzzer target for the dispatcher and request state machine, with a
 * fuzzed HTTP response on the fake socket. Frames skip the CRC layer (the
 * parser itself is fuzzed upstream by tinclib-protocol's fuzz_frame.c).
 *   input = [tcp_state u8][server_len u8][server bytes]
 *           then frames: [type sel u8][seq u8][len u8][payload]
 * clang -g -fsanitize=fuzzer,address,undefined -Iexternal/tinclib-protocol \
 *   -Ilib/tinc_core -Itest test/fuzz/fuzz_dispatch.c lib/tinc_core/[a-z]*.c */
#include <stddef.h>
#include "fake_platform.h"
#include "tinc_frame.h"

static const uint8_t types[] = {
    TINC_T_HELLO, TINC_T_STATUS, TINC_T_REQ_BEGIN, TINC_T_REQ_STATUS,
    TINC_T_REQ_ABORT, TINC_T_BODY_READ, TINC_T_WIFI_GET, TINC_T_WIFI_SET,
    TINC_T_WIFI_FORGET
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint8_t hello[] = {TINC_PROTO_MAJOR, TINC_PROTO_MINOR, 0, 0, 0, 4};
    const uint8_t *rep;
    size_t i, srv, len;
    uint8_t type;
    uint16_t n;

    (void)fk_serve;
    (void)fk_uart_in;
    if (size < 2)
        return 0;
    fk_reset();
    tinc_core_init();
    tinc_dispatch(TINC_T_HELLO, 0, hello, sizeof hello, 1, &rep);

    srv = data[1] < size - 2 ? data[1] : size - 2;
    for (i = 2 + srv; i + 3 <= size; i += 3 + len) {
        type = data[i] & 0x80 ? data[i] : types[data[i] % sizeof types];
        len = data[i + 2] < size - i - 3 ? data[i + 2] : size - i - 3;
        n = tinc_dispatch(type, data[i + 1], data + i + 3, (uint16_t)len, data[i + 1] & 1, &rep);
        /* every reply must be a well-formed frame within the limit */
        if (n != TINC_PENDING &&
            (n < TINC_OVERHEAD || n > TINC_FRAME_BUF(TINC_PAYLOAD_LIMIT) ||
             (uint16_t)(tinc_get_u16(rep + 4) + TINC_OVERHEAD) != n))
            __builtin_trap();
        /* once a request is out, let the "server" answer */
        if (fk_tcp == TINC_TCP_BUSY) {
            fk_tcp = data[0] % 6;
            memcpy(fk_rx, data + 2, srv);
            fk_rx_len = (uint16_t)srv;
        }
        fk_now += data[i + 1];
        tinc_poll();
        tinc_poll();
    }
    return 0;
}
