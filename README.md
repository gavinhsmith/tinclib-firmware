# tinclib-firmware

[![CI](https://github.com/gavinhsmith/tinclib-firmware/actions/workflows/ci.yml/badge.svg)](https://github.com/gavinhsmith/tinclib-firmware/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/gavinhsmith/tinclib-firmware)](https://github.com/gavinhsmith/tinclib-firmware/releases)
[![License: Apache 2.0](https://img.shields.io/github/license/gavinhsmith/tinclib-firmware)](LICENSE)

**ESP8266 firmware that gives the TI-84 Plus CE Wi-Fi, HTTP and HTTPS.** Flash
it onto an ESP8266 board, plug the board into the calculator's USB port, and
programs built with [tinclib](https://github.com/gavinhsmith/tinclib) can call
REST APIs, fetch web pages and upload data. No board? Run the same firmware as
a desktop app and plug the calculator straight into your PC.

- **HTTP and HTTPS**: GET, HEAD, POST, PUT, PATCH and DELETE, request bodies,
  response headers, chunked transfer and redirects.
- **Verified TLS**: certificates are always checked against Mozilla's CA
  roots, built into the firmware image. TLS 1.2 via BearSSL.
- **Wi-Fi profiles**: 5 saved networks (hidden ones included), joined
  strongest-signal first, with an optional firmware-side lock.
- **Small and non-blocking**: fits in the ESP8266's ~40 KB of free heap and
  never blocks the main loop, so the calculator always gets a reply in time.
- **Calculator-friendly**: responses are pulled in pieces as the calculator
  reads them, with optional transcoding to the CE's character set.
- **Runs on a PC too**: the same core builds as a Windows, Linux and macOS
  app, with a packet trace for debugging.
- **Tested**: native unit tests with the protocol's golden vectors, a fuzzed
  dispatcher, and an end-to-end test of the PC build.

## How it fits together

tinclib-firmware is one part of four:

| Repo | What |
|---|---|
| [tinclib](https://github.com/gavinhsmith/tinclib) | The C library your calculator program links against |
| **tinclib-firmware** (this one) | Firmware for the ESP8266 board, which does the networking |
| [tinclib-config](https://github.com/gavinhsmith/tinclib-config) | TINCLIBC, the calculator app for Wi-Fi setup |
| [tinclib-protocol](https://github.com/gavinhsmith/tinclib-protocol) | The wire protocol between calculator and board |

```
TI-84 Plus CE ──USB──> ESP8266 board ──Wi-Fi──> the internet
  (tinclib)            (this firmware)
```

This release speaks tinclib-protocol **v0.7**. See tinclib's README for which
boards the calculator's USB driver supports.

## Quick start

Download a build from [Releases](https://github.com/gavinhsmith/tinclib-firmware/releases/latest):

| Target | File | Status |
|---|---|---|
| ESP8266 | `tinclib-firmware-esp8266.bin` | protocol 0.7, 5 Wi-Fi slots |
| PC: Windows, Linux, macOS | `tinclib-pc-*` | protocol 0.7, uses the PC's network |
| ESP32 | | planned |

Flash the ESP8266 build:

```sh
pip install esptool
esptool.py --port COM5 write_flash 0x0 tinclib-firmware-esp8266.bin   # /dev/ttyUSB0 on Linux
```

Then set up Wi-Fi from the calculator with
[TINCLIBC](https://github.com/gavinhsmith/tinclib-config).

## Build from source

```sh
git clone --recursive https://github.com/gavinhsmith/tinclib-firmware
pip install platformio
pio test -e native            # core tests on the PC
pio run -e esp8266            # ESP8266 firmware
pio run -e esp8266 -t upload
pio run -e pc                 # PC app, see below
```

On Windows, run `pio` from PowerShell or cmd. MSYS2's gcc fails silently
when it is invoked from Git Bash.

## PC target: no board needed

`env:pc` builds the firmware as a desktop app that stands in for the board. Plug the calculator straight
into the PC; `tinclib` then runs in USB device mode and shows up as a serial port, and the app answers it
using the PC's own network.

```sh
pio run -e pc                          # -> .pio/build/pc/program(.exe)
.pio/build/pc/program COM7             # Windows; /dev/ttyACM0 on Linux, /dev/cu.usbmodem* on macOS
```

The app waits for the port to appear and reopens it if the calculator is unplugged, which the calculator
sees like a board reset. The PC can't join another network, so it reports itself as connected, has a
single Wi-Fi slot named "LAN", and keeps it locked (the protocol's Wi-Fi lock): `WIFI_SET` and
`WIFI_FORGET` get `ERR_LOCKED`, and every request goes out through the PC's own networking stack.
`INFO` reports the PC's own MAC: that of the network adapter it reaches the internet through (the physical one under a VPN), or all zero when offline.

Every packet is printed to stdout as it passes, one readable line each (`>` from the calculator, `<` the
reply); log lines (request states and errors) go to stderr:

```
     3.102  calc > #2 HELLO v0.7 max_payload=256
     3.103  calc < #2 HELLO ok v0.7 max_payload=1024 heap=1048576 wifi_slots=1
     3.210  calc > #5 REQ_BEGIN GET http://api.example.com/v1/items?... (headers: 31 bytes, not shown)
     3.498  calc < #7 REQ_STATUS BODY http=200 len=812 type=application/json
     3.520  calc < #8 BODY_READ @0 200 bytes: "{\"items\":[{\"id\":1,\"name\":\"first\"},{\"id\":2,\"n"...
```

The trace never shows request header text, request bodies, response header values, URL query strings
(they often carry API keys) or Wi-Fi passwords; it shows their lengths instead. Response body data is cut
to its first 48 bytes.

### Bridge mode: a real board, without powering it separately

To test a real board with the calculator before it has its own power, plug both into the PC (the board
is powered by its USB) and bridge the two ports:

```sh
.pio/build/pc/program COM7 --bridge COM5    # calculator's port, then the board's
```

Everything is passed through unchanged, and every packet is traced exactly as above; the board, not the
app, answers the calculator. The board port's DTR and RTS are kept off so the dev board's auto-reset
circuit doesn't hold the ESP in reset. Either side can be unplugged and comes back on its own.

`test/pc/e2e.py` tests it end to end (add `--bridge` to run the same test through bridge mode): a
pseudo-terminal plays the calculator and a local HTTP server the internet (Linux and macOS; CI runs it
on both).

## Layout

One shared core runs on every target. Each target adds a thin platform layer.

| Path | What |
|---|---|
| `lib/tinc_core/` | Target-independent C99 with no Arduino or OS calls: UART framing link, dispatcher, request state machine, HTTP parser, ASCII transcoding, Wi-Fi ranking |
| `lib/tinc_core/tinc_platform.h` | What every target must provide: clock, heap, UART, non-blocking TCP, Wi-Fi status and slot storage |
| `lib/tinc_tls/` | HTTPS for the BearSSL targets (ESP8266 and PC) over the platform's plain TCP |
| `certs/cacert.pem` | Mozilla's CA roots, turned into a flash-resident table at build time by `tools/gen_roots.py` |
| `platforms/<target>/` | One platform layer per target. It implements `tinc_platform.h` and calls `tinc_link_step()` and `tinc_poll()` from its loop |
| `external/tinclib-protocol` | Submodule pinned to a protocol tag. `lib/tinc_core/proto.c` builds it; it is never copied |
| `test/test_core/` | Native Unity tests for the core, including the protocol's golden vectors, run against `test/fake_platform.h` |
| `test/fuzz/` | libFuzzer target for the dispatcher |
| `test/pc/e2e.py` | End-to-end test of the PC target |

## Adding a target

1. Create `platforms/<target>/` implementing `lib/tinc_core/tinc_platform.h`.
   Nothing in it may block: DNS, connect, UART writes and Wi-Fi joins must
   all make progress one loop iteration at a time.
2. Add `[env:<target>]` to `platformio.ini` with `build_src_filter = +<<target>/>`.
3. Add the target to the `build` matrix in `.github/workflows/ci.yml`.

The core and its tests don't change. If a target needs something the
interface doesn't offer, extend `tinc_platform.h` and the fake together.

## ESP8266 notes

- The `esp8266` env uses the `esp12e` board, which assumes 4 MB of flash.
  Check the module with `esptool.py flash_id` before the first flash. For a
  1 MB module, set `board_build.ldscript = eagle.flash.1m64.ld`.
- TCP uses lwIP's raw API because `WiFiClient::connect()` blocks. The data
  the server sends is only acknowledged as the CE reads it, so a slow reader
  slows the download instead of filling RAM.
- TLS tops out at 1.2 (BearSSL), so a server that requires TLS 1.3 fails
  with `ERR_TLS`. The clock comes from SNTP after joining Wi-Fi; HTTPS waits
  for it.
- Wi-Fi lock: build with `-D TINC_WIFI_LOCK=1` (e.g. `build_flags` in `platformio.ini`) to stop the
  calculator changing the saved profiles. There's no physical switch wired up yet.
- Hidden networks (the `HIDDEN` slot flag) are joined by name after every network the scan did show.
- 5 Wi-Fi slots (`-D TINC_SLOT_COUNT=5` in `platformio.ini`; the core reports it in `HELLO`).
- Profiles saved by the older 3-slot firmware (protocol 0.1/0.2) move into slots 0-2 on the first boot
  after an upgrade.
- `INFO` reports the Wi-Fi station MAC, for registering the board on a
  MAC-filtered network.
- The debug log goes to Serial1 (GPIO2, TX only). The reset reason is logged
  at boot.
- The ESP8266 SDK doesn't report WPA2-Enterprise in scan results, so those
  networks can't be flagged as unsupported. Joining them simply fails.

## Known protocol gaps (0.7)

- The `WIFI_SET` comment in `protocol.h` says the ESP joins "the first
  reachable slot, 0 → wifi_slots-1". This firmware ranks candidates by RSSI instead, as
  AGENTS.md requires. The protocol.h comment should be fixed upstream.
- There is no error code for a storage failure. A failed slot write returns
  `ERR_NO_MEM`.

## License

[Apache 2.0](LICENSE)
