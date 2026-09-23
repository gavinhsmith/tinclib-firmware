/*
 * ESP8266 platform layer: UART, Wi-Fi join, non-blocking TCP over lwIP's
 * raw API, LittleFS slot storage, debug log on Serial1 (GPIO2). Implements
 * lib/tinc_core/tinc_platform.h; all protocol logic lives in the core.
 */
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <lwip/dns.h>
#include <lwip/tcp.h>

#include "tinc_core.h"

#define SLOTS_FILE     "/wifi.bin"
#define SLOTS_TMP      "/wifi.tmp"
#define SCAN_MAX       16
#define JOIN_TIMEOUT   15000u /* per candidate */
#define RETRY_AFTER_MS 30000u /* after every candidate failed */

/* ---- debug log ------------------------------------------------------- */

extern "C" void tinc_plat_log(const char *msg)
{
    Serial1.println(msg);
}

extern "C" uint32_t tinc_plat_millis(void) { return millis(); }
extern "C" uint32_t tinc_plat_free_heap(void) { return ESP.getFreeHeap(); }

/* ---- slot storage ---------------------------------------------------- */

static void slots_load(void)
{
    File f = LittleFS.open(SLOTS_FILE, "r");
    tinc_slots tmp;

    if (!f)
        return;
    if (f.read((uint8_t *)&tmp, sizeof tmp) == sizeof tmp) {
        uint8_t i;
        for (i = 0; i < TINC_WIFI_SLOTS; i++) { /* never trust flash for NULs */
            tmp.ssid[i][TINC_SSID_MAX] = 0;
            tmp.pass[i][TINC_PASS_MAX] = 0;
        }
        *tinc_core_slots() = tmp;
    }
    f.close();
}

/* Write-then-rename so a brownout mid-write can't lose the old slots. */
extern "C" int tinc_plat_slots_save(const tinc_slots *s)
{
    File f = LittleFS.open(SLOTS_TMP, "w");
    size_t n;

    if (!f)
        return -1;
    n = f.write((const uint8_t *)s, sizeof *s);
    f.close();
    if (n != sizeof *s || !LittleFS.rename(SLOTS_TMP, SLOTS_FILE))
        return -1;
    return 0;
}

/* ---- Wi-Fi: scan, rank by RSSI, try candidates in order --------------- */

enum { W_START, W_SCANNING, W_TRY, W_WAIT, W_UP, W_FAILED };

static uint8_t ws = W_START;
static tinc_scan scan[SCAN_MAX];
static tinc_cand cand[TINC_CAND_MAX];
static uint8_t n_cand, ci, slot_now = TINC_SLOT_NONE;
static uint32_t ws_at;

static bool have_slots(void)
{
    const tinc_slots *s = tinc_core_slots();
    return s->ssid[0][0] || s->ssid[1][0] || s->ssid[2][0];
}

extern "C" void tinc_plat_wifi_reconnect(void)
{
    WiFi.disconnect();
    slot_now = TINC_SLOT_NONE;
    ws = W_START;
}

extern "C" void tinc_plat_wifi_info(tinc_wifi_info *out)
{
    IPAddress ip = WiFi.localIP();
    uint8_t i;

    out->slot = TINC_SLOT_NONE;
    out->rssi = 0;
    memset(out->ip, 0, 4);
    if (!have_slots()) {
        out->state = TINC_WIFI_NO_CREDS;
    } else if (ws == W_UP) {
        out->state = TINC_WIFI_CONNECTED;
        out->slot = slot_now;
        out->rssi = (int8_t)WiFi.RSSI();
        for (i = 0; i < 4; i++)
            out->ip[i] = ip[i];
    } else {
        out->state = ws == W_FAILED ? TINC_WIFI_FAILED : TINC_WIFI_CONNECTING;
    }
}

static void wifi_step(void)
{
    const tinc_slots *s = tinc_core_slots();
    int r, i;
    wl_status_t st;

    switch (ws) {
    case W_START:
        if (!have_slots())
            return;
        WiFi.scanNetworks(true, false); /* async */
        ws = W_SCANNING;
        break;
    case W_SCANNING:
        r = WiFi.scanComplete();
        if (r == WIFI_SCAN_RUNNING)
            return;
        if (r < 0) {
            ws = W_FAILED;
            ws_at = millis();
            return;
        }
        if (r > SCAN_MAX)
            r = SCAN_MAX; /* ponytail: first 16 results only; fine outside dense venues */
        for (i = 0; i < r; i++) {
            strncpy(scan[i].ssid, WiFi.SSID(i).c_str(), TINC_SSID_MAX);
            scan[i].ssid[TINC_SSID_MAX] = 0;
            memcpy(scan[i].bssid, WiFi.BSSID(i), 6);
            scan[i].rssi = (int8_t)WiFi.RSSI(i);
            scan[i].channel = (uint8_t)WiFi.channel(i);
            /* The ESP8266 SDK doesn't report 802.1X in scan results, so
             * enterprise networks can't be flagged here; they fail to join. */
            scan[i].enterprise = 0;
        }
        WiFi.scanDelete();
        n_cand = tinc_wifi_rank(s, scan, (uint8_t)r, cand);
        ci = 0;
        Serial1.printf("wifi: %d seen, %u candidates\n", r, n_cand);
        ws = W_TRY;
        break;
    case W_TRY:
        if (ci >= n_cand) {
            Serial1.println("wifi: no candidate joined");
            ws = W_FAILED;
            ws_at = millis();
            return;
        }
        {
            const tinc_scan *c = &scan[cand[ci].scan];
            const char *pw = s->pass[cand[ci].slot];
            WiFi.begin(c->ssid, pw[0] ? pw : nullptr, c->channel, c->bssid, true);
        }
        ws = W_WAIT;
        ws_at = millis();
        break;
    case W_WAIT:
        st = WiFi.status();
        if (st == WL_CONNECTED) {
            slot_now = cand[ci].slot;
            Serial1.printf("wifi: up on slot %u, rssi %d\n", slot_now, scan[cand[ci].scan].rssi);
            ws = W_UP;
        } else if (st == WL_CONNECT_FAILED || st == WL_WRONG_PASSWORD ||
                   st == WL_NO_SSID_AVAIL || millis() - ws_at > JOIN_TIMEOUT) {
            WiFi.disconnect();
            ci++;
            ws = W_TRY;
        }
        break;
    case W_UP:
        /* no roaming: rescan only after the link drops */
        if (WiFi.status() != WL_CONNECTED) {
            Serial1.println("wifi: link lost");
            slot_now = TINC_SLOT_NONE;
            ws = W_START;
        }
        break;
    case W_FAILED:
        if (millis() - ws_at > RETRY_AFTER_MS)
            ws = W_START;
        break;
    }
}

/* ---- TCP over lwIP raw API (DNS + connect never block the loop) ------- */

static struct tcp_pcb *pcb;
static uint8_t tstate = TINC_TCP_IDLE;
static struct pbuf *rxq; /* received, not yet read; acked to lwIP as read */
static uint16_t rxq_off;
static uint16_t tport;
static uint32_t tgen; /* ignores DNS answers for a connection since closed */
static ip_addr_t taddr;

static void t_err(void *, err_t)
{
    pcb = nullptr; /* lwIP already freed it */
    tstate = TINC_TCP_ERR_CONNECT;
}

static err_t t_recv(void *, struct tcp_pcb *, struct pbuf *p, err_t)
{
    if (!p) {
        tstate = TINC_TCP_CLOSED;
        return ERR_OK;
    }
    if (rxq)
        pbuf_cat(rxq, p);
    else
        rxq = p;
    return ERR_OK;
}

static err_t t_connected(void *, struct tcp_pcb *, err_t)
{
    tstate = TINC_TCP_OPEN;
    return ERR_OK;
}

static void t_connect(const ip_addr_t *ip)
{
    pcb = tcp_new();
    if (!pcb) {
        tstate = TINC_TCP_ERR_CONNECT;
        return;
    }
    tcp_err(pcb, t_err);
    tcp_recv(pcb, t_recv);
    if (tcp_connect(pcb, ip, tport, t_connected) != ERR_OK) {
        tcp_abort(pcb);
        pcb = nullptr;
        tstate = TINC_TCP_ERR_CONNECT;
    }
}

static void t_dns(const char *, const ip_addr_t *ip, void *arg)
{
    if ((uint32_t)(uintptr_t)arg != tgen || tstate != TINC_TCP_BUSY)
        return;
    if (!ip)
        tstate = TINC_TCP_ERR_DNS;
    else
        t_connect(ip);
}

extern "C" void tinc_plat_tcp_close(void)
{
    tgen++;
    if (pcb) {
        tcp_err(pcb, nullptr);
        tcp_recv(pcb, nullptr);
        if (tcp_close(pcb) != ERR_OK)
            tcp_abort(pcb);
        pcb = nullptr;
    }
    if (rxq) {
        pbuf_free(rxq);
        rxq = nullptr;
    }
    rxq_off = 0;
    tstate = TINC_TCP_IDLE;
}

extern "C" int tinc_plat_tcp_open(const char *host, uint16_t port)
{
    err_t e;

    tinc_plat_tcp_close();
    tport = port;
    tstate = TINC_TCP_BUSY;
    e = dns_gethostbyname(host, &taddr, t_dns, (void *)(uintptr_t)tgen);
    if (e == ERR_OK)
        t_connect(&taddr); /* cached or a literal IP */
    else if (e != ERR_INPROGRESS)
        tstate = TINC_TCP_ERR_DNS;
    return 0;
}

extern "C" uint8_t tinc_plat_tcp_state(void) { return tstate; }

extern "C" uint16_t tinc_plat_tcp_write(const uint8_t *p, uint16_t n)
{
    uint16_t room;

    if (!pcb || tstate != TINC_TCP_OPEN)
        return 0;
    room = tcp_sndbuf(pcb);
    if (n > (uint16_t)room)
        n = room;
    if (!n || tcp_write(pcb, p, n, TCP_WRITE_FLAG_COPY) != ERR_OK)
        return 0;
    tcp_output(pcb);
    return n;
}

extern "C" uint16_t tinc_plat_tcp_read(uint8_t *p, uint16_t n)
{
    uint16_t got = 0, take;

    while (got < n && rxq) {
        take = (uint16_t)(rxq->len - rxq_off);
        if (take > n - got)
            take = (uint16_t)(n - got);
        memcpy(p + got, (const uint8_t *)rxq->payload + rxq_off, take);
        got = (uint16_t)(got + take);
        rxq_off = (uint16_t)(rxq_off + take);
        if (rxq_off == rxq->len) { /* drop the head pbuf, keep the rest */
            struct pbuf *head = rxq;
            rxq = rxq->next;
            if (rxq)
                pbuf_ref(rxq);
            pbuf_free(head);
            rxq_off = 0;
        }
    }
    if (got && pcb)
        tcp_recved(pcb, got); /* reopen the window only as the CE drains */
    return got;
}

/* ---- UART link to the CE (framing lives in the core) ----------------- */

extern "C" uint16_t tinc_plat_uart_available(void) { return (uint16_t)Serial.available(); }
extern "C" uint8_t tinc_plat_uart_read(void) { return (uint8_t)Serial.read(); }

/* Write only what fits in the UART FIFO; never wait on it. */
extern "C" uint16_t tinc_plat_uart_write(const uint8_t *p, uint16_t n)
{
    int room = Serial.availableForWrite();

    if (room <= 0)
        return 0;
    if (n > (uint16_t)room)
        n = (uint16_t)room;
    return (uint16_t)Serial.write(p, n);
}

/* ---- entry points ---------------------------------------------------- */

void setup()
{
    Serial.setRxBufferSize(2048);
    Serial.begin(TINC_BAUD_DEFAULT);
    Serial1.begin(115200);
    /* Brownout vs watchdog vs power-on matters on LiPo; keep this. */
    Serial1.printf("\nboot: %s\n", ESP.getResetReason().c_str());

    WiFi.persistent(false); /* slots live in LittleFS, not SDK flash */
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_STA);

    tinc_core_init();
    if (LittleFS.begin())
        slots_load();
    else
        Serial1.println("fs: mount failed, slots not loaded");
    Serial1.printf("heap: %u\n", ESP.getFreeHeap());
}

void loop()
{
    tinc_link_step();
    tinc_poll();
    wifi_step();
}
