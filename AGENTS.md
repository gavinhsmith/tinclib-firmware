# AGENTS.md — tinclib-firmware

## What this repo is

Firmware for the ESP8266 side of TINCLIB: implements the wire protocol
defined in `tinclib-protocol` and does all the real networking (Wi-Fi, TLS,
HTTP) on behalf of the TI-84 Plus CE calculator, which talks to it over
USB serial through a bridge chip on the dev board.

Consumes `tinclib-protocol` as a pinned dependency (submodule or vendored at
a tagged version — check how the repo actually has it wired before assuming
either). **Never fork or hand-copy `protocol.h`/`crc16.c` into this repo.**
If the shared protocol seems to need a change to support something the
firmware needs, that change belongs in `tinclib-protocol` first, tagged,
then pulled in here.

## Target hardware

- Chip: **ESP8266** (module marking seen so far: ESP8266MOD / AI-Thinker
  style module). Confirm actual flash size per board with `esptool.py
  flash_id` rather than assuming — don't hardcode a flash-size assumption
  into partition/LittleFS layout without checking.
- Carrier boards in use include CH340-bridge and CP2102-bridge boards.
  **The bridge chip is irrelevant to this repo** — it only matters to
  `tinclib` (the CE-side USB host driver). Firmware code should not care
  what USB bridge sits between the calculator and this chip; it only sees
  a UART.
- v1 has no Bluetooth requirement — ESP8266 has no BT/BLE hardware at all,
  so don't add capability bits or code paths implying otherwise. If/when a
  v2 target adds a BLE-capable chip, that's capability-gated via `HELLO`,
  not assumed here.

## Toolchain

- **PlatformIO**, `espressif8266` platform, **Arduino core** (not the bare
  ESP8266 NONOS/RTOS SDK). This was a deliberate choice: Wi-Fi
  scan/join/RSSI (`ESP8266WiFi`), TLS (`BearSSL::WiFiClientSecure` +
  `BearSSL::CertStore`), storage (`LittleFS`), and NTP (`configTime()`) are
  all needed and all already solved well in the Arduino core. Do not
  propose dropping to the bare SDK — that reopens problems that were
  explicitly avoided by this choice.
- A native (PC) PlatformIO environment should also build the
  protocol-core and dispatcher layers, for unit testing without hardware.
  Keep that possible — see Architecture below.

## Architecture — layering is not optional

```
┌─────────────────────────────────────────┐
│ Platform layer (C++, Arduino core)       │  Wi-Fi, WiFiClientSecure/TLS,
│                                           │  LittleFS, UART, NTP
├─────────────────────────────────────────┤
│ Command dispatcher / request state       │  Pure logic. No Arduino calls.
│ machine (C or simple C++)                │  Unit-testable on a PC build.
├─────────────────────────────────────────┤
│ Protocol core (from tinclib-protocol)    │  Plain C99, shared verbatim
│ framing, CRC, message structs            │  with tinclib and the PC tools
└─────────────────────────────────────────┘
```

- The dispatcher/state-machine layer must not directly call Arduino/ESP8266
  APIs. It should sit behind a small platform interface (e.g. function
  pointers or a thin abstraction struct) so it can be compiled and tested
  on a PC, and so a future ESP32 port only requires rewriting the platform
  layer.
- Do not let HTTP/TLS/parsing logic leak into the platform layer "because
  `WiFiClientSecure` is right there." Own the HTTP response parsing
  (status line, headers, chunked decode, content-length) in the dispatcher
  layer — this is required for the pull-based `BODY_READ` semantics and
  the ASCII transcoding feature, neither of which `HTTPClient`'s blocking,
  all-at-once model supports well.

## Non-blocking is a hard requirement

The ESP8266 is single-threaded and cooperative. **No call in the main loop
may block for long.** Long blocks starve Wi-Fi housekeeping and trip the
watchdog. Concretely:
- Drive `WiFiClientSecure` (or a thinner TLS wrapper) directly rather than
  reaching for blocking helpers, so `CONNECTING`/`TLS`/`WAIT_HEADERS` can
  be distinct, advanceable states as required by `REQ_STATUS`.
- The request state machine advances one step per loop iteration, not to
  completion in one call.
- Service the UART every loop iteration — don't let a slow operation
  (e.g. a LittleFS write) delay draining RX for long, especially if/when a
  higher baud rate is negotiated.

## Memory discipline (ESP8266 has ~40-50KB free heap after Wi-Fi)

- Check free heap before starting a TLS session; fail cleanly with
  `ERR_TLS_NO_MEM` rather than let BearSSL fail messily or crash/reboot.
- Avoid `String` in hot paths (header parsing, body streaming) — heap
  fragmentation from repeated String churn causes failures that show up
  much later and are hard to trace back. Prefer fixed buffers sized by
  compile-time constants.
- Consider Maximum Fragment Length Negotiation (MFLN) to shrink BearSSL's
  TLS buffers (down to ~512B–1KB from the 16KB default) where the server
  supports it — this is often the difference between a TLS session fitting
  and `ERR_TLS_NO_MEM`. Not all servers support it; this must degrade
  gracefully, not assume support.

## TLS / certificate policy — do not change without flagging it

Default is **CA bundle in flash via LittleFS + `BearSSL::CertStore`** (root
CA lookup on demand, low RAM cost, works against arbitrary sites). This was
chosen over per-service pinning (breaks on key rotation) and over
insecure-by-default (rejected outright). Rules that follow from this:
- Insecure mode is a **global TINCLIBC-controlled setting**, off by
  default, and a request's `INSECURE` flag only takes effect if that global
  setting is also on. See `tinclib-protocol`'s `ERR_INSECURE_DISABLED`.
- BearSSL on ESP8266 tops out at **TLS 1.2** — a server requiring TLS 1.3
  fails; that's expected and should surface as `ERR_TLS_PROTO`, not a
  crash or a hang.
- Certificate validation requires correct time. Sync NTP right after
  joining Wi-Fi; refuse to validate (return `ERR_TLS_TIME`) before time is
  known good. A `SET_TIME` fallback path (CE pushes its RTC time) was
  discussed as a fallback — confirm current status before assuming it's
  implemented.
- The CA bundle needs a refresh path eventually (likely TINCLIBC-driven).
  Don't hardcode an unrefreshable bundle as a permanent design.

## Wi-Fi behavior

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
  disconnect mid-request returns `ERR_NO_WIFI` to the CE; the app decides
  whether to retry, not the firmware.
- Hidden networks need an explicit per-slot "hidden" flag (set via
  TINCLIBC) and are tried directly, since they don't appear in scans.
- WPA2-Enterprise and captive-portal networks are **out of scope** — detect
  what's detectable in scan results and mark unsupported rather than
  silently failing to connect with no explanation.
- Manual "connect to slot N now" (from TINCLIBC) bypasses the RSSI ranking
  entirely.

## Debug / trace output

- The ESP8266's primary UART is shared with the USB bridge and carries
  garbage boot messages at 74880 baud — this is expected and the frame
  parser's SOF+CRC already discards it; don't try to suppress the
  bootloader's own output.
- Use the secondary UART (TX-only) for firmware debug logging. This is
  also the intended basis for the PC-side "trace mode" stretch goal
  (mirroring frames with timestamps out a separate channel for a PC tool
  to decode) — keep that in mind when designing the logging format, even
  though the PC tool itself lives in a different repo.
- **Never log request headers**, even at debug level — this is where an
  `Authorization` header or API key would leak. If a trace/debug feature
  needs to show headers, it must mask sensitive ones (`Authorization` and
  anything the request marks sensitive) — this rule matters even though
  the PC-side masking logic lives elsewhere; the firmware must not emit
  the raw header text into any log path, full stop.

## Reset / robustness

- No session-ID scheme (see `tinclib-protocol` AGENTS.md) — a fresh
  `HELLO` after any reset is how the CE detects it, via `ERR_NO_HELLO` on
  anything sent before `HELLO`.
- Log the reset reason on boot (brownout vs. watchdog vs. power-on) during
  bring-up/debugging — this matters a lot given the project's LiPo-battery
  power plan and its known brownout risk under Wi-Fi TX bursts. Don't
  strip this out as "just debug noise."
- CE-silence watchdog: free any active request's resources if nothing is
  heard from the CE for ~30s.

## What does NOT belong in this repo

- Bluetooth of any kind (v2, different chip likely required).
- Named HTTP "connection profiles" with stored API keys — that idea was
  explicitly walked back once the 3-slot/no-credential-storage constraint
  was set. Only Wi-Fi SSID+password profiles exist.
- Any admin-command access control scheme beyond what's decided in
  `tinclib-protocol` (currently: open, pending a possible physical-button
  gate — don't invent a software gate here unilaterally).
