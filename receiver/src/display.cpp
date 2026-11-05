#include "display.h"
#include "lgfx_config.h"
#include "packet.h"
#include <Arduino.h>
#include <Preferences.h>

// Po tylu ms bez pakietu dane uznajemy za nieaktualne. Nadajnik wysyła co
// 15 min - limit 35 min toleruje zgubienie jednego pakietu.
static constexpr uint32_t DATA_TIMEOUT_MS = 35UL * 60000;

// Kolory RGB565 (typ uint16_t, żeby LovyanGFX nie potraktował ich jako RGB888)
static constexpr uint16_t C_BG = 0x0000;
static constexpr uint16_t C_HEADER = 0x10A3;
static constexpr uint16_t C_TILE = 0x1905;
static constexpr uint16_t C_LABEL = 0x94D4;
static constexpr uint16_t C_TEXT = 0xFFFF;
static constexpr uint16_t C_TEMP = 0xFC65;
static constexpr uint16_t C_HUM = 0x555F;
static constexpr uint16_t C_PRESS = 0xB47F;
static constexpr uint16_t C_GREEN = 0x56EC;
static constexpr uint16_t C_YELLOW = 0xFE46;
static constexpr uint16_t C_RED = 0xF227;
static constexpr uint16_t C_TRACK = 0x39E7;

// Układ ekranu w poziomie 480x320
static constexpr int SCR_W = 480;
static constexpr int HEADER_H = 36;
static constexpr int FOOTER_H = 20;
static constexpr int TILE_W = 234;
static constexpr int TILE_H = 124;
static constexpr int TILE_X[2] = {4, 242};
static constexpr int TILE_Y[2] = {42, 172};
static constexpr int FOOTER_Y = 320 - FOOTER_H;

// Suwak jasności na ekranie informacji
static constexpr int SLIDER_Y = 252; // górna krawędź paska suwaka
static constexpr int SLIDER_H = 44;
static constexpr int TRACK_X0 = 160;
static constexpr int TRACK_X1 = 385;
static constexpr int KNOB_R = 12;

// Jasność podświetlenia (PWM 0..255), zapisywana w NVS
static constexpr uint8_t BRIGHT_MIN = 10; // niżej ekran jest praktycznie czarny
static constexpr uint8_t BRIGHT_DEFAULT = 200;

static LGFX lcd;
static LGFX_Sprite tileSpr(&lcd);
static LGFX_Sprite headerSpr(&lcd);
static LGFX_Sprite footerSpr(&lcd);
static LGFX_Sprite sliderSpr(&lcd);

enum class Screen { Main, Info };
static Screen screen = Screen::Main;
static bool fullRedraw = true;
static uint8_t ownMac[6];
static uint8_t brightness = BRIGHT_DEFAULT;
static uint8_t savedBrightness = BRIGHT_DEFAULT;

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
  uint8_t b = prefs.getUChar("bright", BRIGHT_DEFAULT);
  prefs.end();
  brightness = savedBrightness = b < BRIGHT_MIN ? BRIGHT_MIN : b;
}

static void saveBrightness() {
  if (brightness == savedBrightness)
    return; // oszczędzamy flash - zapis tylko po zmianie
  Preferences prefs;
  prefs.begin("display", false);
  prefs.putUChar("bright", brightness);
  prefs.end();
  savedBrightness = brightness;
}

// ---------------------------------------------------------------------------
// Elementy ekranu

static const char *formatAge(char *buf, size_t n, const SensorData &d) {
  if (!d.valid) {
    snprintf(buf, n, "brak");
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
  return !d.valid || millis() - d.rxMillis > DATA_TIMEOUT_MS;
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

  const int baseY = 88;
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
    s.drawString(sub, 14, TILE_H - 12);
  }

  if (barPct >= 0) {
    if (barPct > 100)
      barPct = 100;
    const int bw = 100, bh = 14, bx = TILE_W - bw - 14,
              by = TILE_H - 12 - bh + 2;
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

static void drawInfoRow(int row, const char *label, const char *value) {
  const int y = HEADER_H + 20 + row * 30;
  lcd.setFont(&fonts::Font4);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setTextColor(C_LABEL, C_BG);
  lcd.setTextPadding(0);
  lcd.drawString(label, 20, y);
  lcd.setTextColor(C_TEXT, C_BG);
  lcd.setTextPadding(SCR_W - 240 - 10); // czyści stary, dłuższy tekst
  lcd.drawString(value, 240, y);
  lcd.setTextPadding(0);
}

static void drawInfo(const SensorData &d) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", ownMac[0],
           ownMac[1], ownMac[2], ownMac[3], ownMac[4], ownMac[5]);
  drawInfoRow(0, "MAC odbiornika", buf);
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", SENDER_MAC[0],
           SENDER_MAC[1], SENDER_MAC[2], SENDER_MAC[3], SENDER_MAC[4],
           SENDER_MAC[5]);
  drawInfoRow(1, "MAC nadajnika", buf);
  snprintf(buf, sizeof(buf), "%d", ESPNOW_CHANNEL);
  drawInfoRow(2, "Kanal ESP-NOW", buf);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)d.packets);
  drawInfoRow(3, "Odebrane pakiety", buf);
  if (d.hasRssi)
    snprintf(buf, sizeof(buf), "%d dBm", d.rssi);
  else
    strcpy(buf, "---");
  drawInfoRow(4, "Ostatni RSSI", buf);
  uint32_t up = millis() / 1000;
  snprintf(buf, sizeof(buf), "%lu d %02lu:%02lu:%02lu",
           (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
           (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
  drawInfoRow(5, "Czas pracy", buf);
  snprintf(buf, sizeof(buf), "%lu kB",
           (unsigned long)(ESP.getFreeHeap() / 1024));
  drawInfoRow(6, "Wolna pamiec", buf);

  lcd.setFont(&fonts::Font2);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(C_LABEL, C_BG);
  lcd.drawString("Dotknij powyzej suwaka, aby wrocic", SCR_W / 2,
                 FOOTER_Y + FOOTER_H / 2);
}

// Cały dół ekranu (od ostatniego wiersza w dół) należy do suwaka -
// kalibracja przy krawędzi bywa niedokładna o kilkadziesiąt pikseli.
static bool inSlider(int32_t y) { return y >= SLIDER_Y - 16; }

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

  int kx = TRACK_X0 + (brightness - BRIGHT_MIN) * (TRACK_X1 - TRACK_X0) /
                          (255 - BRIGHT_MIN);
  s.fillRoundRect(TRACK_X0, cy - 4, TRACK_X1 - TRACK_X0, 8, 4, C_TRACK);
  if (kx > TRACK_X0)
    s.fillRoundRect(TRACK_X0, cy - 4, kx - TRACK_X0, 8, 4, C_YELLOW);
  s.fillCircle(kx, cy, KNOB_R, C_TEXT);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", (brightness * 100 + 127) / 255);
  s.setTextColor(C_TEXT);
  s.setTextDatum(textdatum_t::middle_right);
  s.drawString(buf, SCR_W - 20, cy);

  s.pushSprite(0, SLIDER_Y);
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
  lcd.setBrightness(brightness);

  tileSpr.setColorDepth(16);
  tileSpr.createSprite(TILE_W, TILE_H);
  headerSpr.setColorDepth(16);
  headerSpr.createSprite(SCR_W, HEADER_H);
  footerSpr.setColorDepth(16);
  footerSpr.createSprite(SCR_W, FOOTER_H);
  sliderSpr.setColorDepth(16);
  sliderSpr.createSprite(SCR_W, SLIDER_H);

  initTouch();
  fullRedraw = true;
}

void displayUpdate(const SensorData &d, bool dataChanged) {
  // Dotyk: nowe dotknięcie przełącza ekran główny / informacje,
  // a na ekranie informacji przeciąganie po suwaku zmienia jasność.
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
    if (screen == Screen::Info && inSlider(ty)) {
      dragging = true;
    } else if (millis() - lastTap > 250) {
      lastTap = millis();
      screen = (screen == Screen::Main) ? Screen::Info : Screen::Main;
      fullRedraw = true;
    }
  }
  if (touched && dragging && tx >= 0) {
    uint8_t level = sliderXToLevel(tx);
    if (level != brightness) {
      brightness = level;
      lcd.setBrightness(brightness);
      drawSlider();
    }
  }
  if (!touched && dragging) {
    dragging = false;
    saveBrightness();
  }
  wasTouched = touched;

  static uint32_t lastTick = 0;
  static bool lastStale = true;
  bool tick = millis() - lastTick >= 1000;
  bool stale = isStale(d);
  if (!fullRedraw && !dataChanged && !tick && stale == lastStale)
    return;
  lastTick = millis();

  if (fullRedraw)
    lcd.fillScreen(C_BG);

  if (screen == Screen::Main) {
    drawHeader("Stacja pogodowa", d);
    if (fullRedraw || dataChanged || stale != lastStale)
      drawMainTiles(d);
    drawFooter(d);
  } else {
    drawHeader("Informacje", d);
    drawInfo(d);
    if (fullRedraw)
      drawSlider();
  }
  lastStale = stale;
  fullRedraw = false;
}
