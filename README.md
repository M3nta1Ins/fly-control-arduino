# Fly Control

Arduino Mega 2560 project built in Cirkit Designer. The design includes an ILI9341 SPI TFT, microSD card, DS3231 RTC, keypad, and 4-channel relay module.

## Circuit project

[Open the Cirkit Designer project](https://app.cirkitdesigner.com/project/24b07511-9851-44f0-a418-0e4dbb4c1f2e)

## TFT and SPI wiring

The sketch currently defines these TFT pins:

| TFT signal | Mega 2560 pin |
| --- | ---: |
| CS | 53 |
| DC/RS | 48 |
| RESET | 49 |
| MOSI/SDI | 51 |
| SCK | 52 |
| MISO/SDO | Not connected (not needed for display output) |

The microSD card uses CS pin 47 and shares the SPI bus. The TFT and SD card therefore have separate chip-select pins. Confirm the project wiring against this table before building hardware.

## Current TFT issue

The Cirkit Designer simulation compiles and runs. The serial console reports `SD CARD: OK` and `SD LOG: SYSTEM START`, but the ILI9341 screen remains black. The sketch was updated to start SPI explicitly and set the TFT and SD chip selects inactive before display initialization. This did not resolve the black screen in simulation. The remaining cause has not been confirmed; it could be a connection, sketch/display initialization, or simulator-model issue.

## Firmware

The full sketch and library versions still need to be added from the Cirkit Designer project. Current project libraries include Adafruit GFX/ILI9341, RTClib, and Keypad, along with the Arduino SPI, SD, and Wire libraries.

## Build and test status

- Board: Arduino Mega 2560
- Simulation compile: successful
- SD initialization: successful in simulation
- TFT rendering: unresolved; screen remains black in simulation
- Physical hardware test: not yet recorded
