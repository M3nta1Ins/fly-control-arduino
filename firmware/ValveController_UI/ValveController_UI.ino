#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <U8g2lib.h>
#include <RTClib.h>

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

// UI-only: no relay, compressor, or valve output pins are configured.
const char *menuItems[] = {
  "Interval", "Valve Time", "Start", "Stop", "Set RTC Clock", "Back"
};
constexpr uint8_t MENU_COUNT = sizeof(menuItems) / sizeof(menuItems[0]);

enum UiPage : uint8_t { PAGE_HOME, PAGE_MENU, PAGE_EDIT, PAGE_CLOCK };
UiPage page = PAGE_HOME;

uint8_t selected = 0;
uint8_t firstVisible = 0;
uint8_t editField = 0;
uint8_t intervalField = 0;
uint8_t clockField = 0;
uint8_t intervalHours = 6;
uint8_t intervalMinutes = 0;
uint8_t valveSeconds = 5;

DateTime editTime(2026, 1, 1, 12, 0, 0);

bool rtcAvailable = false;
bool sdAvailable = false;
bool autoMode = true;
bool compressorOn = false;
bool valveOpen = false;

uint32_t lastUiTick = 0;
bool uiDirty = true;
uint32_t lastButtonChange = 0;
bool lastButtonReading = HIGH;
bool stableButton = HIGH;
bool buttonPressedEvent = false;

uint8_t encoderPrevious = 0;
int8_t encoderAccum = 0;

// Quadrature decoder. Left/CCW decrements menu selection.
const int8_t encoderTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

void drawHome();
void drawMenu();
void drawEdit();
void drawClock();
void onEncoderStep(int8_t direction);

uint32_t getIntervalSeconds() {
  return ((uint32_t)intervalHours * 60UL + intervalMinutes) * 60UL;
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
    selected = (selected + MENU_COUNT + direction) % MENU_COUNT;
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
    if (clockField == 2) d = constrain(d + direction, 1, 31);
    if (clockField == 3) h = (h + direction + 24) % 24;
    if (clockField == 4) mi = (mi + direction + 60) % 60;

    editTime = DateTime(y, mo, d, h, mi, 0);
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
  if (page == PAGE_HOME) {
    page = PAGE_MENU;
    return;
  }

  if (page == PAGE_MENU) {
    switch (selected) {
      case 0:
        editField = 0;
        intervalField = 0;
        page = PAGE_EDIT;
        break;

      case 1:
        editField = 1;
        page = PAGE_EDIT;
        break;

      case 2:
        // UI placeholder only. No compressor or relay action.
        autoMode = true;
        break;

      case 3:
        // UI placeholder only. No valve or relay action.
        autoMode = false;
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
  DateTime now = rtcAvailable
    ? rtc.now()
    : DateTime(2000, 1, 1, 0, 0, 0);

  char line[32];
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "VALVE CONTROL");
  u8g2.drawStr(85, 9, "UI ONLY");
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
  snprintf(line, sizeof(line), "COMP: %s", compressorOn ? "ON" : "OFF");
  u8g2.drawStr(62, 33, line);
  snprintf(line, sizeof(line), "VALVE: %s", valveOpen ? "OPEN" : "CLOSED");
  u8g2.drawStr(50, 44, line);

  u8g2.drawHLine(0, 48, 128);

  snprintf(line, sizeof(line), "NEXT --:--");

  if (rtcAvailable) {
    uint32_t period = getIntervalSeconds();
    uint32_t elapsed = (uint32_t)now.hour() * 3600UL
                     + (uint32_t)now.minute() * 60UL
                     + now.second();
    uint32_t remaining = period - (elapsed % period);
    snprintf(line, sizeof(line), "NEXT %02lu:%02lu",
             remaining / 3600UL, (remaining / 60UL) % 60UL);
  }
  u8g2.drawStr(0, 61, line);
  u8g2.drawStr(94, 61, sdAvailable ? "SD OK" : "SD --");
}

void drawMenu() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "SETTINGS");
  u8g2.drawHLine(0, 12, 128);

  if (selected < firstVisible) {
    firstVisible = selected;
  }
  if (selected >= firstVisible + 3) {
    firstVisible = selected - 2;
  }

  u8g2.setFont(u8g2_font_7x14_tf);
  for (uint8_t row = 0; row < 3 && firstVisible + row < MENU_COUNT; row++) {
    uint8_t item = firstVisible + row;
    uint8_t y = 29 + row * 16;

    if (item == selected) {
      u8g2.drawBox(0, y - 13, 128, 15);
      u8g2.setDrawColor(0);
    }

    u8g2.drawStr(2, y, menuItems[item]);

    if (item == 0) {
      char value[16];
      snprintf(value, sizeof(value), "%uh %02um", intervalHours, intervalMinutes);
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(84, y, value);
      u8g2.setFont(u8g2_font_7x14_tf);
    }

    if (item == 1) {
      char value[12];
      snprintf(value, sizeof(value), "%us", valveSeconds);
      u8g2.drawStr(103, y, value);
    }

    if (item == 2 || item == 3) {
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(94, y, "UI only");
      u8g2.setFont(u8g2_font_7x14_tf);
    }

    u8g2.setDrawColor(1);
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
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_BUTTON, INPUT_PULLUP);

  encoderPrevious = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);

  Wire.begin();  // Mega SDA=20, SCL=21
  rtcAvailable = rtc.begin();

  // Mega hardware SS must remain an output for SPI operation.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  sdAvailable = SD.begin(SD_CS);

  u8g2.begin();
  u8g2.setContrast(180);
}

void loop() {
  pollEncoder();
  pollButton();

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
    } else {
      drawClock();
    }

    u8g2.sendBuffer();
  }
}
