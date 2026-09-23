#include <string.h>
#include "tinc_core.h"

uint8_t tinc_wifi_rank(const tinc_slots *s, const tinc_scan *scan, uint8_t n,
                       tinc_cand *out)
{
    uint8_t cnt = 0, i, slot, j;

    for (i = 0; i < n; i++) {
        if (scan[i].rssi < TINC_RSSI_FLOOR || scan[i].enterprise)
            continue;
        for (slot = 0; slot < TINC_SLOT_COUNT; slot++)
            if (s->ssid[slot][0] && strcmp(s->ssid[slot], scan[i].ssid) == 0)
                break;
        if (slot == TINC_SLOT_COUNT)
            continue;
        /* insertion sort, strongest first; drop the weakest when full */
        if (cnt == TINC_CAND_MAX) {
            if (scan[out[cnt - 1].scan].rssi >= scan[i].rssi)
                continue;
            j = cnt - 1;
        } else {
            j = cnt++;
        }
        while (j > 0 && scan[out[j - 1].scan].rssi < scan[i].rssi) {
            out[j] = out[j - 1];
            j--;
        }
        out[j].slot = slot;
        out[j].scan = i;
    }

    /* A hidden slot never shows in a scan (unless it happens to answer
     * someone's probe), so try it directly after everything that did. */
    for (slot = 0; slot < TINC_SLOT_COUNT; slot++) {
        if (!s->ssid[slot][0] || !(s->wflags[slot] & TINC_WF_HIDDEN))
            continue;
        for (j = 0; j < cnt && out[j].slot != slot; j++)
            ;
        if (j < cnt)
            continue; /* seen after all: already a ranked candidate */
        out[cnt].slot = slot;
        out[cnt].scan = TINC_CAND_DIRECT;
        cnt++;
    }
    return cnt;
}
