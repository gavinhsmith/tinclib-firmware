# AGENTS.md — tinclib-firmware

## What this repo is

Firmware for the network co-processor side of TINCLIB: implements the wire
protocol defined in `tinclib-protocol` and does all the real networking
(Wi-Fi, TLS, HTTP) on behalf of the TI-84 Plus CE calculator, which talks to
it over USB serial through a bridge chip on the dev board.

The repo holds **one chip-independent core plus one platform layer per
chip**. ESP8266 is the first (and currently only) target; ESP32 is next.
Everything protocol- or HTTP-shaped belongs in the core so every chip gets
it for free; a platform layer only adapts one chip's APIs to the core's
interface.

Consumes `tinclib-protocol` as a **git submodule** at
`external/tinclib-protocol`, pinned to a release tag (currently `v0.1.0`),
and compiled in place by `lib/tinc_core/proto.c`. **Never fork or hand-copy
`protocol.h`/`crc16.c`/`tinc_frame.c` into this repo.** If the shared
protocol seems to need a change to support something the firmware needs,
that change belongs in `tinclib-protocol` first, tagged, then pulled in
here by moving the submodule to the new tag.

## Layout

```
lib/tinc_core/            chip-independent core, C99, no Arduino/SDK calls
  tinc_core.h             core API (tinc_core_init, tinc_link_step, tinc_poll, ...)
  tinc_platform.h         the interface every chip must implement
  link.c                  UART framing, long-poll hold, non-blocking reply drain
  dispatch.c              HELLO gate, SEQ reply cache, message handlers
  req.c                   request state machine + HTTP response parsing
  transcode.c, wifi_rank.c, proto.c
platforms/<chip>/         one platform layer per chip (platforms/esp8266/ today)
external/tinclib-protocol pinned protocol submodule
test/test_core/           native Unity tests of the core (incl. golden vectors)
test/fake_platform.h      PC implementation of tinc_platform.h for tests/fuzz
test/fuzz/                libFuzzer target for the dispatcher
```

Adding a chip: create `platforms/<chip>/` implementing `tinc_platform.h`,
add `[env:<chip>]` to `platformio.ini` with `build_src_filter = +<<chip>/>`,
and add the chip to the CI `build` matrix. The core and its tests should not
need to change; if a chip needs something the interface doesn't offer,
extend `tinc_platform.h` and `test/fake_platform.h` together.

## Target hardware

- **ESP8266** (`platforms/esp8266/`, env `esp8266`, board `esp12e`). Module
  marking seen so far: ESP8266MOD / AI-Thinker style module.
- **ESP32** — planned, not started. Pick the exact variant/board when the
  port begins; don't assume one here.
- Confirm actual flash size per board (`esptool.py flash_id`) rather than
  assuming — don't hardcode a flash-size assumption into partition/LittleFS
  layout without checking.
- Carrier boards in use include CH340-bridge and CP2102-bridge boards.
  **The bridge chip is irrelevant to this repo** — it only matters to
  `tinclib` (the CE-side USB host driver). Firmware code should not care
  what USB bridge sits between the calculator and the chip; it only sees
  a UART.
- v1 has no Bluetooth requirement. ESP8266 has no BT/BLE hardware at all;
  ESP32 does, but v1 still doesn't use it — don't add capability bits or
  code paths implying otherwise. If/when BLE is added, that's
  capability-gated via `HELLO`, not assumed here.

## Toolchain

- **PlatformIO** with the **Arduino core** for each chip (`espressif8266`
  today; `espressif32` for the ESP32 port), not the bare vendor SDK. This
  was a deliberate choice: Wi-Fi scan/join/RSSI, TLS (`WiFiClientSecure`),
  storage (`LittleFS`), and NTP (`configTime()`) are all needed and all
  already solved well in the Arduino cores. Do not propose dropping to the
  bare SDK (NONOS/RTOS SDK or plain ESP-IDF) — that reopens problems that
  were explicitly avoided by this choice.
- The `native` PlatformIO environment builds and tests the core on a PC
  (`pio test -e native`). Keep that possible — see Architecture below.
- Pin platform versions in `platformio.ini` (e.g. `espressif8266@4.2.1`) so
  builds are reproducible.
- On Windows, run `pio` from PowerShell/cmd: MSYS2's gcc fails silently
  under Git Bash.

## Architecture — layering is not optional

```
┌─────────────────────────────────────────┐
│ Platform layer, per chip                 │  platforms/<chip>/: Wi-Fi, TCP/TLS,
│ (C++, Arduino core)                      │  LittleFS, UART, NTP, debug log
├──────────── tinc_platform.h ────────────┤
│ Core: link, dispatcher, request state    │  lib/tinc_core/: pure C99.
│ machine, HTTP parsing, transcoding       │  No Arduino calls. Tested on a PC.
├─────────────────────────────────────────┤
│ Protocol (from tinclib-protocol)         │  Plain C99, shared verbatim
│ framing, CRC, payload layouts            │  with tinclib and the PC tools
└─────────────────────────────────────────┘
```

- The core must not call Arduino or chip SDK APIs. It reaches the chip only
  through `tinc_platform.h` (link-time `tinc_plat_*` functions), so it can
  be compiled and tested on a PC and so a new chip only requires writing a
  platform layer.
- Keep platform layers thin. Anything that isn't a chip API adaptation —
  framing, protocol rules, HTTP semantics, Wi-Fi candidate ranking — goes
  in the core, not copied between `platforms/*`.
- Do not let HTTP/TLS/parsing logic leak into the platform layer "because
  `WiFiClientSecure` is right there." Own the HTTP response parsing
  (status line, headers, chunked decode, content-length) in the core — this
  is required for the pull-based `BODY_READ` semantics and the ASCII
  transcoding feature, neither of which `HTTPClient`'s blocking,
  all-at-once model supports well.

## Non-blocking is a hard requirement

**No call in the main loop may block for long**, on any chip. On the
ESP8266 (single-threaded, cooperative) long blocks starve Wi-Fi
housekeeping and trip the watchdog; on every chip they stall the UART and
make the CE's 200 ms reply timeout fire. Concretely:
- DNS, TCP connect, TLS handshake, UART writes and Wi-Fi joins must all
  make progress one loop iteration at a time. `WiFiClient::connect()` is
  blocking; the ESP8266 layer uses lwIP's raw API instead. Drive TLS the
  same way (not via blocking helpers), so `CONNECTING`/`TLS`/`WAIT_HEADERS`
  stay distinct, advanceable states as required by `REQ_STATUS`.
- The request state machine advances one step per loop iteration, not to
  completion in one call.
- Service the UART every loop iteration — don't let a slow operation
  (e.g. a LittleFS write) delay draining RX for long, especially if/when a
  higher baud rate is negotiated.
- ESP32: lwIP runs in its own FreeRTOS task, so raw-API calls from the
  Arduino loop must hold the TCP/IP core lock (`LOCK_TCPIP_CORE()`), and
  callbacks run concurrently with the loop — shared state between them
  needs care that the ESP8266 layer doesn't.

## Memory discipline

The ESP8266 has ~40-50KB free heap after Wi-Fi; ESP32 has much more, but
the core is sized for the smallest target and must stay that way.
- Check free heap before starting a connection or TLS session; fail cleanly
  (`ERR_NO_MEM` today, `ERR_TLS_NO_MEM` once TLS lands) rather than let the
  TLS library fail messily or crash/reboot.
- Avoid `String` in hot paths (header parsing, body streaming) — heap
  fragmentation from repeated String churn causes failures that show up
  much later and are hard to trace back. Prefer fixed buffers sized by
  compile-time constants.
- ESP8266: consider Maximum Fragment Length Negotiation (MFLN) to shrink
  BearSSL's TLS buffers (down to ~512B–1KB from the 16KB default) where the
  server supports it — this is often the difference between a TLS session
  fitting and `ERR_TLS_NO_MEM`. Not all servers support it; this must
  degrade gracefully, not assume support.

## TLS / certificate policy — do not change without flagging it

TLS arrives with a later protocol minor version (0.1 is plain HTTP only).
The policy is chip-independent; the library under it differs per chip
(BearSSL on ESP8266, mbedTLS on ESP32).

Default is **CA bundle in flash via LittleFS** with root CA lookup on
demand (`BearSSL::CertStore` on ESP8266; the equivalent CA-bundle mechanism
on ESP32) — low RAM cost, works against arbitrary sites. This was chosen
over per-service pinning (breaks on key rotation) and over
insecure-by-default (rejected outright). Rules that follow from this:
- Insecure mode is a **global TINCLIBC-controlled setting**, off by
  default, and a request's `INSECURE` flag only takes effect if that global
  setting is also on. See `tinclib-protocol`'s `ERR_INSECURE_DISABLED`.
- BearSSL on ESP8266 tops out at **TLS 1.2** — a server requiring TLS 1.3
  fails; that's expected and should surface as `ERR_TLS_PROTO`, not a
  crash or a hang. Don't assume ESP32 has the same limit, or that it
  doesn't — check the mbedTLS build in use.
- Certificate validation requires correct time. Sync NTP right after
  joining Wi-Fi; refuse to validate (return `ERR_TLS_TIME`) before time is
  known good. A `SET_TIME` fallback path (CE pushes its RTC time) was
  discussed as a fallback — confirm current status before assuming it's
  implemented.
- The CA bundle needs a refresh path eventually (likely TINCLIBC-driven).
  Don't hardcode an unrefreshable bundle as a permanent design.

## Wi-Fi behavior

Candidate ranking lives in the core (`wifi_rank.c`); each platform layer
runs the scan/join state machine with its chip's Wi-Fi API.
- **3 saved profiles max**, SSID + password, write-only passwords (no
  command may read one back — enforce this in the dispatcher, not just by
  omission in the API).
- **Boot/reconnect: scan, match saved SSIDs, pick the strongest RSSI
  match, try it, fall through to the next match on failure.** This was a
  deliberate choice over priority-by-slot-order — don't revert to
  slot-order without cause.
- Rank by BSSID as well as SSID for mesh/multi-AP networks — joining "the
  strongest AP broadcasting this SSID," not just the first match found.
- Ignore scan results below an RSSI floor (~-85 dBm) — a marginal signal
  usually fails to associate anyway and isn't worth the timeout.
- No roaming while connected — only rescan after a disconnect. A
  disconnect mid-request returns `ERR_WIFI_DOWN` to the CE; the app
  decides whether to retry, not the firmware.
- Hidden networks need an explicit per-slot "hidden" flag (set via
  TINCLIBC) and are tried directly, since they don't appear in scans.
  (Not in protocol 0.1's `WIFI_SET` yet.)
- WPA2-Enterprise and captive-portal networks are **out of scope** — detect
  what's detectable in scan results and mark unsupported rather than
  silently failing to connect with no explanation. (The ESP8266 SDK doesn't
  expose 802.1X in scan results; ESP32's auth mode does — use it there.)
- Manual "connect to slot N now" (from TINCLIBC) bypasses the RSSI ranking
  entirely.

## Debug / trace output

- The link UART is shared with the USB bridge and carries the ROM
  bootloader's boot messages (74880 baud on ESP8266). This is expected and
  the frame parser's SOF+CRC already discards it; don't try to suppress
  the bootloader's own output.
- Use a secondary UART for firmware debug logging (ESP8266: Serial1,
  TX-only on GPIO2). The core logs only through `tinc_plat_log()`. This is
  also the intended basis for the PC-side "trace mode" stretch goal
  (mirroring frames with timestamps out a separate channel for a PC tool
  to decode) — keep that in mind when designing the logging format, even
  though the PC tool itself lives in a different repo.
- **Never log request headers**, even at debug level — this is where an
  `Authorization` header or API key would leak. If a trace/debug feature
  needs to show headers, it must mask sensitive ones (`Authorization` and
  anything the request marks sensitive) — this rule matters even though
  the PC-side masking logic lives elsewhere; the firmware must not emit
  the raw header text into any log path, full stop. URLs can carry keys
  in query strings too; the core doesn't log them either.

## Reset / robustness

- No session-ID scheme (see `tinclib-protocol` AGENTS.md) — a fresh
  `HELLO` after any reset is how the CE detects it, via `ERR_NO_HELLO` on
  anything sent before `HELLO`.
- Log the reset reason on boot (brownout vs. watchdog vs. power-on) on
  every chip during bring-up/debugging — this matters a lot given the
  project's LiPo-battery power plan and its known brownout risk under
  Wi-Fi TX bursts. Don't strip this out as "just debug noise."
- CE-silence watchdog: free any active request's resources if nothing is
  heard from the CE for ~30s (in the core: `tinc_poll()`).

## Testing and CI

- Core behavior is tested natively (`pio test -e native`) against
  `test/fake_platform.h`, including every applicable golden vector from
  `tinclib-protocol`. New core logic comes with a test there.
- CI runs the native tests, fuzzes the dispatcher and the protocol's frame
  parser, and builds every chip in the `build` matrix; tagged releases
  attach `tinclib-firmware-<chip>.bin` per chip.

## What does NOT belong in this repo

- Bluetooth of any kind in v1, even on chips that have it.
- Named HTTP "connection profiles" with stored API keys — that idea was
  explicitly walked back once the 3-slot/no-credential-storage constraint
  was set. Only Wi-Fi SSID+password profiles exist.
- Any admin-command access control scheme beyond what's decided in
  `tinclib-protocol` (currently: open, pending a possible physical-button
  gate — don't invent a software gate here unilaterally).
- Chip-specific copies of core logic. If two `platforms/*` need the same
  non-API code, it goes in the core.
