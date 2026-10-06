# Fly Control

An Arduino Mega 2560 project designed in Cirkit Designer. The circuit combines an ILI9341 SPI TFT, microSD card, DS3231 real-time clock, keypad, and four-channel relay module. The project and this repository are for documenting and troubleshooting the complete design. Firmware source code is intentionally not included in this README.

## Project links

- [Open the Cirkit Designer project](https://app.cirkitdesigner.com/project/24b07511-9851-44f0-a418-0e4dbb4c1f2e)
- [GitHub repository](https://github.com/M3nta1Ins/fly-control-arduino)

## Components in the design

- Arduino Mega 2560 controller
- ILI9341 SPI TFT display (the display under investigation)
- microSD card module / Cirkit MicroSD VFS simulator component
- DS3231 RTC module using I2C (`Wire` / `RTClib`)
- Matrix keypad (`Keypad` library)
- Four-channel 5 V relay module
- A separate character LCD is also visible in the circuit, but it is not part of the TFT issue and is treated as unrelated for this investigation.

## TFT pin assignments in the sketch

These are the pin numbers defined by the current sketch. They describe the intended connections; they do not by themselves prove that every wire terminates on the correct component pin.

| ILI9341 signal | Arduino Mega 2560 pin |
| --- | ---: |
| CS | 53 |
| DC / RS | 48 |
| RESET | 49 |
| MOSI / SDI | 51 |
| SCK | 52 |
| MISO / SDO | Not connected; not required for display output |

The SD card uses chip select pin 47 and shares the Mega SPI bus. The TFT and SD card have separate chip-select pins. Mega hardware SPI uses pin 51 for MOSI, pin 52 for SCK, and pin 50 for MISO. The SD card needs its SPI signals for storage access; display-only TFT writes do not require TFT MISO. The previous visual review indicated the TFT power and backlight were connected to 3.3 V with a common ground, but the full set of wire endpoints was not conclusively verified.

The RTC is accessed over I2C. The Mega 2560 I2C pins are SDA 20 and SCL 21; verify that the RTC SDA/SCL wires land on those pins. The keypad row/column pin mapping and the relay IN1–IN4 pin mapping have not been recorded here, so they still need to be traced from the project/code before hardware use.

## Firmware setup relevant to the TFT

The sketch uses the Arduino `SPI`, `SD`, and `Wire` libraries, Adafruit GFX and Adafruit ILI9341, RTClib, and Keypad. The TFT object is configured with CS 53, DC 48, and RESET 49. In setup, the display is started with `tft.begin()`, set to rotation 1, and cleared using the project background color.

During troubleshooting, startup was updated to explicitly call `SPI.begin()` and set both TFT CS 53 and SD CS 47 inactive (HIGH) before TFT initialization. This is intended to prevent either device from being selected while the shared SPI bus is starting. No circuit wiring was changed.

## Problem being investigated

The ILI9341 remains completely black in the Cirkit Designer simulation. The program compiles and runs, but no TFT output is visible. The simulation console reports:

```text
SD CARD: OK
SD LOG: SYSTEM START
```

This confirms that the sketch starts and the simulated SD card initializes; it does not confirm that the TFT receives valid SPI commands or that the display model renders correctly. Earlier debug logs showed Arduino serial transmit/receive activity and relay-module status, but no clear TFT-specific error or success message. The LCD shown elsewhere in the design is considered irrelevant to this TFT fault.

## Troubleshooting completed

1. Identified the display as an ILI9341 SPI TFT and checked the sketch pin definitions against the Mega hardware SPI pins.
2. Confirmed in the sketch that TFT CS 53 and SD CS 47 are distinct, so they are not assigned the same chip-select pin.
3. Added explicit SPI startup and deasserted both chip selects before calling `tft.begin()`.
4. Recompiled and restarted the Cirkit simulation. Compilation succeeded (reported as 37,996 bytes of program storage and 2,413 bytes of globals).
5. Confirmed that SD initialization and the startup log succeed, while the TFT still appears black.
6. Reviewed the more visible circuit layout. The intended SPI/control wiring appears plausible, but not every endpoint could be verified conclusively from the view, so a wiring fault has not been ruled out.

## Current assessment and open checks

The SPI startup change did not fix the simulated TFT. The cause is still undetermined. Possible areas to check are: exact wire endpoints and common ground; TFT module power/backlight and logic-level requirements; whether the module is truly an SPI ILI9341 variant; the simulator component model and its supported pin names; and whether the display drawing code runs after initialization. The SD success message alone cannot distinguish among these causes.

Next useful isolation step: run a minimal display-only test that initializes the ILI9341 and fills the screen with a bright color, with the SD card deselected. If that still produces a black simulation display, test the simulator model/pin mapping or the physical module separately. Keep the original circuit and code backed up before any further changes.

## Project status

- Board: Arduino Mega 2560
- Display: ILI9341 SPI TFT
- Cirkit simulation compile: successful after the SPI startup update
- SD initialization in simulation: successful
- TFT rendering in simulation: unresolved; black screen persists
- Every TFT wire endpoint: not conclusively verified
- Keypad and relay pin maps: not documented in this README
- Physical hardware test: not recorded
- Firmware source code: intentionally not included in this README; add the project sketch separately if desired
