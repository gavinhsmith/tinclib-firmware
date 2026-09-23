#include <string.h>
#include "tinc_core.h"

uint8_t tinc_wifi_rank(const tinc_slots *s, const tinc_scan *scan, uint8_t n,
                       tinc_cand *out)
{
    uint8_t cnt = 0, i, slot, j;

    for (i = 0; i < n; i++) {
        if (scan[i].rssi < TINC_RSSI_FLOOR || scan[i].enterprise)
            continue;
        for (slot = 0; slot < TINC_WIFI_SLOTS; slot++)
            if (s->ssid[slot][0] && strcmp(s->ssid[slot], scan[i].ssid) == 0)
                break;
        if (slot == TINC_WIFI_SLOTS)
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
    return cnt;
}
