#include "display.h"
#include "history.h"
#include "lgfx_config.h"
#include "packet.h"
#include "timesync.h"
#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include <math.h>
#include <time.h>

// Po tylu ms bez pakietu dane uznajemy za nieaktualne. Nadajnik wysyła co
// 15 min - limit 35 min toleruje zgubienie jednego pakietu.
static constexpr uint32_t DATA_TIMEOUT_MS = 35UL * 60000;

// Kolory RGB565 (typ uint16_t, żeby LovyanGFX nie potraktował ich jako RGB888)
static constexpr uint16_t C_BG = 0x0000;
static constexpr uint16_t C_HEADER = 0x10A3;
static constexpr uint16_t C_TILE = 0x1905;
static constexpr uint16_t C_LABEL = 0x94D4;
static constexpr uint16_t C_TEXT = 0xFFFF;
static constexpr uint16_t C_TEMP = 0x3C7F;  // niebieski  #3A8DFF
static constexpr uint16_t C_HUM = 0xB31F;   // fioletowy #B060FF
static constexpr uint16_t C_PRESS = 0x468C; // zielony   #40D060
static constexpr uint16_t C_GREEN = 0x56EC;
static constexpr uint16_t C_YELLOW = 0xFE46;
static constexpr uint16_t C_RED = 0xF227;
static constexpr uint16_t C_TRACK = 0x39E7;
static constexpr uint16_t C_GRID = 0x18E3;      // linie pomocnicze wykresu
static constexpr uint16_t C_GRID_MAJOR = 0x3186; // linie z opisem / północ
static constexpr uint16_t C_NAV = 0x0861;        // nieaktywny przycisk

// Układ ekranu w poziomie 480x320
static constexpr int SCR_W = 480;
static constexpr int SCR_H = 320;
static constexpr int HEADER_H = 36;
static constexpr int NAV_H = 40; // pasek 5 przycisków na dole ekranu
static constexpr int NAV_Y = SCR_H - NAV_H;
static constexpr int FOOTER_H = 20;
static constexpr int FOOTER_Y = NAV_Y - FOOTER_H - 2;
static constexpr int TILE_W = 234;
static constexpr int TILE_H = 106;
static constexpr int TILE_X[2] = {4, 242};
static constexpr int TILE_Y[2] = {40, 151};

// Suwak jasności na ekranie informacji
static constexpr int SLIDER_Y = 236; // górna krawędź paska suwaka
static constexpr int SLIDER_H = 40;

// Lista na ekranie informacji (sprite od HEADER_H do SLIDER_Y)
static constexpr int INFO_H = SLIDER_Y - HEADER_H;
static constexpr int INFO_ROW0 = 10; // środek pierwszego wiersza w sprite
static constexpr int INFO_ROW_H = 20;

// Wykres (sprite od HEADER_H do NAV_Y)
static constexpr int CHART_H = NAV_Y - HEADER_H;
static constexpr int PLOT_L = 44, PLOT_R = 470; // obszar danych w sprite
static constexpr int PLOT_T = 30, PLOT_B = CHART_H - 24;
static constexpr uint32_t CHART_SPAN_S = 24UL * 3600;
static constexpr int TRACK_X0 = 130;
static constexpr int TRACK_X1 = 310;
static constexpr int KNOB_R = 12;
// Przycisk "Auto" (jasność z fotorezystora) na prawo od suwaka
static constexpr int AUTO_X = 398, AUTO_W = 74, AUTO_H = 30;

// Jasność podświetlenia (PWM 0..255)
static constexpr uint8_t BRIGHT_MIN = 10; // niżej ekran jest praktycznie czarny
// Poziom bazowy ustawiany suwakiem (zapisywany w NVS); ta wartość jest
// używana tylko przy pierwszym uruchomieniu, zanim cokolwiek zapisano.
static constexpr uint8_t BASE_FIRST_BOOT = 200;

// Automatyczna jasność: jasność = poziom bazowy z suwaka + korekta
// z fotorezystora. Przy średnim oświetleniu (połowa zakresu
// LIGHT_DARK_MV..LIGHT_BRIGHT_MV) korekta jest zerowa, w ciemności
// -AUTO_SWING/2, w pełnym świetle +AUTO_SWING/2. Progi dla fotorezystora
// GL55xx z rezystorem 10 kΩ; bieżące napięcie widać na ekranie informacji.
static constexpr int LIGHT_DARK_MV = 300;
static constexpr int LIGHT_BRIGHT_MV = 2600;
static constexpr int AUTO_SWING = 255 - BRIGHT_MIN;
static constexpr int LIGHT_HYST_MV = 40;     // martwa strefa przeciw migotaniu
static constexpr uint32_t LIGHT_READ_MS = 100;
static constexpr uint32_t BRIGHT_STEP_MS = 30; // płynne przejście jasności

static LGFX lcd;
static LGFX_Sprite tileSpr(&lcd);
static LGFX_Sprite headerSpr(&lcd);
static LGFX_Sprite footerSpr(&lcd);
static LGFX_Sprite sliderSpr(&lcd);
static LGFX_Sprite navSpr(&lcd);
static LGFX_Sprite infoSpr(&lcd);  // PSRAM
static LGFX_Sprite chartSpr(&lcd); // PSRAM

// Kolejność = kolejność przycisków na dolnym pasku
enum class Screen : uint8_t { Main, Temp, Hum, Press, Info, Count };
static constexpr int SCREEN_COUNT = (int)Screen::Count;
static const char *const NAV_LABELS[SCREEN_COUNT] = {"Odczyty", "Temp.",
                                                     "Wilg.", "Cisn.", "Info"};
static const uint16_t NAV_COLORS[SCREEN_COUNT] = {C_GREEN, C_TEMP, C_HUM,
                                                  C_PRESS, C_LABEL};
static Screen screen = Screen::Main;
static bool fullRedraw = true;
static uint8_t ownMac[6];
static uint8_t baseLevel = BASE_FIRST_BOOT; // poziom bazowy (suwak)
static uint8_t savedBase = BASE_FIRST_BOOT;  // ostatnio zapisany w NVS
static uint8_t brightness = BASE_FIRST_BOOT; // aktualna jasność (PWM)
static bool autoBright = false;
static float lightMv = 0;      // napięcie z fotorezystora po filtracji
static int lightUsedMv = 0;    // napięcie, z którego wyliczono korektę
static uint8_t autoTarget = BASE_FIRST_BOOT;

// ---------------------------------------------------------------------------
// Kalibracja dotyku (zapisywana w NVS)

static void runTouchCalibration(uint16_t cal[8]) {
  lcd.fillScreen(C_BG);
  lcd.setTextColor(C_TEXT, C_BG);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.drawString("Kalibracja dotyku", SCR_W / 2, 130);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.drawString("Dotknij kolejno wskazanych rogow ekranu", SCR_W / 2, 170);
  lcd.calibrateTouch(cal, C_TEXT, C_BG, 20);
}

static void initTouch() {
  Preferences prefs;
  prefs.begin("touch", false);
  uint16_t cal[8];
  bool haveCal = prefs.getBytes("cal", cal, sizeof(cal)) == sizeof(cal);

  // Ekran startowy - przytrzymanie dotyku wymusza ponowną kalibrację
  lcd.fillScreen(C_BG);
  lcd.setTextColor(C_TEXT, C_BG);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.drawString("Stacja pogodowa", SCR_W / 2, 130);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(C_LABEL, C_BG);
  lcd.drawString("Przytrzymaj ekran, aby skalibrowac dotyk", SCR_W / 2, 190);

  bool forceCal = false;
  uint32_t start = millis();
  while (millis() - start < 1500) {
    lgfx::touch_point_t tp;
    if (lcd.getTouchRaw(&tp, 1)) {
      forceCal = true;
      break;
    }
    delay(20);
  }

  if (!haveCal || forceCal) {
    if (forceCal) { // poczekaj na puszczenie ekranu
      lgfx::touch_point_t tp;
      while (lcd.getTouchRaw(&tp, 1))
        delay(20);
      delay(200);
    }
    runTouchCalibration(cal);
    prefs.putBytes("cal", cal, sizeof(cal));
    Serial.println("Zapisano kalibracje dotyku");
  }
  lcd.setTouchCalibrate(cal);
  prefs.end();
}

// ---------------------------------------------------------------------------
// Jasność (zapisywana w NVS)

static void loadBrightness() {
  Preferences prefs;
  prefs.begin("display", true);
  uint8_t b = prefs.getUChar("bright", BASE_FIRST_BOOT);
  autoBright = prefs.getBool("auto", false);
  prefs.end();
  baseLevel = savedBase = brightness = b < BRIGHT_MIN ? BRIGHT_MIN : b;
}

static void saveAutoBright() {
  Preferences prefs;
  prefs.begin("display", false);
  prefs.putBool("auto", autoBright);
  prefs.end();
}

// ---------------------------------------------------------------------------
// Automatyczna jasność z fotorezystora

// Korekta jasności (w poziomach PWM) dla napięcia z fotorezystora
static int lightCorrection(int mv) {
  int c = constrain(mv, LIGHT_DARK_MV, LIGHT_BRIGHT_MV);
  return (c - LIGHT_DARK_MV) * AUTO_SWING / (LIGHT_BRIGHT_MV - LIGHT_DARK_MV) -
         AUTO_SWING / 2;
}

// Docelowa jasność: w trybie auto poziom bazowy + korekta, inaczej baza
static void updateTarget() {
  int t = baseLevel + (autoBright ? lightCorrection(lightUsedMv) : 0);
  autoTarget = constrain(t, BRIGHT_MIN, 255);
}

static void lightBegin() {
  pinMode(PIN_LDR, INPUT);
  analogSetPinAttenuation(PIN_LDR, ADC_11db); // zakres ok. 0-3,1 V
  lightMv = analogReadMilliVolts(PIN_LDR);
  lightUsedMv = (int)lightMv;
  updateTarget();
  brightness = autoTarget; // od razu właściwa jasność, bez przejścia
}

// Wywoływane co obieg loop(). Zwraca true, gdy jasność się zmieniła.
static bool lightLoop() {
  static uint32_t lastRead = 0, lastStep = 0;
  uint32_t now = millis();
  if (now - lastRead >= LIGHT_READ_MS) {
    lastRead = now;
    // Filtr dolnoprzepustowy (stała czasowa ok. 1 s) - pomija np. cień ręki
    lightMv += (analogReadMilliVolts(PIN_LDR) - lightMv) * 0.1f;
    if (abs((int)lightMv - lightUsedMv) > LIGHT_HYST_MV) {
      lightUsedMv = (int)lightMv;
      updateTarget();
    }
  }
  if (brightness == autoTarget || now - lastStep < BRIGHT_STEP_MS)
    return false;
  lastStep = now;
  int diff = autoTarget - brightness;
  int step = diff / 8; // szybciej przy dużej różnicy, na końcu po 1
  if (step == 0)
    step = diff > 0 ? 1 : -1;
  brightness += step;
  lcd.setBrightness(brightness);
  return true;
}

// Zapis poziomu bazowego (po puszczeniu suwaka)
static void saveBase() {
  if (baseLevel == savedBase)
    return; // oszczędzamy flash - zapis tylko po zmianie
  Preferences prefs;
  prefs.begin("display", false);
  prefs.putUChar("bright", baseLevel);
  prefs.end();
  savedBase = baseLevel;
}

// ---------------------------------------------------------------------------
// Elementy ekranu

static const char *formatAge(char *buf, size_t n, const SensorData &d) {
  if (!d.valid) {
    snprintf(buf, n, "brak");
  } else if (d.ageUnknown) {
    snprintf(buf, n, "przed restartem");
  } else {
    uint32_t s = (millis() - d.rxMillis) / 1000;
    if (s < 60)
      snprintf(buf, n, "%lu s temu", (unsigned long)s);
    else if (s < 3600)
      snprintf(buf, n, "%lu min temu", (unsigned long)(s / 60));
    else
      snprintf(buf, n, "%lu h temu", (unsigned long)(s / 3600));
  }
  return buf;
}

static bool isStale(const SensorData &d) {
  return !d.valid || d.ageUnknown || millis() - d.rxMillis > DATA_TIMEOUT_MS;
}

static void drawHeader(const char *title, const SensorData &d) {
  auto &s = headerSpr;
  s.fillScreen(C_HEADER);
  s.setFont(&fonts::FreeSansBold12pt7b);
  s.setTextColor(C_TEXT);
  s.setTextDatum(textdatum_t::middle_left);
  s.drawString(title, 12, HEADER_H / 2);

  const char *status;
  uint16_t color;
  if (!d.valid) {
    status = "Czekam na dane";
    color = C_YELLOW;
  } else if (isStale(d)) {
    status = "Brak danych";
    color = C_RED;
  } else {
    status = "Online";
    color = C_GREEN;
  }
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(color);
  s.setTextDatum(textdatum_t::middle_right);
  s.drawString(status, SCR_W - 12, HEADER_H / 2);
  int tw = s.textWidth(status);
  s.fillCircle(SCR_W - 12 - tw - 12, HEADER_H / 2, 5, color);
  s.pushSprite(0, 0);
}

static void drawFooter(const SensorData &d) {
  auto &s = footerSpr;
  char buf[64], age[24];
  s.fillScreen(C_BG);
  s.setFont(&fonts::Font2);
  s.setTextColor(C_LABEL);
  s.setTextDatum(textdatum_t::middle_left);
  snprintf(buf, sizeof(buf), "Ostatni odczyt: %s",
           formatAge(age, sizeof(age), d));
  s.drawString(buf, 8, FOOTER_H / 2);
  if (d.hasRssi)
    snprintf(buf, sizeof(buf), "Pakiety: %lu   RSSI: %d dBm",
             (unsigned long)d.packets, d.rssi);
  else
    snprintf(buf, sizeof(buf), "Pakiety: %lu", (unsigned long)d.packets);
  s.setTextDatum(textdatum_t::middle_right);
  s.drawString(buf, SCR_W - 8, FOOTER_H / 2);
  s.pushSprite(0, FOOTER_Y);
}

// Kafelek z pojedynczym pomiarem. barPct < 0 - bez paska.
static void drawTile(int col, int row, const char *label, uint16_t accent,
                     const char *value, const char *unit, bool degree,
                     const char *sub, int barPct, bool stale) {
  auto &s = tileSpr;
  s.fillScreen(C_BG);
  s.fillRoundRect(0, 0, TILE_W, TILE_H, 10, C_TILE);
  s.fillRoundRect(14, 14, 4, 18, 2, accent);

  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(C_LABEL);
  s.setTextDatum(textdatum_t::middle_left);
  s.drawString(label, 26, 23);

  const int baseY = 76;
  s.setFont(&fonts::FreeSansBold24pt7b);
  s.setTextColor(stale ? C_LABEL : C_TEXT);
  s.setTextDatum(textdatum_t::baseline_left);
  s.drawString(value, 14, baseY);
  int x = 14 + s.textWidth(value) + 6;

  s.setFont(&fonts::FreeSans12pt7b);
  s.setTextColor(C_LABEL);
  if (degree) { // fonty GFX nie mają znaku stopnia - rysujemy kółko
    s.drawCircle(x + 4, baseY - 30, 4, C_LABEL);
    s.drawCircle(x + 4, baseY - 30, 3, C_LABEL);
    x += 12;
  }
  s.drawString(unit, x, baseY);

  if (sub) {
    s.setFont(&fonts::FreeSans9pt7b);
    s.setTextDatum(textdatum_t::baseline_left);
    s.drawString(sub, 14, TILE_H - 10);
  }

  if (barPct >= 0) {
    if (barPct > 100)
      barPct = 100;
    const int bw = 100, bh = 14, bx = TILE_W - bw - 14,
              by = TILE_H - 10 - bh + 2;
    uint16_t c = barPct > 50 ? C_GREEN : (barPct > 20 ? C_YELLOW : C_RED);
    if (stale)
      c = C_LABEL;
    s.drawRoundRect(bx, by, bw, bh, 4, C_LABEL);
    s.fillRect(bx + bw, by + 4, 3, bh - 8, C_LABEL); // "biegun" baterii
    int fw = (bw - 4) * barPct / 100;
    if (fw > 0)
      s.fillRoundRect(bx + 2, by + 2, fw, bh - 4, 2, c);
  }

  s.pushSprite(TILE_X[col], TILE_Y[row]);
}

static void drawMainTiles(const SensorData &d) {
  char v[16], sub[24];
  bool stale = isStale(d);

  if (d.hasT)
    snprintf(v, sizeof(v), "%.1f", d.t);
  else
    strcpy(v, "---");
  drawTile(0, 0, "Temperatura", C_TEMP, v, "C", true, nullptr, -1, stale);

  if (d.hasH)
    snprintf(v, sizeof(v), "%d", d.h);
  else
    strcpy(v, "---");
  drawTile(1, 0, "Wilgotnosc", C_HUM, v, "%", false, nullptr, -1, stale);

  if (d.hasP)
    snprintf(v, sizeof(v), "%d", d.p);
  else
    strcpy(v, "---");
  drawTile(0, 1, "Cisnienie", C_PRESS, v, "hPa", false, nullptr, -1, stale);

  if (d.hasBat) {
    snprintf(v, sizeof(v), "%d", d.bat);
    snprintf(sub, sizeof(sub), "%.2f V", d.batV);
  } else {
    strcpy(v, "---");
    sub[0] = '\0';
  }
  drawTile(1, 1, "Bateria nadajnika", C_GREEN, v, "%", false, sub,
           d.hasBat ? d.bat : -1, stale);
}

// Zmiana napięcia baterii w ciągu ostatnich 24 h (z historii). Liczona
// tylko przy wejściu na ekran i po nowym pakiecie - nie co sekundę.
static bool batTrendValid = false;
static int batTrendMv = 0;
static uint32_t batTrendSpan = 0; // s między skrajnymi odczytami

static void updateBatteryTrend() {
  batTrendValid = false;
  time_t now = timeNow();
  uint16_t n = historyCount();
  if (!now || n == 0)
    return;
  HistoryEntry e, newest, oldest;
  bool haveNewest = false, haveOldest = false;
  for (int i = n - 1, scanned = 0; i >= 0 && scanned < 1000; i--, scanned++) {
    if (!historyGet(i, e) || (e.flags & HF_UPTIME) || !(e.flags & HF_BAT))
      continue;
    if ((uint32_t)now - e.time > CHART_SPAN_S)
      break;
    if (!haveNewest)
      newest = e, haveNewest = true;
    oldest = e, haveOldest = true;
  }
  if (!haveNewest || !haveOldest || newest.time - oldest.time < 3600)
    return; // za krótki okres, żeby mówić o trendzie
  batTrendMv = (int)newest.batMv - (int)oldest.batMv;
  batTrendSpan = newest.time - oldest.time;
  batTrendValid = true;
}

static void drawInfoRow(int row, const char *label, const char *value,
                        uint16_t valueColor = C_TEXT) {
  auto &s = infoSpr;
  const int y = INFO_ROW0 + row * INFO_ROW_H;
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextColor(C_LABEL);
  s.drawString(label, 20, y);
  s.setTextColor(valueColor);
  s.drawString(value, 220, y);
}

static void drawInfo(const SensorData &d) {
  auto &s = infoSpr;
  char buf[48];
  s.fillScreen(C_BG);
  bool stale = isStale(d);

  // Stan baterii nadajnika
  if (d.hasBat) {
    int pct = constrain(d.bat, 0, 100);
    uint16_t c = stale ? C_LABEL
                       : (pct > 50 ? C_GREEN : (pct > 20 ? C_YELLOW : C_RED));
    snprintf(buf, sizeof(buf), "%d %%", d.bat);
    drawInfoRow(0, "Bateria nadajnika", buf, c);
    const int bw = 90, bh = 14, bx = 330, by = INFO_ROW0 - bh / 2;
    s.drawRoundRect(bx, by, bw, bh, 4, C_LABEL);
    s.fillRect(bx + bw, by + 4, 3, bh - 8, C_LABEL);
    int fw = (bw - 4) * pct / 100;
    if (fw > 0)
      s.fillRoundRect(bx + 2, by + 2, fw, bh - 4, 2, c);

    int len = snprintf(buf, sizeof(buf), "%.2f V", d.batV);
    if (batTrendValid)
      snprintf(buf + len, sizeof(buf) - len, "   (%+.2f V / %lu h)",
               batTrendMv / 1000.0f, (unsigned long)((batTrendSpan + 1800) / 3600));
    drawInfoRow(1, "Napiecie baterii", buf);
  } else {
    drawInfoRow(0, "Bateria nadajnika", "---");
    drawInfoRow(1, "Napiecie baterii", "---");
  }
  char age[24];
  drawInfoRow(2, "Ostatni odczyt", formatAge(age, sizeof(age), d),
              stale ? C_RED : C_TEXT);

  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", ownMac[0],
           ownMac[1], ownMac[2], ownMac[3], ownMac[4], ownMac[5]);
  drawInfoRow(3, "MAC odbiornika", buf);
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X  (kanal %d)",
           SENDER_MAC[0], SENDER_MAC[1], SENDER_MAC[2], SENDER_MAC[3],
           SENDER_MAC[4], SENDER_MAC[5], ESPNOW_CHANNEL);
  drawInfoRow(4, "MAC nadajnika", buf);
  snprintf(buf, sizeof(buf), "%d mV  (korekta %+d%%)", (int)lightMv,
           lightCorrection((int)lightMv) * 100 / 255);
  drawInfoRow(5, "Czujnik swiatla", buf);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)d.packets);
  drawInfoRow(6, "Odebrane pakiety", buf);
  if (d.hasRssi)
    snprintf(buf, sizeof(buf), "%d dBm", d.rssi);
  else
    strcpy(buf, "---");
  drawInfoRow(7, "Ostatni RSSI", buf);
  uint32_t up = millis() / 1000;
  snprintf(buf, sizeof(buf), "%lu d %02lu:%02lu:%02lu",
           (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
           (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
  drawInfoRow(8, "Czas pracy", buf);
  snprintf(buf, sizeof(buf), "%lu kB",
           (unsigned long)(ESP.getFreeHeap() / 1024));
  drawInfoRow(9, "Wolna pamiec", buf);

  s.pushSprite(0, HEADER_H);
}

// ---------------------------------------------------------------------------
// Wykresy dobowe (ostatnie 24 h z historii)

struct MetricInfo {
  const char *title;
  const char *unit;
  bool degree;   // rysować znak stopnia przed jednostką
  int decimals;  // miejsca po przecinku w podsumowaniu
  uint16_t color;
  uint8_t flag;  // HF_*
  // Szerokość wygładzania [s] (sigma jądra Gaussa), 0 = bez wygładzania.
  // Wilgotność i ciśnienie są zapisywane w pełnych jednostkach - bez
  // wygładzania powolna zmiana wygląda jak schodki co 1 %/1 hPa.
  uint16_t smoothS;
  // Kolor krzywej dla wartości ujemnych, 0 = jak dla dodatnich (m.color)
  uint16_t negColor;
};

static const MetricInfo METRICS[3] = {
    {"Temperatura - 24 h", "C", true, 1, C_TEMP, HF_T, 0, C_RED},
    {"Wilgotnosc - 24 h", "%", false, 0, C_HUM, HF_H, 30 * 60, 0},
    {"Cisnienie - 24 h", "hPa", false, 0, C_PRESS, HF_P, 60 * 60, 0},
};

struct ChartPoint {
  uint32_t t;
  float v;
};
// Nadajnik wysyła co 15 min (96 odczytów na dobę) - zapas na częstsze pakiety
static constexpr int CHART_MAX_POINTS = 400;
static ChartPoint chartPts[CHART_MAX_POINTS];
static float chartFit[CHART_MAX_POINTS]; // wartości po wygładzeniu
static uint32_t chartEnd = 0; // prawa krawędź narysowanego wykresu (0 = brak)

// Koniec osi czasu: najbliższa pełna godzina po "now" (oś od pełnej godziny)
static uint32_t chartEndFor(time_t now) {
  return now ? ((uint32_t)now / 3600 + 1) * 3600 : 0;
}

// Wczytuje odczyty z przedziału [from, to] - od najstarszego.
static int loadChartPoints(const MetricInfo &m, uint32_t from, uint32_t to) {
  int n = 0;
  HistoryEntry e;
  for (int i = historyCount() - 1, scanned = 0;
       i >= 0 && n < CHART_MAX_POINTS && scanned < 1000; i--, scanned++) {
    if (!historyGet(i, e) || (e.flags & HF_UPTIME)) // bez znacznika czasu
      continue;
    if (e.time < from)
      break;
    if (e.time > to || !(e.flags & m.flag))
      continue;
    float v = m.flag == HF_T ? e.t10 / 10.0f : (m.flag == HF_H ? e.h : e.p);
    chartPts[n++] = {e.time, v};
  }
  std::reverse(chartPts, chartPts + n);
  return n;
}

// Przerwa w danych: między odczytami brakuje co najmniej jednego pakietu
static constexpr uint32_t CHART_GAP_S = DATA_TIMEOUT_MS / 1000;

static bool linked(int i) { // czy odczyt i łączy się z i+1
  return chartPts[i + 1].t - chartPts[i].t <= CHART_GAP_S;
}

// Wygładzanie lokalną regresją liniową z wagami Gaussa (w obrębie ciągłego
// fragmentu danych). W odróżnieniu od średniej kroczącej nie opóźnia
// krzywej na końcach - ostatni punkt nadąża za bieżącym trendem.
static void smoothChart(int n, uint32_t sigmaS) {
  if (!sigmaS) {
    for (int i = 0; i < n; i++)
      chartFit[i] = chartPts[i].v;
    return;
  }
  const float sigmaH = sigmaS / 3600.0f;
  for (int seg0 = 0; seg0 < n;) {
    int seg1 = seg0; // ostatni punkt fragmentu
    while (seg1 + 1 < n && linked(seg1))
      seg1++;
    for (int i = seg0; i <= seg1; i++) {
      float sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
      for (int j = seg0; j <= seg1; j++) {
        const float x = ((int32_t)(chartPts[j].t - chartPts[i].t)) / 3600.0f;
        const float z = x / sigmaH;
        if (z < -3 || z > 3)
          continue;
        const float w = expf(-0.5f * z * z);
        const float y = chartPts[j].v;
        sw += w, sx += w * x, sy += w * y, sxx += w * x * x, sxy += w * x * y;
      }
      const float det = sw * sxx - sx * sx;
      chartFit[i] = det > 1e-4f * sw * sw ? (sxx * sy - sx * sxy) / det : sy / sw;
    }
    seg0 = seg1 + 1;
  }
}

// Nachylenia dla monotonicznej interpolacji sześciennej (Fritsch-Carlson):
// krzywa przechodzi przez punkty i nie tworzy fałszywych ekstremów.
static void curveSlopes(const float *x, const float *y, int n, float *m) {
  if (n < 2) {
    m[0] = 0;
    return;
  }
  float dPrev = (y[1] - y[0]) / (x[1] - x[0]);
  m[0] = dPrev;
  for (int i = 1; i < n - 1; i++) {
    const float h0 = x[i] - x[i - 1], h1 = x[i + 1] - x[i];
    const float d = (y[i + 1] - y[i]) / h1;
    if (dPrev * d <= 0) {
      m[i] = 0; // lokalne ekstremum - płasko, bez przestrzeliwania
    } else { // ważona średnia harmoniczna nachyleń sąsiednich odcinków
      const float w1 = 2 * h1 + h0, w2 = h1 + 2 * h0;
      m[i] = (w1 + w2) / (w1 / dPrev + w2 / d);
    }
    dPrev = d;
  }
  m[n - 1] = dPrev;
}

// Napis wyśrodkowany w obszarze wykresu
static void chartMessage(const char *line1, const char *line2) {
  auto &s = chartSpr;
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextDatum(textdatum_t::middle_center);
  s.setTextColor(C_LABEL);
  s.drawString(line1, (PLOT_L + PLOT_R) / 2, (PLOT_T + PLOT_B) / 2 - 12);
  if (line2)
    s.drawString(line2, (PLOT_L + PLOT_R) / 2, (PLOT_T + PLOT_B) / 2 + 12);
}

static void drawChart(const MetricInfo &m) {
  auto &s = chartSpr;
  s.fillScreen(C_BG);
  const time_t now = timeNow();
  chartEnd = chartEndFor(now);
  if (!chartEnd) {
    chartMessage("Zegar niezsynchronizowany", "wykres dostepny po synchronizacji NTP");
    s.pushSprite(0, HEADER_H);
    return;
  }
  const uint32_t start = chartEnd - CHART_SPAN_S;
  const int n = loadChartPoints(m, start, chartEnd);

  // Jednostka nad osią Y
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(m.color);
  s.setTextDatum(textdatum_t::middle_left);
  int ux = 8;
  if (m.degree) {
    s.drawCircle(ux + 3, 8, 3, m.color);
    ux += 9;
  }
  s.drawString(m.unit, ux, 12);

  // Zakres osi Y: pełne jednostki obejmujące wszystkie odczyty, min. 2 jednostki
  int lo = 0, hi = 0;
  float vmin = 0, vmax = 0;
  if (n) {
    smoothChart(n, m.smoothS);
    vmin = vmax = chartPts[0].v;
    float fmin = chartFit[0], fmax = chartFit[0];
    for (int i = 1; i < n; i++) {
      vmin = std::min(vmin, chartPts[i].v);
      vmax = std::max(vmax, chartPts[i].v);
      fmin = std::min(fmin, chartFit[i]);
      fmax = std::max(fmax, chartFit[i]);
    }
    // Zakres z odczytów i krzywej (regresja może minimalnie wyjść poza odczyty)
    lo = (int)floorf(std::min(vmin, fmin));
    hi = (int)ceilf(std::max(vmax, fmax));
    while (hi - lo < 2) {
      hi++;
      if (hi - lo < 2)
        lo--;
    }

    char buf[64];
    const int dec = m.decimals;
    snprintf(buf, sizeof(buf), "min %.*f    max %.*f    teraz %.*f", dec, vmin,
             dec, vmax, dec, chartPts[n - 1].v);
    s.setTextColor(C_TEXT);
    s.setTextDatum(textdatum_t::middle_right);
    s.drawString(buf, PLOT_R, 12);
  }

  const float pxPerUnit = n ? (float)(PLOT_B - PLOT_T) / (hi - lo) : 0;
  const float pxPerSec = (float)(PLOT_R - PLOT_L) / CHART_SPAN_S;
  auto yOf = [&](float v) { return (int)lroundf(PLOT_B - (v - lo) * pxPerUnit); };
  auto xOf = [&](uint32_t t) {
    return (int)lroundf(PLOT_L + (int32_t)(t - start) * pxPerSec);
  };

  s.setFont(&fonts::Font2);
  s.setTextColor(C_LABEL);

  // Siatka pozioma co 1 jednostkę, opisy co 1/2/5/10... jednostek - tak,
  // żeby się nie nakładały
  if (n) {
    static const int STEPS[] = {1, 2, 5, 10, 20, 50, 100};
    int labelStep = STEPS[6];
    for (int st : STEPS)
      if (st * pxPerUnit >= 16) {
        labelStep = st;
        break;
      }
    const bool everyUnit = pxPerUnit >= 4;
    s.setTextDatum(textdatum_t::middle_right);
    for (int v = lo; v <= hi; v++) {
      const bool major = v % labelStep == 0;
      if (!major && !everyUnit)
        continue;
      const int y = yOf(v);
      s.drawFastHLine(PLOT_L, y, PLOT_R - PLOT_L + 1, major ? C_GRID_MAJOR : C_GRID);
      if (major) {
        char lb[8];
        snprintf(lb, sizeof(lb), "%d", v);
        s.drawString(lb, PLOT_L - 5, y);
      }
    }
  }

  // Siatka pionowa co 1 h, opis co 2 h; o północy data zamiast godziny
  s.setTextDatum(textdatum_t::top_center);
  for (int h = 0; h <= 24; h++) {
    const uint32_t t = start + h * 3600;
    const time_t tt = t;
    struct tm lt;
    localtime_r(&tt, &lt);
    const int x = xOf(t);
    const bool midnight = lt.tm_hour == 0;
    s.drawFastVLine(x, PLOT_T, PLOT_B - PLOT_T + 1, midnight ? C_LABEL : (lt.tm_hour % 2 ? C_GRID : C_GRID_MAJOR));
    s.drawFastVLine(x, PLOT_B, 4, C_LABEL);
    if (lt.tm_hour % 2 == 0) {
      char lb[8];
      if (midnight)
        snprintf(lb, sizeof(lb), "%d.%02d", lt.tm_mday, lt.tm_mon + 1);
      else
        snprintf(lb, sizeof(lb), "%d", lt.tm_hour);
      s.setTextColor(midnight ? C_TEXT : C_LABEL);
      s.drawString(lb, x, PLOT_B + 6);
    }
  }
  s.drawFastHLine(PLOT_L, PLOT_B, PLOT_R - PLOT_L + 1, C_LABEL);
  s.drawFastVLine(PLOT_L, PLOT_T, PLOT_B - PLOT_T + 1, C_LABEL);

  if (!n) {
    chartMessage("Brak odczytow z ostatnich 24 h", nullptr);
    s.pushSprite(0, HEADER_H);
    return;
  }

  // Płynna krzywa przez wygładzone punkty, osobno dla każdego ciągłego
  // fragmentu - przerwa, gdy między odczytami brakuje pakietów
  static float px[CHART_MAX_POINTS], py[CHART_MAX_POINTS], slope[CHART_MAX_POINTS];
  for (int i = 0; i < n; i++) {
    px[i] = PLOT_L + (int32_t)(chartPts[i].t - start) * pxPerSec;
    py[i] = PLOT_B - (chartFit[i] - lo) * pxPerUnit;
  }
  // Kolor zależny od znaku wartości: punkt poniżej linii zera (większe y)
  // oznacza wartość ujemną
  const float zeroY = PLOT_B + lo * pxPerUnit;
  auto colorAt = [&](float y) {
    return m.negColor && y > zeroY ? m.negColor : m.color;
  };
  for (int seg0 = 0; seg0 < n;) {
    int seg1 = seg0;
    while (seg1 + 1 < n && linked(seg1))
      seg1++;
    const int cnt = seg1 - seg0 + 1;
    if (cnt == 1) { // pojedynczy odczyt
      s.fillCircle(lroundf(px[seg0]), lroundf(py[seg0]), 2, colorAt(py[seg0]));
    } else {
      curveSlopes(px + seg0, py + seg0, cnt, slope + seg0);
      int x0 = lroundf(px[seg0]), y0 = lroundf(py[seg0]);
      float yPrev = py[seg0];
      for (int i = seg0; i < seg1; i++) {
        const float h = px[i + 1] - px[i];
        const int steps = std::max(1, (int)(h / 2)); // odcinki co ok. 2 px
        for (int k = 1; k <= steps; k++) {
          const float u = (float)k / steps, u2 = u * u, u3 = u2 * u;
          const float y = (2 * u3 - 3 * u2 + 1) * py[i] +
                          (u3 - 2 * u2 + u) * h * slope[i] +
                          (-2 * u3 + 3 * u2) * py[i + 1] +
                          (u3 - u2) * h * slope[i + 1];
          const int x1 = lroundf(px[i] + u * h), y1 = lroundf(y);
          // Kolor wg środka odcinka (odcinki mają ok. 2 px, więc zmiana
          // koloru wypada praktycznie dokładnie na linii zera)
          s.drawWideLine(x0, y0, x1, y1, 1.0f, colorAt((yPrev + y) / 2));
          x0 = x1, y0 = y1, yPrev = y;
        }
      }
    }
    seg0 = seg1 + 1;
  }
  const int lx = lroundf(px[n - 1]), ly = lroundf(py[n - 1]);
  s.fillCircle(lx, ly, 4, colorAt(py[n - 1]));
  s.drawCircle(lx, ly, 4, C_TEXT);

  s.pushSprite(0, HEADER_H);
}

// ---------------------------------------------------------------------------
// Dolny pasek przycisków

static void drawNav() {
  auto &s = navSpr;
  const int bw = SCR_W / SCREEN_COUNT;
  s.fillScreen(C_BG);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextDatum(textdatum_t::middle_center);
  for (int i = 0; i < SCREEN_COUNT; i++) {
    const bool active = i == (int)screen;
    const int x = i * bw + 2, w = bw - 4;
    s.fillRoundRect(x, 2, w, NAV_H - 4, 6, active ? C_TILE : C_NAV);
    s.fillRoundRect(x + 10, 2, w - 20, 3, 1,
                    active ? NAV_COLORS[i] : C_TRACK);
    s.setTextColor(active ? C_TEXT : C_LABEL);
    s.drawString(NAV_LABELS[i], x + w / 2, NAV_H / 2 + 2);
  }
  s.pushSprite(0, NAV_Y);
}

// Przycisk paska pod punktem dotyku lub -1. Pasek łapie też kilka pikseli
// nad sobą - kalibracja przy krawędzi bywa niedokładna.
static int navButtonAt(int32_t x, int32_t y) {
  if (y < NAV_Y - 8)
    return -1;
  return constrain((int)(x * SCREEN_COUNT / SCR_W), 0, SCREEN_COUNT - 1);
}

// Pas suwaka z zapasem w górę (kalibracja bywa niedokładna); poniżej
// zaczyna się pasek przycisków.
static bool inSlider(int32_t y) {
  return y >= SLIDER_Y - 16 && y < NAV_Y - 8;
}

static bool inAutoButton(int32_t x) { return x >= AUTO_X - 10; }

static uint8_t sliderXToLevel(int32_t x) {
  if (x <= TRACK_X0)
    return BRIGHT_MIN;
  if (x >= TRACK_X1)
    return 255;
  return BRIGHT_MIN + (x - TRACK_X0) * (255 - BRIGHT_MIN) / (TRACK_X1 - TRACK_X0);
}

static void drawSlider() {
  auto &s = sliderSpr;
  const int cy = SLIDER_H / 2;
  s.fillScreen(C_BG);

  s.setFont(&fonts::Font4);
  s.setTextColor(C_LABEL);
  s.setTextDatum(textdatum_t::middle_left);
  s.drawString("Jasnosc", 20, cy);

  // Gałka = poziom bazowy, wypełnienie = aktualna jasność (w trybie auto
  // różni się od bazy o korektę z fotorezystora), napis = aktualna jasność
  auto levelX = [](int level) {
    return TRACK_X0 + (level - BRIGHT_MIN) * (TRACK_X1 - TRACK_X0) /
                          (255 - BRIGHT_MIN);
  };
  const int fx = levelX(brightness), kx = levelX(baseLevel);
  s.fillRoundRect(TRACK_X0, cy - 4, TRACK_X1 - TRACK_X0, 8, 4, C_TRACK);
  if (fx > TRACK_X0)
    s.fillRoundRect(TRACK_X0, cy - 4, fx - TRACK_X0, 8, 4, C_YELLOW);
  s.fillCircle(kx, cy, KNOB_R, C_TEXT);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", (brightness * 100 + 127) / 255);
  s.setTextColor(C_TEXT);
  s.setTextDatum(textdatum_t::middle_right);
  s.drawString(buf, AUTO_X - 10, cy);

  const int by = cy - AUTO_H / 2;
  if (autoBright)
    s.fillRoundRect(AUTO_X, by, AUTO_W, AUTO_H, 6, C_YELLOW);
  else
    s.drawRoundRect(AUTO_X, by, AUTO_W, AUTO_H, 6, C_LABEL);
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextColor(autoBright ? C_BG : C_LABEL);
  s.setTextDatum(textdatum_t::middle_center);
  s.drawString("Auto", AUTO_X + AUTO_W / 2, cy + 1);

  s.pushSprite(0, SLIDER_Y);
}

// ---------------------------------------------------------------------------
// Ekran startowy z listą kroków

static constexpr int BOOT_LIST_Y = 128; // środek pierwszego wiersza
static constexpr int BOOT_LINE_H = 30;
static constexpr int BOOT_MAX_LINES = 6;
static int bootLines = 0;

static int bootLineY(int i) { return BOOT_LIST_Y + i * BOOT_LINE_H; }

static void drawBootScreen() {
  lcd.fillScreen(C_BG);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextColor(C_TEXT, C_BG);
  lcd.drawString("Stacja pogodowa", SCR_W / 2, 44);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(C_YELLOW, C_BG);
  lcd.drawString("Uruchamianie - dotyk jeszcze nieaktywny", SCR_W / 2, 86);
  bootLines = 0;
}

void displayBootStep(const char *text) {
  if (bootLines >= BOOT_MAX_LINES) { // brak miejsca - przewiń listę od nowa
    lcd.fillRect(0, bootLineY(0) - BOOT_LINE_H / 2, SCR_W,
                 BOOT_MAX_LINES * BOOT_LINE_H, C_BG);
    bootLines = 0;
  }
  int y = bootLineY(bootLines++);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setTextColor(C_TEXT, C_BG);
  lcd.drawString(text, 30, y);
  displayBootResult("...", BootState::Pending);
}

void displayBootResult(const char *result, BootState state) {
  if (bootLines == 0)
    return;
  int y = bootLineY(bootLines - 1);
  uint16_t c = state == BootState::Ok      ? C_GREEN
               : state == BootState::Error ? C_RED
                                           : C_LABEL;
  lcd.fillRect(300, y - BOOT_LINE_H / 2 + 2, SCR_W - 300, BOOT_LINE_H - 4, C_BG);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextDatum(textdatum_t::middle_right);
  lcd.setTextColor(c, C_BG);
  lcd.drawString(result, SCR_W - 30, y);
}

// ---------------------------------------------------------------------------

void displayBegin(const uint8_t mac[6]) {
  memcpy(ownMac, mac, 6);
  lcd.init();
  // T_IRQ (PENIRQ) w XPT2046 to wyjście open-drain, a moduł nie ma podciągnięcia.
  // LovyanGFX ustawia pin jako zwykłe wejście - bez pull-up pin "pływa"
  // i sterownik losowo pomija odczyty dotyku.
  if (PIN_TOUCH_IRQ >= 0) // -1 = T_IRQ niepodłączony
    pinMode(PIN_TOUCH_IRQ, INPUT_PULLUP);
  lcd.setRotation(1); // poziomo 480x320; obrócony obraz -> 3
  loadBrightness();
  lightBegin();
  lcd.setBrightness(brightness);

  tileSpr.setColorDepth(16);
  tileSpr.createSprite(TILE_W, TILE_H);
  headerSpr.setColorDepth(16);
  headerSpr.createSprite(SCR_W, HEADER_H);
  footerSpr.setColorDepth(16);
  footerSpr.createSprite(SCR_W, FOOTER_H);
  sliderSpr.setColorDepth(16);
  sliderSpr.createSprite(SCR_W, SLIDER_H);
  navSpr.setColorDepth(16);
  navSpr.createSprite(SCR_W, NAV_H);
  // Duże sprite'y (ok. 200 i 230 KB) w PSRAM
  infoSpr.setColorDepth(16);
  infoSpr.setPsram(true);
  chartSpr.setColorDepth(16);
  chartSpr.setPsram(true);
  if (!infoSpr.createSprite(SCR_W, INFO_H) ||
      !chartSpr.createSprite(SCR_W, CHART_H))
    Serial.println("[EKRAN] Brak pamieci na sprite'y ekranu info/wykresu");

  initTouch();
  drawBootScreen();
  fullRedraw = true;
}

void displayUpdate(const SensorData &d, bool dataChanged) {
  // Dotyk: przyciski na dolnym pasku przełączają ekrany, a na ekranie
  // informacji przeciąganie po suwaku zmienia jasność.
  // Krótkie przerwy w odczycie (XPT2046 gubi pojedyncze próbki przy
  // przesuwaniu palca) nie są traktowane jako puszczenie ekranu.
  static constexpr uint32_t RELEASE_MS = 80;
  static bool wasTouched = false;
  static bool dragging = false;
  static uint32_t lastTap = 0;
  static uint32_t lastSeen = 0;
  int32_t tx = 0, ty = 0;
  bool touched = lcd.getTouch(&tx, &ty);
  if (touched)
    lastSeen = millis();
  else if (wasTouched && millis() - lastSeen < RELEASE_MS)
    touched = wasTouched = true, tx = -1; // nadal "wciśnięty", bez nowej pozycji
  if (touched && !wasTouched) {
    Serial.printf("[DOTYK] x=%ld y=%ld\n", (long)tx, (long)ty);
    int btn = navButtonAt(tx, ty);
    if (btn >= 0) {
      if (btn != (int)screen && millis() - lastTap > 250) {
        lastTap = millis();
        screen = (Screen)btn;
        fullRedraw = true;
      }
    } else if (screen == Screen::Info && inSlider(ty)) {
      if (inAutoButton(tx)) {
        // Jasność płynnie przechodzi do bazy lub bazy z korektą
        autoBright = !autoBright;
        saveAutoBright();
        updateTarget();
        drawSlider();
      } else {
        dragging = true; // zmiana poziomu bazowego - tryb auto zostaje
      }
    }
  }
  // Jasność z fotorezystora; suwak odświeżany najwyżej 5 razy na sekundę
  static uint32_t lastSliderDraw = 0;
  static bool sliderDirty = false;
  if (lightLoop())
    sliderDirty = true;
  if (sliderDirty && screen == Screen::Info && !fullRedraw &&
      millis() - lastSliderDraw >= 200) {
    sliderDirty = false;
    lastSliderDraw = millis();
    drawSlider();
  }

  if (touched && dragging && tx >= 0) {
    uint8_t level = sliderXToLevel(tx);
    if (level != baseLevel) {
      baseLevel = level;
      updateTarget();
      brightness = autoTarget; // od razu, bez płynnego przejścia
      lcd.setBrightness(brightness);
      drawSlider();
    }
  }
  if (!touched && dragging) {
    dragging = false;
    saveBase();
  }
  wasTouched = touched;

  static uint32_t lastTick = 0;
  static bool lastStale = true;
  bool tick = millis() - lastTick >= 1000;
  bool stale = isStale(d);
  if (!fullRedraw && !dataChanged && !tick && stale == lastStale)
    return;
  lastTick = millis();

  if (fullRedraw) {
    lcd.fillScreen(C_BG);
    drawNav();
  }

  switch (screen) {
  case Screen::Main:
    drawHeader("Stacja pogodowa", d);
    if (fullRedraw || dataChanged || stale != lastStale)
      drawMainTiles(d);
    drawFooter(d);
    break;
  case Screen::Temp:
  case Screen::Hum:
  case Screen::Press: {
    const MetricInfo &m = METRICS[(int)screen - (int)Screen::Temp];
    drawHeader(m.title, d);
    // Nowy odczyt lub przesunięcie osi czasu o pełną godzinę
    if (fullRedraw || dataChanged || chartEndFor(timeNow()) != chartEnd)
      drawChart(m);
    break;
  }
  default:
    drawHeader("Informacje", d);
    if (fullRedraw || dataChanged)
      updateBatteryTrend();
    drawInfo(d);
    if (fullRedraw)
      drawSlider();
    break;
  }
  lastStale = stale;
  fullRedraw = false;
}
