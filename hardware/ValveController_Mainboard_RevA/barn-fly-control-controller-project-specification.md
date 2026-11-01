# Project Specification

## Status
Controller schematic authorized; not yet built. Project renamed Barn Fly-Control Controller. Execution stopped because account ACUs ran out. At stop: 0 components and 0 nets; no layout or routing performed.

## Approved scope
Design a fly-control controller PCB with Mega 2560-compatible controller, DS3231 RTC, SD socket, Ender 3 ENH12864Z-1 VER1.0 ST7920 display interface, and E-stop monitoring. Begin safe low-voltage schematic work now. Retain eventual onboard 120 V AC input and two onboard actuator relays, but DO NOT choose or build AC-to-DC supply or actuator power circuitry until pump and valve ratings are verified. DO NOT route PCB before schematic approval.

## Application
20 × 40 × 10 ft cattle-and-goat barn in Texas. Roughly eight anti-drip nozzles is a preliminary sizing basis only. 0.257 L/min total at 100 psi at nozzles is screening only, not a finalized operating requirement. Texas pesticide registration and final spray layout remain explicitly unverified.

## Firmware baseline to re-read
Repository https://github.com/M3nta1Ins/fly-control-arduino ; previously inspected commit 9028f789e48d.
Files: README.md; hardware/ValveController_Mainboard_RevA/DESIGN_BRIEF.md; firmware/ValveController_MultiInterface/ValveController_MultiInterface.ino; firmware/ValveController_UI/ValveController_UI.ino.
Previously observed pin assignments (revalidate before capture): ST7920 clock/data/CS D6/D7/D8; encoder A/B/button D22/D23/D24; E-stop NC to ground D25 with INPUT_PULLUP; active-low actuator commands D30/D31; RTC SDA/SCL D20/D21; SD MISO/MOSI/SCK/CS D50/D51/D52/D53.

## Display blocker
Brief's connector map: 1=5V, 2=GND, 3=SID, 4=CS, 5=clock, 6=NC, 7=encoder A, 8=encoder B, 9=click, 10=beeper. Firmware comment instead says connector pin 5=encoder A. Neither map is approved. Resolve exact display connector numbering, orientation, and signal mapping using authoritative evidence before wiring display connector. If unavailable, leave only dependent connections open and ask for board photos/documentation; continue independent blocks.

## Controller implementation direction
Build reference-based MCU support circuitry with proper supply decoupling, reset, clock and programming access. Establish 5 V/3.3 V logic compatibility and SD level translation from sourced data. Firmware/bootloader programming path must be explicit. Choose small reversible details autonomously; do not assume an unapproved Mega shield/module architecture. Complete capture plan before drafting a multi-block schematic. Provide component Role, Role Details, Design Note metadata and functional groups. Verify power, reset states, pin roles, pull-ups, and decoupling; do not treat ERC alone as electrical verification.

## Deferred mains and actuator sections
No AC/DC module, load relay, motor/valve power protection, or associated power wiring is authorized yet. Keep these visible as deferred requirements, not connected placeholders suggesting a finished circuit. Future mains design needs protected isolated conversion, coordinated protection, physical segregation, enclosure/strain relief and PE or insulation-class decisions. Do not claim the completed assembly is certified.

## E-stop
D25 is monitoring only, not a safety-rated stop. Independent hardwired interruption of actuator energy is unfinished and must be tracked. Do not finalize safety contacts or interrupt ratings without actuator data. Keep monitoring and safety functions distinguished.

## Pump research state
Candidate Aquatec 6841-2P77-B334, not approved for purchase/use. Distributor catalogs describe 12 VDC, Viton, 160 psi bypass, quarter-inch John Guest ports, metal mount. Detailed exact-catalog extraction was still processing in prior research; manufacturer build confirmation remains missing. Standard 6800 12 V reference is NOT exact variant and stops tabulated flow/current at 80 psi. Exact pressure-flow-current above 100 psi, startup peak/duration, restart against pressure, stall current, bypass tolerance/adjustment/stability, hot-barn partial/full bypass thermal duty, complete wetted materials, and compatibility with diluted Sector Misting Concentrate EPA 1021-1834 all remain unverified. Dilution and duty conditions not finalized. User received a send-ready inquiry; it was NOT sent.

Prior shortlist also included BETE MW085 anti-drip nozzle and Solenoid Solutions Series 8 NC direct-acting 12 V/11 W valve configuration; neither is finalized. Revalidate before committing to parts. Original Pumptec 113C candidate was oversized and has unresolved startup/configuration data. Existing pump-before-valve two-second firmware sequence must not be assumed safe; requires reconciled sequencing/regulation.

## Next execution
1. Read project state and this specification; fetch required schematic, sourcing, verification, and executor workflow skills.
2. Create capture plan and build authorized low-voltage blocks using sourced facts and a scoped build executor.
3. Research display mapping in parallel with independent safe controller work.
4. Review completed electrical capture with fresh-context review; correct findings and inspect checks.
5. Present schematic progress and full list of any open pin-to-pin connections with blockers. Stop before layout/routing and request schematic approval at the appropriate checkpoint.
