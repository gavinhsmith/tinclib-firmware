# tinclib-firmware

Firmware for the network co-processor that TINCLIB talks to. It speaks
[tinclib-protocol](https://github.com/gavinhsmith/tinclib-protocol) **v0.1.0**
to the TI-84 Plus CE over UART, and handles the Wi-Fi and HTTP work for it.
Protocol 0.1 covers plain-HTTP GET only. HTTPS, TLS, NTP and POST arrive with
later protocol versions.

One shared core runs on every chip. Each chip adds a thin platform layer.

| Chip | Directory | PlatformIO env | Status |
|---|---|---|---|
| ESP8266 | `platforms/esp8266/` | `esp8266` | v0.1 |
| ESP32 | not started | | planned |

## Layout

| Path | What |
|---|---|
| `lib/tinc_core/` | Chip-independent C99 with no Arduino calls: UART framing link, dispatcher, request state machine, HTTP parser, ASCII transcoding, Wi-Fi ranking |
| `lib/tinc_core/tinc_platform.h` | What every chip must provide: clock, heap, UART, non-blocking TCP, Wi-Fi status and slot storage |
| `platforms/<chip>/` | One platform layer per chip. It implements `tinc_platform.h` and calls `tinc_link_step()` and `tinc_poll()` from its loop |
| `external/tinclib-protocol` | Submodule pinned to a protocol tag. `lib/tinc_core/proto.c` builds it; it is never copied |
| `test/test_core/` | Native Unity tests for the core, including the protocol's golden vectors, run against `test/fake_platform.h` |
| `test/fuzz/` | libFuzzer target for the dispatcher |

## Build and test

```sh
git clone --recursive https://github.com/gavinhsmith/tinclib-firmware
pip install platformio
pio test -e native            # core tests on the PC
pio run -e esp8266            # ESP8266 firmware
pio run -e esp8266 -t upload
```

On Windows, run `pio` from PowerShell or cmd. MSYS2's gcc fails silently
when it is invoked from Git Bash.

## Adding a chip

1. Create `platforms/<chip>/` implementing `lib/tinc_core/tinc_platform.h`.
   Nothing in it may block: DNS, connect, UART writes and Wi-Fi joins must
   all make progress one loop iteration at a time.
2. Add `[env:<chip>]` to `platformio.ini` with `build_src_filter = +<<chip>/>`.
3. Add the chip to the `build` matrix in `.github/workflows/ci.yml`.

The core and its tests don't change. If a chip needs something the
interface doesn't offer, extend `tinc_platform.h` and the fake together.

## ESP8266 notes

- The `esp8266` env uses the `esp12e` board, which assumes 4 MB of flash.
  Check the module with `esptool.py flash_id` before the first flash. For a
  1 MB module, set `board_build.ldscript = eagle.flash.1m64.ld`.
- TCP uses lwIP's raw API because `WiFiClient::connect()` blocks. The data
  the server sends is only acknowledged as the CE reads it, so a slow reader
  slows the download instead of filling RAM.
- The debug log goes to Serial1 (GPIO2, TX only). The reset reason is logged
  at boot.
- The ESP8266 SDK doesn't report WPA2-Enterprise in scan results, so those
  networks can't be flagged as unsupported. Joining them simply fails.

## Known protocol gaps (0.1)

- The `WIFI_SET` comment in `protocol.h` says the ESP joins "the first
  reachable slot, 0 → 2". This firmware ranks candidates by RSSI instead, as
  AGENTS.md requires. The protocol.h comment should be fixed upstream.
- `WIFI_LIST` can be up to 99 bytes, which is more than a peer that
  advertised the 64-byte minimum `max_payload` can receive.
- There is no error code for a storage failure. A failed slot write returns
  `ERR_NO_MEM`.
- The hidden-SSID flag doesn't exist in `WIFI_SET` yet, so hidden networks
  can't be joined.
