#include "packet.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <sys/time.h>

// MAC odbiornika (płytka CYD 4.0" ESP32-3248S040)
static const uint8_t RECEIVER_MAC[6] = {0x4C, 0xC3, 0x82, 0xC3, 0xAE, 0xFC};

// Okres wysyłek. Gdy zegar jest zsynchronizowany (czas odsyła odbiornik
// w odpowiedzi na każdą ramkę), wysyłki są wyrównane do zegara: o pełnej
// godzinie oraz +15, +30 i +45 min. Bez zegara (np. odbiornik nie ma czasu
// z NTP) - co SEND_INTERVAL_MS licząc od poprzedniej wysyłki.
static const uint32_t SEND_INTERVAL_MS = 15 * 60000;

// Zworka trybu pracy (wewnętrzny pull-down):
//   pin niepodłączony / GND - między wysyłkami deep sleep
//   pin na 3.3V             - płytka cały czas pracuje
static const uint8_t DEEP_SLEEP_DISABLE_PIN = D3; // GPIO4

static bool deepSleepEnabled() {
  return digitalRead(DEEP_SLEEP_DISABLE_PIN) == LOW;
}

// Czas utrzymania stanu aktywnego po włączeniu zasilania / resecie, zanim
// nastąpi pierwsza wysyłka (np. na wgranie firmware lub podgląd monitora).
// Nie dotyczy wybudzenia z deep sleep - wtedy wysyłka następuje od razu.
static const uint32_t STARTUP_AWAKE_MS = 5 * 60000;
// Co ile wypisywać na Serial czas pozostały do pierwszej wysyłki
static const uint32_t STARTUP_LOG_INTERVAL_MS = 30000;

// Adresy I2C czujników
static const uint8_t SHT41_ADDR = 0x44;
// LPS22HB / LPS22HH: 0x5C (SA0 = GND) lub 0x5D (SA0 = VDD) - wykrywany przy starcie
static const uint8_t LPS_ADDR_CANDIDATES[] = {0x5C, 0x5D};
static uint8_t lpsAddr = 0x5D;

// SHT41 - pomiar wysokiej precyzji (max 8.3 ms)
static const uint8_t SHT41_CMD_MEASURE_HIGH = 0xFD;

// Rejestry LPS22HB / LPS22HH (mapa używanych rejestrów jest wspólna)
static const uint8_t LPS_WHO_AM_I = 0x0F;
static const uint8_t LPS22HB_WHO_AM_I_VALUE = 0xB1;
static const uint8_t LPS22HH_WHO_AM_I_VALUE = 0xB3;
static const uint8_t LPS_CTRL_REG1 = 0x10;
static const uint8_t LPS_CTRL_REG2 = 0x11;
static const uint8_t LPS_STATUS = 0x27;
static const uint8_t LPS_PRESS_OUT_XL = 0x28;

// Pomiar baterii: dzielnik 2 x 200 kOhm z BAT+ do A0 (D0 = GPIO1)
static const uint8_t BATTERY_PIN = A0;
static const float BATTERY_DIVIDER = 2.0f;
// Poniżej tego napięcia uznajemy, że bateria / dzielnik nie są podłączone
static const float BATTERY_MIN_VALID_V = 2.5f;

// Licznik przetrwa deep sleep (pamięć RTC), zeruje się po odłączeniu zasilania
RTC_DATA_ATTR static uint32_t seq = 0;
static bool lpsReady = false;

// Najbliższa wysyłka: czas zegara (Unix, ms) albo - gdy zegar nie jest
// zsynchronizowany (nextSlotMs == 0) - millis()
static int64_t nextSlotMs = 0;
static uint32_t nextSendMs = 0;
// Do tej chwili (millis) płytka nie usypia - okno czuwania po zimnym starcie
static uint32_t awakeUntilMs = 0;

// Synchronizacja zegara z odbiornikiem
// Ile wcześniej budzić się przed wysyłką - start płytki i radia trwa ~1.3 s,
// resztę program odczekuje, by pomiar wyszedł o pełnym kwadransie
static const uint32_t WAKE_ADVANCE_MS = 2000;
// Maksymalny czas oczekiwania na czas od odbiornika po potwierdzeniu ramki
static const uint32_t TIME_REPLY_TIMEOUT_MS = 300;
// Czas uznawany za poprawny (po 2024-01-01) - po podaniu zasilania zegar
// ESP liczy od 1970
static const time_t TIME_VALID_AFTER = 1704067200;
// Zegar RTC odmierzający deep sleep (wewnętrzny generator RC) potrafi się
// mylić o ułamek procenta - kilka sekund na 15 min. Błąd jest mierzony przy
// każdej synchronizacji i korygowany (czas snu i zegar po wybudzeniu).
// rtcDrift = (czas rzeczywisty - czas wg RTC) / czas wg RTC
RTC_DATA_ATTR static float rtcDrift = 0;
RTC_DATA_ATTR static bool rtcDriftKnown = false;
static const float RTC_DRIFT_MAX = 0.05f;
// Ostatni sen (wg RTC) i suma snu od ostatniej synchronizacji
RTC_DATA_ATTR static int64_t lastSleepMs = 0;
RTC_DATA_ATTR static int64_t unsyncedSleepMs = 0;

// Czas z odbiornika - zapisywany w callbacku, przetwarzany w measureAndSend()
static portMUX_TYPE timeMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool timeReplyReceived = false;
static int64_t timeReplyUs = 0;

static bool clockValid() { return time(nullptr) > TIME_VALID_AFTER; }

static int64_t clockUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static int64_t clockMs() { return clockUs() / 1000; }

static void clockSetUs(int64_t us) {
  struct timeval tv;
  tv.tv_sec = (time_t)(us / 1000000LL);
  tv.tv_usec = (suseconds_t)(us % 1000000LL);
  settimeofday(&tv, nullptr);
}

// Formatuje czas lokalny (ms od 1970) jako "RRRR-MM-DD GG:MM:SS"
static const char *formatClock(int64_t ms) {
  static char buf[24];
  time_t t = (time_t)(ms / 1000);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

// Pierwszy pełny kwadrans po chwili t
static int64_t slotAfter(int64_t t) {
  const int64_t slot = SEND_INTERVAL_MS;
  return (t / slot + 1) * slot;
}

// Kwadrans najbliższy chwili t (po wybudzeniu - zegar może się nieco mylić)
static int64_t slotNearest(int64_t t) {
  const int64_t slot = SEND_INTERVAL_MS;
  return (t + slot / 2) / slot * slot;
}

// Ustawia zegar według czasu od odbiornika i aktualizuje oszacowanie błędu RTC
static void applyTimeReply(int64_t remoteUs) {
  bool wasValid = clockValid();
  int64_t offsetMs = (remoteUs - clockUs()) / 1000;
  clockSetUs(remoteUs);
  // Błąd RTC da się zmierzyć, gdy od ostatniej synchronizacji płytka spała
  // dość długo. Każdy sen był już skorygowany o rtcDrift, więc offset to
  // błąd resztkowy.
  if (wasValid && unsyncedSleepMs >= 60000 && llabs(offsetMs) < 120000) {
    float measured = rtcDrift + (float)offsetMs / (float)unsyncedSleepMs;
    measured = constrain(measured, -RTC_DRIFT_MAX, RTC_DRIFT_MAX);
    rtcDrift = rtcDriftKnown ? (rtcDrift + measured) / 2 : measured;
    rtcDriftKnown = true;
  }
  unsyncedSleepMs = 0;
  if (wasValid)
    Serial.printf("  Czas z odbiornika: %s (korekta %+lld ms, blad RTC %+.3f%%)\n",
                  formatClock(remoteUs / 1000), (long long)offsetMs,
                  rtcDrift * 100.0f);
  else
    Serial.printf("  Czas z odbiornika: %s (pierwsza synchronizacja)\n",
                  formatClock(remoteUs / 1000));
}

// Maksymalny czas oczekiwania na potwierdzenie wysyłki ESP-NOW
static const uint32_t SEND_CB_TIMEOUT_MS = 200;
static volatile bool sendDone = false;
static volatile bool sendOk = false;
// Ponowienia wysyłki, gdy odbiornik nie potwierdzi ramki (np. tuż po starcie
// radio nie jest jeszcze gotowe albo odbiornik dopiero się uruchamia)
static const int SEND_RETRIES = 10;
static const uint32_t SEND_RETRY_DELAY_MS = 300;
// Po zimnym starcie (np. podłączenie baterii) okno ponowień jest dłuższe -
// zasilanie i radio mogą stabilizować się kilka sekund (~60 x 0.5 s = 30 s)
static const int SEND_RETRIES_STARTUP = 60;

// Opóźnienie przed włączeniem radia po zimnym starcie / resecie brownout -
// daje czas na ustabilizowanie napięcia baterii (drgania styku, LDO)
static const uint32_t POWERON_SETTLE_MS = 500;
static const uint32_t BROWNOUT_SETTLE_MS = 3000;

// Dioda użytkownika XIAO ESP32S3 (GPIO21, aktywna stanem niskim) - sygnalizuje
// wynik wysyłki startowej, co pozwala sprawdzić działanie bez USB:
//   1 długie mignięcie - odbiornik potwierdził ramkę
//   3 krótkie mignięcia - brak potwierdzenia
static const uint8_t STATUS_LED_PIN = LED_BUILTIN;

static void ledBlink(int count, uint32_t onMs, uint32_t offMs) {
  pinMode(STATUS_LED_PIN, OUTPUT);
  for (int i = 0; i < count; i++) {
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(onMs);
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(offMs);
  }
}

static uint8_t sht41Crc(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
  }
  return crc;
}

static bool sht41Read(float &temperature, float &humidity) {
  Wire.beginTransmission(SHT41_ADDR);
  Wire.write(SHT41_CMD_MEASURE_HIGH);
  if (Wire.endTransmission() != 0)
    return false;
  delay(10);

  uint8_t buf[6];
  if (Wire.requestFrom(SHT41_ADDR, (uint8_t)6) != 6)
    return false;
  for (int i = 0; i < 6; i++)
    buf[i] = Wire.read();
  if (sht41Crc(buf, 2) != buf[2] || sht41Crc(buf + 3, 2) != buf[5])
    return false;

  uint16_t rawT = (buf[0] << 8) | buf[1];
  uint16_t rawH = (buf[3] << 8) | buf[4];
  temperature = -45.0f + 175.0f * rawT / 65535.0f;
  humidity = constrain(-6.0f + 125.0f * rawH / 65535.0f, 0.0f, 100.0f);
  return true;
}

static bool lpsWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(lpsAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool lpsRead(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(lpsAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom(lpsAddr, len) != len)
    return false;
  for (int i = 0; i < len; i++)
    buf[i] = Wire.read();
  return true;
}

static bool lpsInit() {
  bool found = false;
  for (uint8_t addr : LPS_ADDR_CANDIDATES) {
    lpsAddr = addr;
    uint8_t id = 0;
    if (!lpsRead(LPS_WHO_AM_I, &id, 1)) {
      Serial.printf("LPS22: brak odpowiedzi na 0x%02X\n", addr);
      continue;
    }
    Serial.printf("LPS22: adres 0x%02X, WHO_AM_I = 0x%02X\n", addr, id);
    if (id == LPS22HB_WHO_AM_I_VALUE || id == LPS22HH_WHO_AM_I_VALUE) {
      Serial.printf("LPS22: wykryto %s\n",
                    id == LPS22HB_WHO_AM_I_VALUE ? "LPS22HB" : "LPS22HH");
      found = true;
      break;
    }
  }
  if (!found)
    return false;
  // ODR = power-down (tryb one-shot), BDU = 1
  return lpsWrite(LPS_CTRL_REG1, 0x02);
}

static bool lpsReadPressure(float &pressure) {
  // IF_ADD_INC | ONE_SHOT - wyzwolenie pojedynczego pomiaru
  if (!lpsWrite(LPS_CTRL_REG2, 0x11))
    return false;

  uint8_t status = 0;
  uint32_t start = millis();
  do {
    delay(2);
    if (!lpsRead(LPS_STATUS, &status, 1))
      return false;
    if (millis() - start > 100)
      return false;
  } while (!(status & 0x01)); // P_DA

  uint8_t buf[3];
  if (!lpsRead(LPS_PRESS_OUT_XL, buf, 3))
    return false;
  int32_t raw = (int32_t)((uint32_t)buf[2] << 24 | (uint32_t)buf[1] << 16 |
                          (uint32_t)buf[0] << 8) >>
                8;
  pressure = raw / 4096.0f;
  return true;
}

static float batteryReadVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++)
    sum += analogReadMilliVolts(BATTERY_PIN);
  return sum / 16 / 1000.0f * BATTERY_DIVIDER;
}

// Przybliżona krzywa rozładowania ogniwa Li-Po/Li-Ion (napięcie spoczynkowe)
static int batteryPercent(float v) {
  static const float volts[] = {3.30f, 3.50f, 3.60f, 3.70f, 3.75f,
                                3.80f, 3.85f, 3.95f, 4.05f, 4.20f};
  static const int pct[] = {0, 5, 10, 20, 30, 40, 50, 65, 80, 100};
  const int n = sizeof(volts) / sizeof(volts[0]);
  if (v <= volts[0])
    return 0;
  if (v >= volts[n - 1])
    return 100;
  for (int i = 1; i < n; i++) {
    if (v < volts[i]) {
      return lroundf(pct[i - 1] + (v - volts[i - 1]) * (pct[i] - pct[i - 1]) /
                                      (volts[i] - volts[i - 1]));
    }
  }
  return 100;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3 &&                                          \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
static void onSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  Serial.printf("  -> status: %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
  sendOk = status == ESP_NOW_SEND_SUCCESS;
  sendDone = true;
}

// Odpowiedź odbiornika z bieżącym czasem: {"Time":<s>,"Us":<us>}
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onReceive(const esp_now_recv_info_t *info, const uint8_t *data,
                      int len) {
  const uint8_t *mac = info->src_addr;
#else
static void onReceive(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (memcmp(mac, RECEIVER_MAC, 6) != 0 || len <= 0 || len > PACKET_MAX_LEN)
    return;
  JsonDocument doc;
  if (deserializeJson(doc, data, len))
    return;
  JsonVariant t = doc[KEY_TIME];
  if (!t.is<long long>())
    return;
  int64_t us = (int64_t)t.as<long long>() * 1000000LL + doc[KEY_TIME_US].as<long>();
  if (us / 1000000LL <= TIME_VALID_AFTER)
    return;
  portENTER_CRITICAL(&timeMux);
  timeReplyUs = us;
  timeReplyReceived = true;
  portEXIT_CRITICAL(&timeMux);
}

// Deep sleep na realMs (czas rzeczywisty) - czas snu wg RTC jest korygowany
// o zmierzony błąd zegara RTC
static void sleepFor(int64_t realMs) {
  if (realMs < 1000)
    realMs = 1000;
  int64_t rtcMs = (int64_t)(realMs / (1.0f + rtcDrift));
  if (clockValid())
    Serial.printf("Deep sleep do %s\n", formatClock(clockMs() + realMs));
  else
    Serial.printf("Deep sleep na %lld s\n", (long long)(realMs / 1000));
  lastSleepMs = rtcMs;
  unsyncedSleepMs += rtcMs;
  Serial.flush();
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)rtcMs * 1000ULL);
  esp_deep_sleep_start();
}

// Czas (ms) do najbliższej wysyłki według harmonogramu
static int64_t msUntilNextSend() {
  if (nextSlotMs)
    return nextSlotMs - clockMs();
  return (int32_t)(nextSendMs - millis());
}

// Błąd inicjalizacji radia - ponowna próba przy najbliższym kwadransie
// (bez zegara - po SEND_INTERVAL_MS)
static void initFailed() {
  int64_t waitMs = clockValid() ? slotAfter(clockMs()) - clockMs() - WAKE_ADVANCE_MS
                                : SEND_INTERVAL_MS;
  if (deepSleepEnabled())
    sleepFor(waitMs);
  Serial.flush();
  delay(waitMs > 0 ? (uint32_t)waitMs : 1000);
  ESP.restart();
}

static bool measureAndSend(int retries = SEND_RETRIES);

void setup() {
  // Dioda świeci od początku startu do wyniku wysyłki startowej - pozwala
  // odróżnić brak startu (nie zapala się), zawieszenie (świeci ciągle)
  // i resety (zapala się raz za razem)
  bool coldStart = esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER;
  if (coldStart) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
  }
  // Zegar przetrwał deep sleep, ale czas snu odmierzał niedokładny RTC -
  // korekta o zmierzony błąd
  if (!coldStart && clockValid() && lastSleepMs > 0)
    clockSetUs(clockUs() + (int64_t)(lastSleepMs * rtcDrift) * 1000LL);
  setenv("TZ", TIME_ZONE, 1);
  tzset();
  Serial.begin(115200);
  pinMode(DEEP_SLEEP_DISABLE_PIN, INPUT_PULLDOWN);
  delay(1000);

  esp_reset_reason_t resetReason = esp_reset_reason();
  Serial.printf("Powod resetu: %d%s\n", (int)resetReason,
                resetReason == ESP_RST_BROWNOUT ? " (BROWNOUT - za niskie napiecie)" : "");
  if (resetReason == ESP_RST_BROWNOUT)
    delay(BROWNOUT_SETTLE_MS);
  else if (resetReason == ESP_RST_POWERON)
    delay(POWERON_SETTLE_MS);

  Serial.printf("Deep sleep: %s\n", deepSleepEnabled() ? "wlaczony" : "wylaczony (pin na 3.3V)");

  // XIAO ESP32S3: SDA = D4 (GPIO5), SCL = D5 (GPIO6)
  Wire.begin(SDA, SCL);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  lpsReady = lpsInit();
  if (!lpsReady)
    Serial.println("LPS22 nie odpowiada");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  // Przy pełnej mocy TX transmisja bywa gubiona (spadki napięcia, mała
  // antena)
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  uint8_t ownMac[6] = {};
  esp_wifi_get_mac(WIFI_IF_STA, ownMac);
  Serial.printf("\n[NADAJNIK] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", ownMac[0],
                ownMac[1], ownMac[2], ownMac[3], ownMac[4], ownMac[5]);
  if (memcmp(ownMac, SENDER_MAC, 6) != 0) {
    Serial.println("UWAGA: MAC plytki rozni sie od SENDER_MAC w packet.h - "
                   "odbiornik odrzuci ramki");
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("Blad esp_now_init()");
    initFailed();
  }
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RECEIVER_MAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Blad esp_now_add_peer()");
    initFailed();
  }

  bool wokeFromSleep = !coldStart;
  awakeUntilMs = wokeFromSleep ? 0 : STARTUP_AWAKE_MS;

  // Po zimnym starcie jednorazowa wysyłka od razu (bez deep sleep po niej) -
  // przy okazji synchronizuje zegar, dalej wysyłki o pełnych kwadransach
  if (!wokeFromSleep) {
    Serial.println("Wysylka startowa");
    bool ok = measureAndSend(SEND_RETRIES_STARTUP);
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(500);
    if (ok)
      ledBlink(1, 1000, 0);
    else
      ledBlink(3, 150, 150);
  }

  if (clockValid()) {
    // Po wybudzeniu: kwadrans, na który ustawiono budzik (zwykle kilka
    // sekund przed nim - loop() odczeka resztę). Po zimnym starcie: najbliższy.
    int64_t now = clockMs();
    nextSlotMs = wokeFromSleep ? slotNearest(now) : slotAfter(now);
  } else {
    // Bez zegara - harmonogram względny jak dawniej
    nextSendMs = wokeFromSleep ? 0 : STARTUP_AWAKE_MS;
  }

  if (!wokeFromSleep && STARTUP_AWAKE_MS > 0) {
    Serial.printf("Tryb aktywny przez %lu s\n",
                  (unsigned long)(STARTUP_AWAKE_MS / 1000));
    if (nextSlotMs)
      Serial.printf("Kolejna wysylka: %s\n", formatClock(nextSlotMs));
    else
      Serial.println("Brak czasu z odbiornika - wysylki co 15 min od startu");
  }
}

// Ustala termin kolejnej wysyłki po wysyłce zaplanowanej na servedSlotMs
// (lub servedSendMs przy harmonogramie bez zegara)
static void scheduleNext(int64_t servedSlotMs, uint32_t servedSendMs) {
  if (clockValid()) {
    int64_t now = clockMs();
    // Od obsłużonego kwadransa, a nie od bieżącej chwili - po korekcie zegara
    // (np. cofnięciu o kilka sekund) ten sam kwadrans nie wypadnie drugi raz
    int64_t next = servedSlotMs ? servedSlotMs + SEND_INTERVAL_MS : slotAfter(now);
    while (next <= now)
      next += SEND_INTERVAL_MS;
    nextSlotMs = next;
    Serial.printf("Kolejna wysylka: %s\n", formatClock(nextSlotMs));
  } else {
    nextSlotMs = 0;
    nextSendMs = servedSendMs + SEND_INTERVAL_MS;
  }
}

void loop() {
  int64_t remaining = msUntilNextSend();
  if (remaining > 0) {
    bool awakeWindow = (int32_t)(awakeUntilMs - millis()) > 0;
    // Uśpienie, gdy do wysyłki zostało więcej niż czas startu po wybudzeniu
    if (deepSleepEnabled() && !awakeWindow && remaining > WAKE_ADVANCE_MS + 1000)
      sleepFor(remaining - (nextSlotMs ? WAKE_ADVANCE_MS : 0));
    static uint32_t lastLogMs = 0;
    if (awakeWindow && millis() - lastLogMs >= STARTUP_LOG_INTERVAL_MS) {
      lastLogMs = millis();
      Serial.printf("Wysylka za %ld s\n", (long)(remaining / 1000));
    }
    delay(remaining > 10 ? 10 : (uint32_t)remaining);
    return;
  }

  int64_t servedSlotMs = nextSlotMs;
  uint32_t servedSendMs = nextSendMs;
  measureAndSend();
  scheduleNext(servedSlotMs, servedSendMs);
}

static bool measureAndSend(int retries) {
  seq++;
  if (clockValid())
    Serial.printf("[%s] ", formatClock(clockMs()));

  // Temperatura z dokładnością 0.1, wilgotność i ciśnienie jako liczby
  // całkowite
  auto round1 = [](float v) { return roundf(v * 10.0f) / 10.0f; };

  JsonDocument doc;
  float t, h, p;
  if (sht41Read(t, h)) {
    doc[KEY_TEMPERATURE] = round1(t);
    doc[KEY_HUMIDITY] = (int)lroundf(h);
  } else {
    doc[KEY_TEMPERATURE] = nullptr;
    doc[KEY_HUMIDITY] = nullptr;
    Serial.println("Blad odczytu SHT41");
  }

  if (lpsReady && lpsReadPressure(p)) {
    doc[KEY_PRESSURE] = (int)lroundf(p);
  } else {
    doc[KEY_PRESSURE] = nullptr;
    Serial.println("Blad odczytu LPS22");
  }

  float vbat = batteryReadVoltage();
  if (vbat >= BATTERY_MIN_VALID_V) {
    doc[KEY_BATTERY] = batteryPercent(vbat);
    doc[KEY_BATTERY_VOLTAGE] = roundf(vbat * 100.0f) / 100.0f;
  } else {
    doc[KEY_BATTERY] = nullptr;
    doc[KEY_BATTERY_VOLTAGE] = nullptr;
  }

  char json[PACKET_MAX_LEN];
  size_t len = serializeJson(doc, json, sizeof(json));
  if (len == 0 || len >= sizeof(json)) {
    Serial.println("JSON za dlugi");
    return false;
  }

  Serial.printf("Wysylam #%lu: %s\n", (unsigned long)seq, json);

  timeReplyReceived = false;
  for (int attempt = 1; attempt <= retries; attempt++) {
    sendDone = false;
    sendOk = false;
    esp_err_t res = esp_now_send(RECEIVER_MAC, (const uint8_t *)json, len);
    if (res != ESP_OK) {
      Serial.printf("  esp_now_send blad: %s\n", esp_err_to_name(res));
    } else {
      uint32_t start = millis();
      while (!sendDone && millis() - start < SEND_CB_TIMEOUT_MS)
        delay(1);
      if (sendOk) {
        // Odbiornik odsyła czas zaraz po odebraniu ramki
        start = millis();
        while (!timeReplyReceived && millis() - start < TIME_REPLY_TIMEOUT_MS)
          delay(1);
        if (timeReplyReceived) {
          portENTER_CRITICAL(&timeMux);
          int64_t us = timeReplyUs;
          portEXIT_CRITICAL(&timeMux);
          applyTimeReply(us);
        } else {
          Serial.println("  Brak czasu od odbiornika");
        }
        return true;
      }
    }
    if (attempt < retries) {
      Serial.printf("  ponowienie %d/%d\n", attempt, retries - 1);
      delay(SEND_RETRY_DELAY_MS);
    }
  }
  Serial.println("  Brak potwierdzenia od odbiornika");
  return false;
}
