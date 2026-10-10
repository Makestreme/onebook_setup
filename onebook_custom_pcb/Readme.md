# OneBook on my custom PCB (personal notes)

> **Building a OneBook? You can ignore this folder.** It's my personal reference for an early prototype board and isn't needed for any build. Use [OneBook/OneBook.ino](../OneBook/OneBook.ino) and the [setup guide](../SETUP.md) instead.

Personal reference for running OneBook on my own board (Makestreme, rev 1.0, schematic in [../sch.png](../sch.png)).

Firmware: [onebook_custom_pcb.ino](onebook_custom_pcb.ino). It's the universal `OneBook.ino` plus the board hacks below. Arduino needs the `.ino` name to match its folder, so keep the two names in sync.

## The board in short
| Part | Notes |
|---|---|
| ESP32-S3-MINI-1-**N8** | 8 MB flash, no PSRAM |
| CP2102N (USB-serial) | TXD drives RXD0 (GPIO44), RXD on TXD0 (GPIO43). Powered **only from VBUS** |
| LSM6DS3TR-C IMU | I²C on IO7 (SDA) / IO8 (SCL), 4.7k pull-ups R11/R13, CS tied to 3V3 |
| AP2112K 3.3 V regulator | Fed **only from V_SYS** (H1 pin 1), not from USB |
| MCP73831 charger | Charges BAT+ from VBUS |
| J1 "FTDI breakout" | 1 GND, 2 3V3, 3 CP_TX (= GPIO44 / RX), 4 CP_RX (= GPIO43 / TX), 5 EN, 6 IO0 |

Only IO0, IO7, IO8, GPIO43 and GPIO44 are reachable, hence the hacks.

## Wiring
| Signal | GPIO | Solder point | Goes to |
|---|---|---|---|
| SD CS | 8 (SCL net) | Signal side of R11 | Blue module CS |
| SD SCK | 7 (SDA net) | Signal side of R13 | Blue module SCK |
| SD MOSI | 43 (TXD0) | J1 pin 4, labelled **CP_RX** | Blue module MOSI |
| SD MISO | 44 (RXD0) | J1 pin 3, labelled **CP_TX** | Blue module MISO (direct, see below) |
| LED data | 0 | J1 pin 6 (IO0) | WS2812 DIN |
| 5 V in | | H1 pin 1 (V_SYS) | External 5 V supply |
| | | | Blue module VCC and WS2812 VCC from the same 5 V |
| GND | | J1 pin 1 or H2 pin 2 | Common ground |

- J1 labels are from the CP2102's point of view: **CP_RX is the ESP32's TX**. Easy to swap MOSI/MISO by mistake.
- CS on SCL and SCK on SDA is deliberate: SCL only rises with CS, when SCK is low, so the IMU can only ever see address 0x00 (general call) and ignores it.
- Pin constants in the sketch: `SD_CS=8`, `SD_SCK=7`, `SD_MOSI=43`, `SD_MISO=44`, `LED=0`, `I2C_SDA=7`, `I2C_SCL=8`.

## Power rules
- **5 V into H1 pin 1 (V_SYS).** Switch open, BAT terminals empty, battery disconnected.
  - Don't feed 5 V into the BAT terminals: that puts 5 V on the charger's VBAT pin, and any leak back into VBUS would wake the CP2102.
- **Never plug USB-C in while it's running.** A powered CP2102 holds RX high and breaks SD reads (and fights MISO directly if there's no resistor).
- Back-powering check done: C7 (CP2102 VDD) reads ~0.4 V with the board running from H1. Fine; anything under ~1 V is safe.

## Flashing
USB-C isn't detected by the PC (known board issue, see below), so I flash through an external UART adapter on J1.

1. Arduino settings: ESP32S3 Dev Module, Flash 8MB, Partition **8M with spiffs (3MB APP/1.5MB SPIFFS)**, PSRAM off, **USB CDC On Boot off**, Upload Mode UART0, **Core Debug Level None** (debug output would go out on the SD lines).
2. Connect adapter GND, TX → J1 pin 3, RX → J1 pin 4. Board powered from the 5 V supply, not the adapter's 3V3.
3. **Without the MISO resistor: disconnect the SD module's MISO wire (or the module) first.**
4. Hold BOOT, tap RST, release BOOT, upload.
5. Remove the adapter's TX wire (it overrides MISO), reconnect MISO, tap RST.

"Sketch too big" = Flash Size / Partition still on 4 MB defaults (*Maximum is 1310720*). Set Flash Size 8MB first, then the 8M partition. Huge APP also works.

## The MISO resistor
- Plan was a series resistor (330 Ω) so the UART adapter or CP2102 can override the always-driven MISO of the blue module without a fight.
- **With 470 Ω fitted, the log said "SD: MISO stuck low".** The unpowered CP2102's TXD pin drags RX towards GND and beat the module through 470 Ω.
- **Wired direct, it works.** That's the current setup.
- Without the resistor, two outputs share RX during flashing (50–100 mA when they disagree, uploads may fail) and if USB-C gets plugged in. Hence step 3 above and the USB-C rule.
- If I want protection back: try ~100 Ω, then confirm the SD card still mounts and uploads still work with MISO connected.

## IMU hack
- At every boot `imuDisableI2c()` looks for WHO_AM_I (reg 0x0F) = 0x6A at addresses 0x6A and 0x6B, then sets **CTRL4_C (0x13) bit 2, I2C_disable**. It's lost on power-off, so it runs every boot, before the SD card starts.
- The IMU may stop acknowledging mid-write once the bit takes effect, so the firmware re-reads WHO_AM_I: no answer also counts as success.

| Log line | Meaning |
|---|---|
| `IMU at 0x6A: I2C disabled` | Normal |
| `IMU at 0x6A: I2C disable failed` | Write failed and the IMU still answers. Usually harmless thanks to the CS/SCK pin order |
| `IMU: no answer (already disabled or absent)` | Normal after a warm reset (RST without power-off) |

## Serial debugging
SD and UART0 share pins, so there's no serial log normally. Use `http://onebook.local` or the web app's **Settings → Show device log**.

For bench debugging of Wi-Fi or the tunnel: set `#define DEBUG_SERIAL 1` (logs over UART0 at 115200 and **skips the SD card**, so the LED won't show red), keep the adapter connected, open Serial Monitor. Set it back to 0 afterwards.

## Troubleshooting
| Log / symptom | Cause | Fix |
|---|---|---|
| `SD: mounted at 10000 kHz` | Fine | |
| `SD: mounted at 4000/1000 kHz` | Marginal signal at 10 MHz | Shorten SD wires, check joints on R11/R13 |
| `SD: card answers but has no readable FAT32 filesystem` | Wiring OK | Format FAT32 (cards > 32 GB ship exFAT) or try another card |
| `SD: no reply` | Card's reply not reaching GPIO44 | MOSI/MISO swapped (J1 labels!), adapter TX still on J1 pin 3, USB-C plugged in, CS/SCK swapped, module not on 5 V (its regulator output should be ~3.3 V) |
| `SD: MISO stuck low` | RX pulled low | MISO wire, a series resistor too large (see above), MOSI/MISO swapped |
| `SD: garbled reply 0x..` | Poor contact or noise | SCK/MOSI wiring, module contacts |
| LED red after flashing | Adapter TX still connected, or MISO left disconnected | Remove TX, reconnect MISO, tap RST |
| "Failed to connect to ESP32" | Not in download mode, or MISO fighting the adapter | BOOT + RST again; disconnect MISO |
| Works on the bench, fails in the case | USB-C plugged in, or 5 V into BAT instead of H1 | See power rules |

## Known board issues (for the next revision)
- **USB-C:** 5 V reaches C9 (connector and CC resistors fine), but the PC doesn't detect the CP2102 and the battery doesn't charge. Suspects: CP2102N QFN joints / exposed pad, D+/D- routing (must go to A6/B6 and A7/B7), charge LED1 orientation.
- **VBUS doesn't power the ESP32.** The regulator is fed only from V_SYS (battery via switch). Needs a power path (e.g. diode-OR VBUS into V_SYS).
- **Running 24/7 on the battery** keeps it at 4.2 V and the charger never terminates. Fine for now since the battery is disconnected.
- **No free GPIOs.** Break out spare pins for SD (ideally the S3's default SPI pins 10–13) so the UART and IMU hacks aren't needed.
- **GPIO0 drives the LED** and is also the BOOT strap. Works, but a dedicated pin would be cleaner.

## Keeping it in sync with OneBook.ino
When the universal firmware changes, port the change here. The differences are limited to:
- the `DEBUG_SERIAL` switch and its `#if` blocks (log, LED red check, `sdRetry`, WiFiManager debug, `setup()`),
- `#include <Wire.h>`, the pin constants and `imuDisableI2c()`,
- the SD failure messages in `logSdFailure()` (mention RX/USB-C/resistor).
