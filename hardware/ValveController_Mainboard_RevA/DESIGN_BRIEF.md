# Valve Controller Mainboard — Rev A Design Brief

**Status:** interface definition only. This is not a PCB layout or fabrication release; do not order a board from this document.

## Architecture

Standalone Mega 2560-class board using an ATmega2560 and a USB-to-serial interface. The design should follow Arduino's published Mega 2560 Rev3 schematic for the MCU clock, reset, programming, USB, and power sections. The existing LCD, encoder, RTC breakout, SD breakout, and two-channel relay board remain separate modules and connect to keyed/labeled headers.

The two-channel relay module remains external. This board only sends low-voltage control signals; no compressor or valve load wiring is routed on it.

## Firmware pin map to preserve

| Function | Mega-compatible pin | Board connection |
|---|---:|---|
| LCD SCLK | D6 | LCD header pin 5 |
| LCD SID / MOSI | D7 | LCD header pin 3 |
| LCD CS / RS | D8 | LCD header pin 4 |
| Encoder A | D22 | LCD header pin 7 |
| Encoder B | D23 | LCD header pin 8 |
| Encoder click | D24 | LCD header pin 9 |
| E-stop NC input | D25 | 2-pin E-stop terminal; other pin to GND |
| Relay 1 command (compressor) | D30 | Relay header IN1 |
| Relay 2 command (valve) | D31 | Relay header IN2 |
| RTC SDA | D20 / SDA | RTC header SDA |
| RTC SCL | D21 / SCL | RTC header SCL |
| SD MISO | D50 | SD header MISO |
| SD MOSI | D51 | SD header MOSI |
| SD SCK | D52 | SD header SCK |
| SD chip select | D53 | SD header CS |

LCD header is the user's 2x5 module pinout: 1=+5V, 2=GND, 3=SID, 4=CS/RS, 5=SCLK, 6=NC, 7=Encoder A, 8=Encoder B, 9=Encoder click, 10=Beeper. The firmware currently does not use the beeper. Confirm connector gender, key/orientation, and cable before routing the PCB.

Suggested module header labels: RTC `5V GND SDA SCL`; SD `5V GND MISO MOSI SCK CS`; relay `5V GND IN1 IN2`; E-stop `NC GND`.

## Current firmware cycle behavior

The primary firmware starts the compressor relay command at cycle start and keeps the valve relay off for a 2-second compressor lead-in. It then opens the valve for the configured valve time (1–60 seconds; default 5 seconds). When that valve timer expires, the firmware commands both the compressor and valve relays off in the same cycle-finish pass and marks the cycle idle. The compressor command does not continue after the cycle ends. If an automatic cycle's next-start deadline passes before the current cycle finishes, the next start is deferred by one full configured interval after completion so the compressor does not restart immediately.

Opening or breaking the normally closed E-stop circuit latches an emergency stop and disables both relay outputs. Reclosing the circuit and acknowledging the stop clears the latch but does not restart a cycle. These are firmware command behaviors; the E-stop input does not independently interrupt the compressor or valve power circuit.

## Safety and power boundaries

- E-stop is a software input only with the current single NC contact wiring. It does not independently disconnect compressor/valve power. The eventual load circuit needs a correctly rated hardwired safety interruption designed for its supply and load.
- Do not route mains or compressor/valve load current through this logic PCB.
- Power input, regulator, USB power isolation, mounting holes, board outline, and connector dimensions must be set before the manufacturing layout is finalized.

## Needed before a fabrication release

1. Confirm the exact LCD 2x5 connector/cable and orientation against the physical module.
2. Confirm the SD and RTC breakout header order and whether their 5V pins accept 5V.
3. Select power input and enclosure/mounting-hole dimensions.
4. Create the KiCad schematic and PCB, run electrical and layout checks, then export Gerber and drill files with a matching BOM/assembly drawing.
