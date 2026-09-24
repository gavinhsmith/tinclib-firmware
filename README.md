# tinclib-firmware

Firmware for the network co-processor that TINCLIB talks to. It speaks
[tinclib-protocol](https://github.com/gavinhsmith/tinclib-protocol) **v0.3.0**
to the TI-84 Plus CE over UART, and handles the Wi-Fi and HTTP work for it.
Protocol 0.3 covers plain-HTTP GET, Wi-Fi profiles (hidden networks included;
the firmware decides how many slots it has) and a firmware-side Wi-Fi lock. HTTPS, TLS, NTP and POST arrive with
later protocol versions.

One shared core runs on every target. Each target adds a thin platform layer.

| Target | Directory | PlatformIO env | Status |
|---|---|---|---|
| ESP8266 | `platforms/esp8266/` | `esp8266` | protocol 0.3, 5 Wi-Fi slots |
| PC (Windows, Linux, macOS) | `platforms/pc/` | `pc` | protocol 0.3, 1 Wi-Fi slot, calculator plugged into the PC |
| ESP32 | not started | | planned |

## Layout

| Path | What |
|---|---|
| `lib/tinc_core/` | Target-independent C99 with no Arduino or OS calls: UART framing link, dispatcher, request state machine, HTTP parser, ASCII transcoding, Wi-Fi ranking |
| `lib/tinc_core/tinc_platform.h` | What every target must provide: clock, heap, UART, non-blocking TCP, Wi-Fi status and slot storage |
| `platforms/<target>/` | One platform layer per target. It implements `tinc_platform.h` and calls `tinc_link_step()` and `tinc_poll()` from its loop |
| `external/tinclib-protocol` | Submodule pinned to a protocol tag. `lib/tinc_core/proto.c` builds it; it is never copied |
| `test/test_core/` | Native Unity tests for the core, including the protocol's golden vectors, run against `test/fake_platform.h` |
| `test/fuzz/` | libFuzzer target for the dispatcher |
| `test/pc/e2e.py` | End-to-end test of the PC target |

## Build and test

```sh
git clone --recursive https://github.com/gavinhsmith/tinclib-firmware
pip install platformio
pio test -e native            # core tests on the PC
pio run -e esp8266            # ESP8266 firmware
pio run -e esp8266 -t upload
pio run -e pc                 # PC stand-in for the board, see below
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
Releases attach prebuilt `tinclib-pc-*` binaries for Windows, Linux and macOS.

Every packet is printed to stdout as it passes, one readable line each (`>` from the calculator, `<` the
reply); log lines (request states and errors) go to stderr:

```
     3.102  calc > #2 HELLO v0.3 max_payload=256
     3.103  calc < #2 HELLO ok v0.3 max_payload=1024 heap=1048576 wifi_slots=1
     3.210  calc > #5 REQ_BEGIN GET http://api.example.com/v1/items?... (headers: 31 bytes, not shown)
     3.498  calc < #7 REQ_STATUS BODY http=200 len=812 type=application/json
     3.520  calc < #8 BODY_READ @0 200 bytes: "{\"items\":[{\"id\":1,\"name\":\"first\"},{\"id\":2,\"n"...
```

The trace never shows request header text, URL query strings (they often carry API keys) or Wi-Fi
passwords; it shows their lengths instead. Body data is cut to its first 48 bytes.

### Bridge mode: a real board, without powering it separately

To test a real board with the calculator before it has its own power, plug both into the PC (the board
is powered by its USB) and bridge the two ports:

```sh
.pio/build/pc/program COM7 --bridge COM5    # calculator's port, then the board's
```

Everything is passed through unchanged, and every packet is traced exactly as above; the board, not the
app, answers the calculator. The board port's DTR and RTS are kept off so the dev board's auto-reset
circuit doesn't hold the ESP in reset. Either side can be unplugged and comes back on its own.

`test/pc/e2e.py` tests it end to end (add `--bridge` to run the same test through bridge mode): a pseudo-terminal plays the calculator and a local HTTP server the
internet (Linux and macOS; CI runs it on both).

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
- Wi-Fi lock: build with `-D TINC_WIFI_LOCK=1` (e.g. `build_flags` in `platformio.ini`) to stop the
  calculator changing the saved profiles. There's no physical switch wired up yet.
- Hidden networks (the `HIDDEN` slot flag) are joined by name after every network the scan did show.
- 5 Wi-Fi slots (`-D TINC_SLOT_COUNT=5` in `platformio.ini`; the core reports it in `HELLO`).
- Profiles saved by the older 3-slot firmware (protocol 0.1/0.2) move into slots 0-2 on the first boot
  after an upgrade.
- The debug log goes to Serial1 (GPIO2, TX only). The reset reason is logged
  at boot.
- The ESP8266 SDK doesn't report WPA2-Enterprise in scan results, so those
  networks can't be flagged as unsupported. Joining them simply fails.

## Known protocol gaps (0.3)

- The `WIFI_SET` comment in `protocol.h` says the ESP joins "the first
  reachable slot, 0 → wifi_slots-1". This firmware ranks candidates by RSSI instead, as
  AGENTS.md requires. The protocol.h comment should be fixed upstream.
- There is no error code for a storage failure. A failed slot write returns
  `ERR_NO_MEM`.
