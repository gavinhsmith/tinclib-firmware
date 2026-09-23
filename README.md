# tinclib-firmware-esp8266

ESP8266 firmware for TINCLIB. It speaks
[tinclib-protocol](https://github.com/gavinhsmith/tinclib-protocol) **v0.1.0**
to the TI-84 Plus CE over UART, and handles the Wi-Fi and HTTP work for it.
Protocol 0.1 covers plain-HTTP GET only. HTTPS, TLS, NTP and POST arrive with
later protocol versions.

## Layout

| Path | What |
|---|---|
| `external/tinclib-protocol` | Submodule pinned to a protocol tag. `lib/core/proto.c` builds it; it is never copied |
| `lib/core/` | Pure C core with no Arduino calls: dispatcher, request state machine, HTTP parser, ASCII transcoding, Wi-Fi ranking. `platform.h` is what the platform layer must provide |
| `src/main.cpp` | ESP8266 platform layer: UART, Wi-Fi, lwIP raw TCP (DNS and connect never block), LittleFS slots, and a debug log on Serial1 (GPIO2) |
| `test/test_core/` | Native Unity tests, including the protocol's golden vectors, run against `test/fake_platform.h` |
| `test/fuzz/` | libFuzzer target for the dispatcher |

## Build and test

```sh
git clone --recursive https://github.com/gavinhsmith/tinclib-firmware
pip install platformio
pio test -e native      # core tests on the PC
pio run -e esp12e       # firmware
pio run -e esp12e -t upload
```

On Windows, run `pio` from PowerShell or cmd. MSYS2's gcc fails silently
when it is invoked from Git Bash.

Before the first flash, check the module's flash size with
`esptool.py flash_id`. The `esp12e` board assumes 4 MB. For a 1 MB module,
set `board_build.ldscript = eagle.flash.1m64.ld`.

## Known gaps (protocol 0.1)

- The `WIFI_SET` comment in `protocol.h` says the ESP joins "the first
  reachable slot, 0 → 2". This firmware ranks candidates by RSSI instead, as
  AGENTS.md requires. The protocol.h comment should be fixed upstream.
- `WIFI_LIST` can be up to 99 bytes, which is more than a peer that
  advertised the 64-byte minimum `max_payload` can receive.
- There is no error code for a storage failure. A failed slot write returns
  `ERR_NO_MEM`.
- The hidden-SSID flag doesn't exist in `WIFI_SET` yet, so hidden networks
  can't be joined.
- The ESP8266 SDK doesn't report WPA2-Enterprise in scan results, so those
  networks can't be flagged as unsupported. Joining them simply fails.
