/* Streaming UTF-8 -> calculator-safe ASCII. */
#include <string.h>
#include "core.h"

/* U+00C0..U+00FF folded to a base letter. */
static const char latin1[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuypy";

static const struct { uint16_t cp; const char *s; } map[] = {
    {0x00A0, " "}, {0x00A9, "(c)"}, {0x00AE, "(R)"}, {0x00B0, "o"},
    {0x00AB, "<<"}, {0x00BB, ">>"}, {0x00B7, "."},
    {0x2010, "-"}, {0x2011, "-"}, {0x2012, "-"}, {0x2013, "-"},
    {0x2014, "-"}, {0x2015, "-"}, {0x2212, "-"},
    {0x2018, "'"}, {0x2019, "'"}, {0x201A, "'"}, {0x2032, "'"},
    {0x201C, "\""}, {0x201D, "\""}, {0x201E, "\""}, {0x2033, "\""},
    {0x2022, "*"}, {0x2026, "..."}, {0x2122, "TM"}, {0x20AC, "EUR"},
    {0x2009, " "}, {0x200B, ""}, {0xFEFF, ""},
};

int tinc_tc_applies(const char *ct)
{
    return strncmp(ct, "text/", 5) == 0 || strstr(ct, "json") || strstr(ct, "xml");
}

static uint8_t emit_cp(uint32_t cp, uint8_t *out)
{
    unsigned i;

    if (cp >= 0xC0 && cp <= 0xFF) {
        out[0] = (uint8_t)latin1[cp - 0xC0];
        return 1;
    }
    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (map[i].cp == cp) {
            uint8_t n = (uint8_t)strlen(map[i].s);
            memcpy(out, map[i].s, n);
            return n;
        }
    }
    out[0] = '?';
    return 1;
}

uint8_t tinc_tc_byte(tinc_tc *t, uint8_t b, uint8_t *out)
{
    uint8_t n = 0;

    if (t->need) {
        if ((b & 0xC0) == 0x80) {
            t->cp = (t->cp << 6) | (b & 0x3F);
            return --t->need ? 0 : emit_cp(t->cp, out);
        }
        out[n++] = '?'; /* truncated sequence; reprocess b */
        t->need = 0;
    }
    if (b < 0x80) {
        out[n++] = b;
    } else if ((b & 0xE0) == 0xC0) {
        t->cp = b & 0x1F; t->need = 1;
    } else if ((b & 0xF0) == 0xE0) {
        t->cp = b & 0x0F; t->need = 2;
    } else if ((b & 0xF8) == 0xF0) {
        t->cp = b & 0x07; t->need = 3;
    } else {
        out[n++] = '?';
    }
    return n;
}

uint8_t tinc_tc_flush(tinc_tc *t, uint8_t *out)
{
    if (!t->need)
        return 0;
    t->need = 0;
    out[0] = '?';
    return 1;
}
