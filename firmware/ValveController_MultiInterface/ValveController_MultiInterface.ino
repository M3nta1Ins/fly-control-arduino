#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <U8g2lib.h>
#include <RTClib.h>

// Primary working UI sketch; ValveController_UI remains the simpler backup.
// Ender 3 ENH12864Z-1 VER1.0 (ST7920), software SPI keeps Mega SPI free for SD.
// LCD signals: E/clock=6, RW/SID/data=7, RS/CS=8.
U8G2_ST7920_128X64_F_SW_SPI u8g2(U8G2_R0, 6, 7, 8, U8X8_PIN_NONE);
RTC_DS3231 rtc;

// Encoder connections per the user's latest LCD-module pin identification:
// LCD module pin 5 (A) -> Mega 22
// LCD module pin 8 (B) -> Mega 23
// LCD module pin 9 (click) -> Mega 24
constexpr uint8_t ENC_A = 22;
constexpr uint8_t ENC_B = 23;
constexpr uint8_t ENC_BUTTON = 24;

// Mega hardware SPI for SD: MISO=50, MOSI=51, SCK=52, CS=53.
constexpr uint8_t SD_CS = 53;
const char SD_TEST_FILE[] = "/VC_TEST.TXT";
const char SD_TEST_TEXT[] = "MEGA_SD_TEST_OK";

// SunFounder 2-channel 5V optocoupled relay board. It is low-level triggered:
// HIGH is inactive and LOW energizes a channel. Keep the load contacts empty
// while using the short relay diagnostics in this sketch.
constexpr uint8_t RELAY_1_PIN = 30;
constexpr uint8_t RELAY_2_PIN = 31;
constexpr uint8_t RELAY_ON_LEVEL = LOW;
constexpr uint8_t RELAY_OFF_LEVEL = HIGH;
constexpr uint16_t RELAY_TEST_DURATION_MS = 700;

// YJ139-LA38A-11ZS: NC contact terminals 11 and 12. With INPUT_PULLUP,
// the closed, released contact reads LOW; pressed or broken wiring reads HIGH.
constexpr uint8_t ESTOP_PIN = 25;

// Relay outputs now run the configured cycle; actuator loads are still not wired.
const char *menuItems[] = {
  "Display-", "Mode", "Interval", "Valve Time", "Set RTC Clock",
  "Test SD Card", "Test Relay 1", "Test Relay 2", "Start Cycle", "Back"
};
constexpr uint8_t MENU_ITEM_COUNT = sizeof(menuItems) / sizeof(menuItems[0]);

enum UiPage : uint8_t {
  PAGE_HOME, PAGE_MENU, PAGE_EDIT, PAGE_CLOCK, PAGE_STYLE, PAGE_MODE,
  PAGE_SD_TEST, PAGE_RELAY_TEST, PAGE_ESTOP_OVERRIDE
};
enum UiStyle : uint8_t { STYLE_SIMPLE, STYLE_CRITICAL, STYLE_DESCRIPTIVE };
enum CyclePhase : uint8_t { CYCLE_IDLE, CYCLE_COMPRESSOR_DELAY, CYCLE_VALVE_OPEN };
struct StoredSettings {
  uint16_t magic;
  uint8_t version;
  uint8_t intervalHours;
  uint8_t intervalMinutes;
  uint8_t valveSeconds;
  uint8_t autoMode;
  uint8_t interfaceStyle;
  uint8_t checksum;
};
enum SdTestResult : uint8_t {
  SD_TEST_NOT_RUN, SD_TEST_PASS, SD_TEST_CARD_FAIL, SD_TEST_VOLUME_FAIL,
  SD_TEST_ROOT_FAIL, SD_TEST_API_FAIL, SD_TEST_CREATE_FAIL, SD_TEST_WRITE_FAIL,
  SD_TEST_READ_FAIL, SD_TEST_DATA_FAIL
};
UiPage page = PAGE_HOME;
UiStyle interfaceStyle = STYLE_SIMPLE;
SdTestResult sdTestResult = SD_TEST_NOT_RUN;
const char *styleNames[] = {"Simple", "Classic", "Advanced"};
const char *styleMenuNames[] = {"Simple", "Classic", "Advanced"};

uint8_t selected = 0;
uint8_t firstVisible = 0;
uint8_t editField = 0;
uint8_t intervalField = 0;
uint8_t clockField = 0;
uint8_t intervalHours = 6;
uint8_t intervalMinutes = 0;
uint8_t valveSeconds = 5;
bool pendingAutoMode = true;
CyclePhase cyclePhase = CYCLE_IDLE;
bool cycleActive = false;
uint32_t cyclePhaseStartedAt = 0;
uint32_t cycleValveDurationMs = 0;
uint32_t nextAutoCycleAt = 0;
bool autoScheduleActive = false;
bool relayTestBlocked = false;
constexpr uint16_t COMPRESSOR_PRECHARGE_MS = 2000;
constexpr uint16_t SETTINGS_MAGIC = 0x5643;
constexpr uint8_t SETTINGS_VERSION = 1;

DateTime editTime(2026, 1, 1, 12, 0, 0);

bool rtcAvailable = false;
bool sdAvailable = false;
bool autoMode = true;
bool compressorOn = false;
bool valveOpen = false;
bool estopLatched = true;
bool estopCircuitClosed = false;

uint32_t lastUiTick = 0;
bool uiDirty = true;
uint32_t lastButtonChange = 0;
bool lastButtonReading = HIGH;
bool stableButton = HIGH;
bool buttonPressedEvent = false;

uint8_t encoderPrevious = 0;
int8_t encoderAccum = 0;
uint8_t relayTestChannel = 0;
bool relayTestActive = false;
uint32_t relayTestStartedAt = 0;

// Quadrature decoder. Left/CCW decrements menu selection.
const int8_t encoderTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

void drawHome();
void drawSimpleHome();
void drawCriticalHome();
void drawDescriptiveHome();
void drawMenu();
void drawEdit();
void drawClock();
void drawStyle();
void drawMode();
void drawSdTest();
void drawRelayTest();
void drawEstopOverride();
void onEncoderStep(int8_t direction);
uint32_t getIntervalSeconds();

uint8_t relayPin(uint8_t channel) {
  return channel == 0 ? RELAY_1_PIN : RELAY_2_PIN;
}

uint8_t menuCount() {
  // Manual mode exposes the one-shot Start Cycle item before Back.
  return autoMode ? MENU_ITEM_COUNT - 1 : MENU_ITEM_COUNT;
}

const char *menuItemLabel(uint8_t item) {
  if (autoMode && item == MENU_ITEM_COUNT - 2) {
    return menuItems[MENU_ITEM_COUNT - 1];
  }
  return menuItems[item];
}

uint32_t intervalMilliseconds() {
  uint32_t periodSeconds = getIntervalSeconds();
  if (periodSeconds == 0) periodSeconds = 60;
  return periodSeconds * 1000UL;
}

void scheduleNextAutoCycle() {
  if (autoMode && !estopLatched) {
    nextAutoCycleAt = millis() + intervalMilliseconds();
    autoScheduleActive = true;
  } else {
    autoScheduleActive = false;
  }
}

void setRelay(uint8_t channel, bool energized) {
  digitalWrite(relayPin(channel),
               energized && !estopLatched ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

void beginCycle() {
  if (estopLatched || cycleActive || relayTestActive) return;

  // Channel 1 starts first. Channel 2 (valve) opens after the 2-second
  // compressor lead-in, then closes after the user-configured valve time.
  cycleActive = true;
  cyclePhase = CYCLE_COMPRESSOR_DELAY;
  cyclePhaseStartedAt = millis();
  cycleValveDurationMs = (uint32_t)valveSeconds * 1000UL;
  compressorOn = true;
  valveOpen = false;
  setRelay(0, true);
  setRelay(1, false);
  uiDirty = true;
}

void abortCycleAndDisableOutputs() {
  setRelay(0, false);
  setRelay(1, false);
  compressorOn = false;
  valveOpen = false;
  cycleActive = false;
  cyclePhase = CYCLE_IDLE;
  autoScheduleActive = false;
}

void pollCycle() {
  if (!cycleActive || estopLatched) return;

  uint32_t now = millis();
  if (cyclePhase == CYCLE_COMPRESSOR_DELAY &&
      now - cyclePhaseStartedAt >= COMPRESSOR_PRECHARGE_MS) {
    setRelay(1, true);
    valveOpen = true;
    cyclePhase = CYCLE_VALVE_OPEN;
    cyclePhaseStartedAt = now;
    uiDirty = true;
  } else if (cyclePhase == CYCLE_VALVE_OPEN &&
             now - cyclePhaseStartedAt >= cycleValveDurationMs) {
    setRelay(0, false);
    setRelay(1, false);
    compressorOn = false;
    valveOpen = false;
    cycleActive = false;
    cyclePhase = CYCLE_IDLE;
    // If a long cycle outlasted the next automatic start deadline, defer the
    // next cycle so completion still leaves a real compressor-off interval.
    if (autoMode && autoScheduleActive &&
        (int32_t)(now - nextAutoCycleAt) >= 0) {
      nextAutoCycleAt = now + intervalMilliseconds();
    }
    // End the compressor and valve commands together when the timed cycle
    // completes. The E-stop remains the independent immediate-stop path.
    uiDirty = true;
  }
}

void pollAutoCycle() {
  if (!autoMode || estopLatched || cycleActive || relayTestActive ||
      !autoScheduleActive) return;
  if ((int32_t)(millis() - nextAutoCycleAt) >= 0) {
    beginCycle();
    if (cycleActive) {
      nextAutoCycleAt = millis() + intervalMilliseconds();
    }
  }
}

void formatCycleStatus(char *buffer, size_t bufferSize) {
  if (estopLatched) {
    snprintf(buffer, bufferSize, "E-STOP");
  } else if (cycleActive && cyclePhase == CYCLE_COMPRESSOR_DELAY) {
    snprintf(buffer, bufferSize, "COMP START");
  } else if (cycleActive && cyclePhase == CYCLE_VALVE_OPEN) {
    snprintf(buffer, bufferSize, "VALVE OPEN");
  } else if (!autoMode) {
    snprintf(buffer, bufferSize, "MANUAL");
  } else if (autoScheduleActive) {
    int32_t remainingMs = (int32_t)(nextAutoCycleAt - millis());
    uint32_t remainingMinutes = remainingMs > 0
      ? ((uint32_t)remainingMs + 59999UL) / 60000UL : 0;
    snprintf(buffer, bufferSize, "NEXT %02lu:%02lu",
             remainingMinutes / 60UL, remainingMinutes % 60UL);
  } else {
    snprintf(buffer, bufferSize, "AUTO WAIT");
  }
}

void startRelayTest(uint8_t channel) {
  relayTestChannel = channel;
  if (digitalRead(ESTOP_PIN) != LOW) {
    estopLatched = true;
  }
  if (estopLatched) {
    relayTestActive = false;
    page = PAGE_ESTOP_OVERRIDE;
    uiDirty = true;
    return;
  }

  if (cycleActive || compressorOn || valveOpen) {
    relayTestActive = false;
    relayTestBlocked = true;
    page = PAGE_RELAY_TEST;
    uiDirty = true;
    return;
  }

  relayTestBlocked = false;
  relayTestActive = true;
  relayTestStartedAt = millis();
  setRelay(relayTestChannel, true);
  page = PAGE_RELAY_TEST;
  uiDirty = true;
}

void stopRelayTest() {
  if (relayTestActive) {
    setRelay(relayTestChannel, false);
    relayTestActive = false;
  }
}

void pollEmergencyStop() {
  // The NC loop is fail-safe for an open wire: either case latches a stop.
  bool closed = digitalRead(ESTOP_PIN) == LOW;
  if (closed != estopCircuitClosed) {
    estopCircuitClosed = closed;
    uiDirty = true;
  }

  if (!closed) {
    if (!estopLatched || relayTestActive || page != PAGE_ESTOP_OVERRIDE) {
      uiDirty = true;
    }
    estopLatched = true;
    stopRelayTest();
    abortCycleAndDisableOutputs();
    page = PAGE_ESTOP_OVERRIDE;
  }
}

void pollRelayTest() {
  if (relayTestActive && millis() - relayTestStartedAt >= RELAY_TEST_DURATION_MS) {
    stopRelayTest();
    uiDirty = true;
  }
}

uint32_t getIntervalSeconds() {
  return ((uint32_t)intervalHours * 60UL + intervalMinutes) * 60UL;
}

uint8_t settingsChecksum(const StoredSettings &settings) {
  uint8_t checksum = 0xA5;
  checksum ^= (uint8_t)(settings.magic & 0xFF);
  checksum ^= (uint8_t)(settings.magic >> 8);
  checksum ^= settings.version;
  checksum ^= settings.intervalHours;
  checksum ^= settings.intervalMinutes;
  checksum ^= settings.valveSeconds;
  checksum ^= settings.autoMode;
  checksum ^= settings.interfaceStyle;
  return checksum;
}

void saveSettings() {
  StoredSettings settings = {};
  settings.magic = SETTINGS_MAGIC;
  settings.version = SETTINGS_VERSION;
  settings.intervalHours = intervalHours;
  settings.intervalMinutes = intervalMinutes;
  settings.valveSeconds = valveSeconds;
  settings.autoMode = autoMode ? 1 : 0;
  settings.interfaceStyle = (uint8_t)interfaceStyle;
  settings.checksum = settingsChecksum(settings);
  EEPROM.put(0, settings);
}

void loadSettings() {
  StoredSettings settings = {};
  EEPROM.get(0, settings);

  bool valid = settings.magic == SETTINGS_MAGIC &&
               settings.version == SETTINGS_VERSION &&
               settings.checksum == settingsChecksum(settings) &&
               settings.intervalHours <= 24 &&
               settings.intervalMinutes <= 59 &&
               !(settings.intervalHours == 24 && settings.intervalMinutes != 0) &&
               (settings.intervalHours != 0 || settings.intervalMinutes != 0) &&
               settings.valveSeconds >= 1 && settings.valveSeconds <= 60 &&
               settings.autoMode <= 1 &&
               settings.interfaceStyle <= STYLE_DESCRIPTIVE;
  if (!valid) return;

  intervalHours = settings.intervalHours;
  intervalMinutes = settings.intervalMinutes;
  valveSeconds = settings.valveSeconds;
  autoMode = settings.autoMode != 0;
  interfaceStyle = (UiStyle)settings.interfaceStyle;
}

uint8_t daysInMonth(int year, int month) {
  static const uint8_t monthDays[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
    return 29;
  }
  return monthDays[month - 1];
}

SdTestResult runSdCardTest() {
  Serial.println(F("SD test: checking card communication"));
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  Sd2Card diagnosticCard;
  if (!diagnosticCard.init(SPI_HALF_SPEED, SD_CS)) {
    sdAvailable = false;
    sdTestResult = SD_TEST_CARD_FAIL;
    Serial.println(F("SD test failed: card did not respond over SPI."));
    Serial.print(F("SD error code: 0x"));
    if (diagnosticCard.errorCode() < 0x10) Serial.print('0');
    Serial.println(diagnosticCard.errorCode(), HEX);
    Serial.print(F("SD error data: 0x"));
    if (diagnosticCard.errorData() < 0x10) Serial.print('0');
    Serial.println(diagnosticCard.errorData(), HEX);
    return sdTestResult;
  }

  Serial.println(F("SD card communication OK; checking FAT volume"));
  SdVolume diagnosticVolume;
  if (!diagnosticVolume.init(diagnosticCard)) {
    sdAvailable = false;
    sdTestResult = SD_TEST_VOLUME_FAIL;
    Serial.println(F("SD card responds, but no FAT16/FAT32 volume was found."));
    return sdTestResult;
  }

  SdFile diagnosticRoot;
  if (!diagnosticRoot.openRoot(diagnosticVolume)) {
    sdAvailable = false;
    sdTestResult = SD_TEST_ROOT_FAIL;
    Serial.println(F("FAT volume found, but its root directory could not be opened."));
    return sdTestResult;
  }
  diagnosticRoot.close();

  if (!SD.begin(SD_CS)) {
    sdAvailable = false;
    sdTestResult = SD_TEST_API_FAIL;
    Serial.println(F("Card and FAT checks passed, but SD.begin failed."));
    return sdTestResult;
  }

  if (SD.exists(SD_TEST_FILE) && !SD.remove(SD_TEST_FILE)) {
    sdAvailable = false;
    sdTestResult = SD_TEST_WRITE_FAIL;
    Serial.println(F("SD test failed: could not clear old test file."));
    return sdTestResult;
  }

  File output = SD.open(SD_TEST_FILE, FILE_WRITE);
  if (!output) {
    sdAvailable = false;
    sdTestResult = SD_TEST_CREATE_FAIL;
    Serial.println(F("SD test failed: could not create test file."));
    return sdTestResult;
  }

  size_t expectedLength = sizeof(SD_TEST_TEXT) - 1;
  size_t bytesWritten = output.write((const uint8_t *)SD_TEST_TEXT, expectedLength);
  output.close();
  if (bytesWritten != expectedLength) {
    SD.remove(SD_TEST_FILE);
    sdAvailable = false;
    sdTestResult = SD_TEST_WRITE_FAIL;
    Serial.println(F("SD test failed: write was incomplete."));
    return sdTestResult;
  }

  File input = SD.open(SD_TEST_FILE, FILE_READ);
  if (!input) {
    SD.remove(SD_TEST_FILE);
    sdAvailable = false;
    sdTestResult = SD_TEST_READ_FAIL;
    Serial.println(F("SD test failed: could not reopen test file."));
    return sdTestResult;
  }

  char readback[sizeof(SD_TEST_TEXT)];
  size_t bytesRead = input.readBytes(readback, expectedLength);
  input.close();
  readback[bytesRead] = '\0';

  if (bytesRead != expectedLength || strcmp(readback, SD_TEST_TEXT) != 0) {
    SD.remove(SD_TEST_FILE);
    sdAvailable = false;
    sdTestResult = SD_TEST_DATA_FAIL;
    Serial.println(F("SD test failed: read-back data did not match."));
    return sdTestResult;
  }

  SD.remove(SD_TEST_FILE);
  sdAvailable = true;
  sdTestResult = SD_TEST_PASS;
  Serial.println(F("SD test passed: card initialized; test file wrote and read correctly."));
  return sdTestResult;
}

const char *sdTestResultText() {
  switch (sdTestResult) {
    case SD_TEST_PASS: return "PASS";
    case SD_TEST_CARD_FAIL: return "CARD FAIL";
    case SD_TEST_VOLUME_FAIL: return "FAT FAIL";
    case SD_TEST_ROOT_FAIL: return "ROOT FAIL";
    case SD_TEST_API_FAIL: return "SD API FAIL";
    case SD_TEST_CREATE_FAIL: return "CREATE FAILED";
    case SD_TEST_WRITE_FAIL: return "WRITE FAILED";
    case SD_TEST_READ_FAIL: return "READ FAILED";
    case SD_TEST_DATA_FAIL: return "DATA MISMATCH";
    default: return "NOT RUN";
  }
}

void pollEncoder() {
  uint8_t now = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  encoderAccum += encoderTable[(encoderPrevious << 2) | now];
  encoderPrevious = now;

  // Wait for one complete quadrature cycle per step. Screen redraws are
  // throttled below, so polling can keep sampling while the knob turns.
  const int8_t threshold = 4;
  if (encoderAccum >= threshold) {
    encoderAccum = 0;
    onEncoderStep(1);
  } else if (encoderAccum <= -threshold) {
    encoderAccum = 0;
    onEncoderStep(-1);
  }
}

void onEncoderStep(int8_t direction) {
  if (page == PAGE_MENU) {
    uint8_t count = menuCount();
    selected = (selected + count + direction) % count;
  } else if (page == PAGE_EDIT) {
    if (editField == 0) {
      if (intervalField == 0) {
        intervalHours = constrain((int)intervalHours + direction, 0, 24);
        if (intervalHours == 24) intervalMinutes = 0;
      } else if (intervalHours < 24) {
        if (intervalHours == 0) {
          intervalMinutes = constrain((int)intervalMinutes + direction, 1, 59);
        } else {
          intervalMinutes = (intervalMinutes + direction + 60) % 60;
        }
      }
    } else {
      valveSeconds = constrain((int)valveSeconds + direction, 1, 60);
    }
  } else if (page == PAGE_CLOCK) {
    int y = editTime.year();
    int mo = editTime.month();
    int d = editTime.day();
    int h = editTime.hour();
    int mi = editTime.minute();

    if (clockField == 0) y = constrain(y + direction, 2000, 2099);
    if (clockField == 1) mo = constrain(mo + direction, 1, 12);
    if (clockField == 2) d = constrain(d + direction, 1, daysInMonth(y, mo));
    if (clockField == 3) h = (h + direction + 24) % 24;
    if (clockField == 4) mi = (mi + direction + 60) % 60;

    // Keep the edited date valid when changing month or year (e.g. Feb 31).
    if (d > daysInMonth(y, mo)) d = daysInMonth(y, mo);

    editTime = DateTime(y, mo, d, h, mi, 0);
  } else if (page == PAGE_STYLE) {
    int nextStyle = (int)interfaceStyle + direction;
    if (nextStyle < STYLE_SIMPLE) nextStyle = STYLE_DESCRIPTIVE;
    if (nextStyle > STYLE_DESCRIPTIVE) nextStyle = STYLE_SIMPLE;
    interfaceStyle = (UiStyle)nextStyle;
  } else if (page == PAGE_MODE) {
    pendingAutoMode = !pendingAutoMode;
  }
  uiDirty = true;
}

void pollButton() {
  bool reading = digitalRead(ENC_BUTTON);

  if (reading != lastButtonReading) {
    lastButtonChange = millis();
  }

  if (millis() - lastButtonChange > 25 && reading != stableButton) {
    stableButton = reading;
    if (stableButton == LOW) {
      buttonPressedEvent = true;
    }
  }

  lastButtonReading = reading;
}

void activateSelected() {
  uiDirty = true;

  if (page == PAGE_ESTOP_OVERRIDE) {
    // The switch must be released (NC circuit closed) before the encoder
    // acknowledges the stop. Clearing this latch never starts a relay.
    if (digitalRead(ESTOP_PIN) == LOW) {
      estopCircuitClosed = true;
      estopLatched = false;
      scheduleNextAutoCycle();
      page = PAGE_HOME;
    } else {
      estopCircuitClosed = false;
      estopLatched = true;
    }
    return;
  }

  if (page == PAGE_HOME) {
    page = PAGE_MENU;
    return;
  }

  if (page == PAGE_MODE) {
    autoMode = pendingAutoMode;
    saveSettings();
    if (autoMode) {
      scheduleNextAutoCycle();
    } else {
      autoScheduleActive = false;
    }
    if (selected >= menuCount()) selected = menuCount() - 1;
    page = PAGE_MENU;
    return;
  }

  if (page == PAGE_MENU) {
    switch (selected) {
      case 0:
        page = PAGE_STYLE;
        break;

      case 1:
        pendingAutoMode = autoMode;
        page = PAGE_MODE;
        break;

      case 2:
        editField = 0;
        intervalField = 0;
        page = PAGE_EDIT;
        break;

      case 3:
        editField = 1;
        page = PAGE_EDIT;
        break;

      case 4:
        if (rtcAvailable) {
          editTime = rtc.now();
        } else {
          editTime = DateTime(2026, 1, 1, 12, 0, 0);
        }
        clockField = 0;
        page = PAGE_CLOCK;
        break;

      case 5:
        runSdCardTest();
        page = PAGE_SD_TEST;
        break;

      case 6:
        startRelayTest(0);
        break;

      case 7:
        startRelayTest(1);
        break;

      case 8:
        if (!autoMode) {
          beginCycle();
          page = PAGE_HOME;
        } else {
          page = PAGE_HOME;  // The final item is Back in automatic mode.
        }
        break;

      default:
        page = PAGE_HOME;
        break;
    }
    return;
  }

  if (page == PAGE_EDIT) {
    if (editField == 0 && intervalField == 0) {
      intervalField = 1;
      return;
    }
    if (editField == 0 && intervalHours == 0 && intervalMinutes == 0) {
      intervalMinutes = 1;
    }
    page = PAGE_MENU;
    intervalField = 0;
    saveSettings();
    if (editField == 0) scheduleNextAutoCycle();
    return;
  }

  if (page == PAGE_STYLE) {
    saveSettings();
    page = PAGE_MENU;
    return;
  }

  if (page == PAGE_SD_TEST) {
    page = PAGE_MENU;
    return;
  }

  if (page == PAGE_RELAY_TEST) {
    stopRelayTest();
    relayTestBlocked = false;
    page = PAGE_MENU;
    return;
  }

  if (page == PAGE_CLOCK) {
    if (clockField < 4) {
      clockField++;
    } else {
      if (rtcAvailable) {
        rtc.adjust(editTime);  // Updates RTC only; never operates actuators.
      }
      page = PAGE_MENU;
    }
  }
}

void drawHome() {
  if (interfaceStyle == STYLE_SIMPLE) {
    drawSimpleHome();
  } else if (interfaceStyle == STYLE_CRITICAL) {
    drawCriticalHome();
  } else {
    drawDescriptiveHome();
  }
}

void drawSimpleHome() {
  DateTime now = rtcAvailable
    ? rtc.now()
    : DateTime(2000, 1, 1, 0, 0, 0);

  char line[32];
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "FLY CONTROL");
  u8g2.drawStr(estopLatched ? 96 : 85, 9,
               estopLatched ? "STOP" : "BETA");
  u8g2.drawHLine(0, 12, 128);

  // Large clock on the left; operating state grouped on the right.
  u8g2.setFont(u8g2_font_10x20_tf);
  if (rtcAvailable) {
    snprintf(line, sizeof(line), "%02u:%02u", now.hour(), now.minute());
    u8g2.drawStr(0, 34, line);
  } else {
    u8g2.drawStr(0, 34, "--:--");
  }

  u8g2.setFont(u8g2_font_6x10_tf);
  if (rtcAvailable) {
    snprintf(line, sizeof(line), "%04u-%02u-%02u",
             now.year(), now.month(), now.day());
  } else {
    snprintf(line, sizeof(line), "NO RTC");
  }
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 44, line);

  u8g2.setFont(u8g2_font_6x10_tf);
  snprintf(line, sizeof(line), "MODE: %s", autoMode ? "AUTO" : "MANUAL");
  u8g2.drawStr(62, 22, line);
  snprintf(line, sizeof(line), "COMP CMD: %s", compressorOn ? "ON" : "OFF");
  u8g2.drawStr(53, 33, line);
  snprintf(line, sizeof(line), "VALVE: %s", valveOpen ? "OPEN" : "CLOSED");
  u8g2.drawStr(50, 44, line);

  u8g2.drawHLine(0, 48, 128);

  formatCycleStatus(line, sizeof(line));
  u8g2.drawStr(0, 61, line);
  u8g2.drawStr(94, 61, sdAvailable ? "SD OK" : "SD --");
}

void drawCriticalHome() {
  char line[32];

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "FLY CONTROL");
  u8g2.drawStr(estopLatched ? 96 : 85, 9,
               estopLatched ? "STOP" : "BETA");
  u8g2.drawHLine(0, 12, 128);

  snprintf(line, sizeof(line), "MODE: %s", autoMode ? "AUTO" : "MANUAL");
  u8g2.drawStr(0, 24, line);
  u8g2.drawStr(82, 24, rtcAvailable ? "RTC: OK" : "RTC: ERR");

  snprintf(line, sizeof(line), "COMP RELAY: %s", compressorOn ? "ON" : "OFF");
  u8g2.drawStr(0, 35, line);
  u8g2.drawStr(82, 35, sdAvailable ? "SD: OK" : "SD: ERR");

  snprintf(line, sizeof(line), "VALVE: %s", valveOpen ? "OPEN" : "CLOSED");
  u8g2.drawStr(0, 46, line);

  u8g2.drawHLine(0, 50, 128);
  formatCycleStatus(line, sizeof(line));
  u8g2.drawStr(0, 63, line);
}

void drawDescriptiveHome() {
  DateTime now = rtcAvailable
    ? rtc.now()
    : DateTime(2000, 1, 1, 0, 0, 0);
  char line[32];

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "FLY CONTROL");
  u8g2.drawStr(estopLatched ? 96 : 85, 9,
               estopLatched ? "STOP" : "BETA");
  u8g2.drawHLine(0, 12, 128);

  u8g2.setFont(u8g2_font_5x7_tf);
  if (rtcAvailable) {
    snprintf(line, sizeof(line), "TIME %02u:%02u:%02u",
             now.hour(), now.minute(), now.second());
  } else {
    snprintf(line, sizeof(line), "TIME --:--:--  RTC ERROR");
  }
  u8g2.drawStr(0, 20, line);

  if (rtcAvailable) {
    snprintf(line, sizeof(line), "DATE %04u-%02u-%02u",
             now.year(), now.month(), now.day());
  } else {
    snprintf(line, sizeof(line), "DATE UNAVAILABLE");
  }
  u8g2.drawStr(0, 27, line);

  snprintf(line, sizeof(line), "MODE %s", autoMode ? "AUTO" : "MANUAL");
  u8g2.drawStr(0, 34, line);
  snprintf(line, sizeof(line), "COMPRESSOR CMD %s", compressorOn ? "ON" : "OFF");
  u8g2.drawStr(0, 41, line);
  snprintf(line, sizeof(line), "VALVE %s", valveOpen ? "OPEN" : "CLOSED");
  u8g2.drawStr(0, 48, line);
  snprintf(line, sizeof(line), "INTERVAL %uh%02um  VALVE %us",
           intervalHours, intervalMinutes, valveSeconds);
  u8g2.drawStr(0, 55, line);

  char cycleStatus[16];
  formatCycleStatus(cycleStatus, sizeof(cycleStatus));
  snprintf(line, sizeof(line), "%-14s RTC %s SD %s", cycleStatus,
           rtcAvailable ? "OK" : "ERR", sdAvailable ? "OK" : "ERR");
  u8g2.drawStr(0, 62, line);
}

void drawMenu() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "SETTINGS");
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(78, 9, estopLatched ? "ESTOP: STOP" : "BETA");
  u8g2.drawHLine(0, 12, 128);

  uint8_t count = menuCount();
  if (selected >= count) selected = count - 1;

  if (selected < firstVisible) {
    firstVisible = selected;
  }
  if (selected >= firstVisible + 3) {
    firstVisible = selected - 2;
  }

  u8g2.setFont(u8g2_font_7x14_tf);
  for (uint8_t row = 0; row < 3 && firstVisible + row < count; row++) {
    uint8_t item = firstVisible + row;
    uint8_t y = 29 + row * 16;

    if (item == selected) {
      u8g2.drawBox(0, y - 13, 128, 15);
      u8g2.setDrawColor(0);
    }

    u8g2.drawStr(2, y, menuItemLabel(item));

    if (item == 0) {
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(64, y, styleMenuNames[interfaceStyle]);
      u8g2.setFont(u8g2_font_7x14_tf);
    }

    if (item == 1) {
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(84, y, autoMode ? "AUTO" : "MANUAL");
      u8g2.setFont(u8g2_font_7x14_tf);
    }

    if (item == 2) {
      char value[16];
      snprintf(value, sizeof(value), "%uh %02um", intervalHours, intervalMinutes);
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(84, y, value);
      u8g2.setFont(u8g2_font_7x14_tf);
    }

    if (item == 3) {
      char value[12];
      snprintf(value, sizeof(value), "%us", valveSeconds);
      u8g2.drawStr(103, y, value);
    }

    u8g2.setDrawColor(1);
  }

}

void drawStyle() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "DISPLAY");
  u8g2.drawHLine(0, 13, 128);

  // Center the selected style name so Simple, Classic, and Advanced all use
  // the same preview layout and the old "Descriptive" label cannot linger.
  u8g2.setFont(u8g2_font_10x20_tf);
  const char *styleName = styleNames[interfaceStyle];
  int16_t nameWidth = u8g2.getStrWidth(styleName);
  u8g2.drawStr((128 - nameWidth) / 2, 39, styleName);

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 62, "Turn: preview  Press: select");
}

void drawMode() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "OPERATING MODE");
  u8g2.drawHLine(0, 13, 128);

  const char *modeName = pendingAutoMode ? "AUTOMATIC" : "MANUAL";
  u8g2.setFont(u8g2_font_10x20_tf);
  int16_t modeWidth = u8g2.getStrWidth(modeName);
  u8g2.drawStr((128 - modeWidth) / 2, 38, modeName);

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 51, pendingAutoMode
    ? "Runs cycles at the interval"
    : "Use Start Cycle in Settings");
  u8g2.drawStr(0, 62, "Turn: choose|Press: save");
}

void drawSdTest() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "SD CARD TEST");
  u8g2.drawHLine(0, 13, 128);

  u8g2.setFont(u8g2_font_7x14_tf);
  u8g2.drawStr(4, 34, sdTestResultText());

  u8g2.setFont(u8g2_font_5x7_tf);
  if (sdTestResult == SD_TEST_PASS) {
    u8g2.drawStr(0, 48, "Card read and write verified");
    u8g2.drawStr(0, 56, "Temporary test file removed");
  } else if (sdTestResult == SD_TEST_CARD_FAIL) {
    u8g2.drawStr(0, 46, "SPI COMM FAILED");
    u8g2.drawStr(0, 55, "Check CS/SPI, VCC, GND, card");
  } else if (sdTestResult == SD_TEST_VOLUME_FAIL) {
    u8g2.drawStr(0, 46, "Card OK; FAT mount failed");
    u8g2.drawStr(0, 55, "Use FAT16 or FAT32 format");
  } else if (sdTestResult == SD_TEST_ROOT_FAIL) {
    u8g2.drawStr(0, 49, "FAT OK; root open failed");
  } else if (sdTestResult == SD_TEST_API_FAIL) {
    u8g2.drawStr(0, 49, "Card OK; SD API failed");
  } else if (sdTestResult == SD_TEST_CREATE_FAIL) {
    u8g2.drawStr(0, 49, "Card started; create failed");
  } else if (sdTestResult == SD_TEST_WRITE_FAIL) {
    u8g2.drawStr(0, 49, "Card started; write failed");
  } else if (sdTestResult == SD_TEST_READ_FAIL) {
    u8g2.drawStr(0, 49, "Card started; read failed");
  } else if (sdTestResult == SD_TEST_DATA_FAIL) {
    u8g2.drawStr(0, 49, "Read-back data did not match");
  } else {
    u8g2.drawStr(0, 49, "Test has not run");
  }

  u8g2.drawStr(0, 63, "Press to return");
}

void drawRelayTest() {
  char line[24];
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "RELAY CLICK TEST");
  u8g2.drawHLine(0, 13, 128);

  if (estopLatched) {
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(12, 34, "STOP ACTIVE");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 51, "Release button, then reset");
    u8g2.drawStr(0, 63, "Press to return");
    return;
  }

  if (relayTestBlocked) {
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(3, 34, "OUTPUT IN USE");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 51, "Finish cycle or trip E-stop");
    u8g2.drawStr(0, 63, "Press to return");
    return;
  }

  u8g2.setFont(u8g2_font_10x20_tf);
  snprintf(line, sizeof(line), "RELAY %u", relayTestChannel + 1);
  u8g2.drawStr(24, 37, line);

  u8g2.setFont(u8g2_font_6x10_tf);
  if (relayTestActive) {
    u8g2.drawStr(31, 50, "ON - listen for click");
    u8g2.drawStr(0, 63, "Auto OFF in 0.7 sec");
  } else {
    u8g2.drawStr(42, 50, "TEST COMPLETE");
    u8g2.drawStr(0, 63, "Press to return");
  }
}

void drawEstopOverride() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "------------------------------");
  u8g2.drawHLine(0, 13, 128);

  if (!estopCircuitClosed) {
    u8g2.setFont(u8g2_font_10x20_tf);
    u8g2.drawStr(12, 37, "E-STOP");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 51, "Reset switch");
    u8g2.drawStr(0, 62, "Then click the encoder");
  } else {
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(6, 34, "UNARMED");
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 49, "Click encoder to clear stop");
    u8g2.drawStr(0, 61, "Outputs stay OFF");
  }
}

void drawEdit() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, editField == 0 ? "SET INTERVAL" : "SETTINGS VALUE");
  u8g2.drawHLine(0, 13, 128);

  if (editField == 0) {
    char value[12];
    snprintf(value, sizeof(value), "%02u:%02u", intervalHours, intervalMinutes);
    u8g2.setFont(u8g2_font_10x20_tf);
    u8g2.drawStr(39, 42, value);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(38, 24, "HOURS");
    u8g2.drawStr(68, 24, "MINUTES");
    u8g2.drawHLine(intervalField == 0 ? 39 : 69, 44, 20);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 62, "Turn: change  Press: next/save");
    return;
  }

  u8g2.setFont(u8g2_font_7x14_tf);
  char value[24];

  snprintf(value, sizeof(value), "Valve Time: %u s", valveSeconds);

  u8g2.drawStr(5, 34, value);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 53, "Turn to change; press done");
}

void drawClock() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, rtcAvailable ? "SET RTC CLOCK" : "RTC NOT FOUND");
  u8g2.drawHLine(0, 13, 128);

  char value[28];

  snprintf(value, sizeof(value), "%04u-%02u-%02u",
           editTime.year(), editTime.month(), editTime.day());
  u8g2.drawStr(20, 29, value);

  snprintf(value, sizeof(value), "%02u:%02u",
           editTime.hour(), editTime.minute());
  u8g2.drawStr(44, 45, value);

  if (clockField <= 2) {
    u8g2.drawHLine(20 + clockField * 12, 31,
                   clockField == 0 ? 24 : 12);
  } else {
    u8g2.drawHLine(clockField == 3 ? 44 : 62, 47, 12);
  }

  u8g2.drawStr(0, 62, "Turn: change  Press: next/save");
}

void setup() {
  Serial.begin(115200);
  loadSettings();

  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_BUTTON, INPUT_PULLUP);

  // Set the output latch inactive before enabling outputs so the active-low
  // relay inputs do not receive an unintended ON pulse during initialization.
  digitalWrite(RELAY_1_PIN, RELAY_OFF_LEVEL);
  digitalWrite(RELAY_2_PIN, RELAY_OFF_LEVEL);
  pinMode(RELAY_1_PIN, OUTPUT);
  pinMode(RELAY_2_PIN, OUTPUT);

  pinMode(ESTOP_PIN, INPUT_PULLUP);
  estopCircuitClosed = digitalRead(ESTOP_PIN) == LOW;
  estopLatched = !estopCircuitClosed;
  if (estopLatched) {
    page = PAGE_ESTOP_OVERRIDE;
  }

  encoderPrevious = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);

  Wire.begin();  // Mega SDA=20, SCL=21
  rtcAvailable = rtc.begin();

  // Mega hardware SS must remain an output for SPI operation.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  runSdCardTest();

  u8g2.begin();
  u8g2.setContrast(180);
  scheduleNextAutoCycle();
}

void loop() {
  pollEncoder();
  pollButton();
  pollEmergencyStop();
  pollCycle();
  pollAutoCycle();
  pollRelayTest();

  if (buttonPressedEvent) {
    buttonPressedEvent = false;
    activateSelected();
  }

  // Refresh the live home screen on its timer. Draw other pages only when
  // opened or changed, so an idle settings menu does not keep redrawing.
  const bool homeRefreshDue =
      page == PAGE_HOME && millis() - lastUiTick >= 250;
  const bool changedPageNeedsDraw = uiDirty && page != PAGE_HOME;
  const bool changedHomeNeedsDraw =
      uiDirty && page == PAGE_HOME && millis() - lastUiTick >= 40;

  if (homeRefreshDue || changedPageNeedsDraw || changedHomeNeedsDraw) {
    lastUiTick = millis();
    uiDirty = false;

    u8g2.clearBuffer();

    if (page == PAGE_HOME) {
      drawHome();
    } else if (page == PAGE_MENU) {
      drawMenu();
    } else if (page == PAGE_EDIT) {
      drawEdit();
    } else if (page == PAGE_CLOCK) {
      drawClock();
    } else if (page == PAGE_SD_TEST) {
      drawSdTest();
    } else if (page == PAGE_MODE) {
      drawMode();
    } else if (page == PAGE_RELAY_TEST) {
      drawRelayTest();
    } else if (page == PAGE_ESTOP_OVERRIDE) {
      drawEstopOverride();
    } else {
      drawStyle();
    }

    u8g2.sendBuffer();
  }
}
