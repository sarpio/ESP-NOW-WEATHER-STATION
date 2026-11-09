#include "timesync.h"
#include "display.h"
#include "packet.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#error "Brak src/wifi_secrets.h - skopiuj src/wifi_secrets.example.h i wpisz dane sieci"
#endif

static const char *TZ_INFO = TIME_ZONE;
static const char *NTP_SERVERS[] = {
    "tempus1.gum.gov.pl", // Główny Urząd Miar
    "pool.ntp.org",
    "time.google.com",
};
static constexpr uint8_t NTP_SERVER_COUNT = sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]);

static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000; // łączenie z Wi-Fi
static constexpr uint32_t NTP_TIMEOUT_MS = 25000;     // oczekiwanie na NTP
// SNTP w rdzeniu wysyła pierwsze zapytanie z losowym opóźnieniem do 5 s,
// a do kolejnego serwera przechodzi dopiero po 15 s bez odpowiedzi -
// ponawiamy sami, częściej, za każdym razem zaczynając od innego serwera
// (restart SNTP zawsze zaczyna od serwera nr 0).
static constexpr uint32_t NTP_RETRY_MS = 7000;
static constexpr uint32_t RESYNC_INTERVAL_MS = 6 * 3600000UL; // po udanej
static constexpr uint32_t RETRY_INTERVAL_MS = 30 * 60000UL;   // po nieudanej
// Opóźnienie startu synchronizacji po odebraniu pakietu
static constexpr uint32_t SYNC_DELAY_MS = 3000;

// Czas uznawany za poprawny (po 2024-01-01) - przed synchronizacją zegar
// ESP liczy od 1970.
static constexpr time_t VALID_AFTER = 1704067200;

enum class State { Idle, Connecting, WaitNtp };
static State state = State::Idle;
static uint32_t attemptStart = 0;
static uint32_t ntpStart = 0;
static uint32_t ntpRequest = 0;
static uint32_t lastAttempt = 0;
static bool lastOk = false;
static volatile bool ntpDone = false;
static bool booting = false; // pierwsza synchronizacja - postęp na ekranie
static uint8_t ntpFirst = 0; // od którego serwera zaczyna bieżące zapytanie

static void bootResult(const char *text, BootState st) {
  if (booting)
    displayBootResult(text, st);
}

static void onNtpSync(struct timeval *) { ntpDone = true; }

// (Re)start SNTP z listą serwerów przesuniętą o ntpFirst.
static void requestNtp() {
  const char *s[NTP_SERVER_COUNT];
  for (uint8_t i = 0; i < NTP_SERVER_COUNT; i++)
    s[i] = NTP_SERVERS[(ntpFirst + i) % NTP_SERVER_COUNT];
  Serial.printf("[CZAS] Zapytanie NTP: %s\n", s[0]);
  configTzTime(TZ_INFO, s[0], s[1], s[2]);
}

static void startSync() {
  if (strlen(WIFI_SSID) == 0) {
    Serial.println("[CZAS] Brak SSID w wifi_secrets.h - synchronizacja wylaczona");
    if (booting) {
      displayBootStep("Synchronizacja czasu");
      displayBootResult("wylaczona (brak SSID)", BootState::Error);
    }
    lastAttempt = millis();
    lastOk = false;
    return;
  }
  Serial.printf("[CZAS] Laczenie z Wi-Fi \"%s\"...\n", WIFI_SSID);
  if (booting) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Wi-Fi: %s", WIFI_SSID);
    displayBootStep(buf);
  }
  attemptStart = lastAttempt = millis();
  ntpDone = false;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  state = State::Connecting;
}

static void finish(bool ok) {
  esp_sntp_stop();
  WiFi.disconnect(false, false);
  // Powrót na kanał ESP-NOW
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  state = State::Idle;
  lastOk = ok;
  if (ok) {
    char buf[24];
    timeFormat(timeNow(), buf, sizeof(buf));
    Serial.printf("[CZAS] Zsynchronizowano: %s\n", buf);
    bootResult(buf, BootState::Ok);
  } else {
    Serial.printf("[CZAS] Synchronizacja nieudana (ponowienie za %lu min)\n",
                  (unsigned long)(RETRY_INTERVAL_MS / 60000));
  }
  Serial.printf("[CZAS] Wi-Fi rozlaczone, powrot na kanal %d\n", ESPNOW_CHANNEL);
}

static void step() {
  if (state == State::Idle)
    return;
  uint32_t now = millis();
  if ((state == State::Connecting && now - attemptStart > CONNECT_TIMEOUT_MS) ||
      (state == State::WaitNtp && now - ntpStart > NTP_TIMEOUT_MS)) {
    Serial.println(state == State::Connecting ? "[CZAS] Brak polaczenia z Wi-Fi"
                                              : "[CZAS] Brak odpowiedzi serwera NTP");
    bootResult(state == State::Connecting ? "brak polaczenia" : "brak odpowiedzi",
               BootState::Error);
    finish(false);
    return;
  }
  if (state == State::Connecting && WiFi.status() == WL_CONNECTED) {
    Serial.printf("[CZAS] Polaczono (kanal %d, IP %s), pobieranie czasu NTP...\n",
                  WiFi.channel(), WiFi.localIP().toString().c_str());
    if (booting) {
      char buf[24];
      snprintf(buf, sizeof(buf), "OK (kanal %d)", WiFi.channel());
      displayBootResult(buf, BootState::Ok);
      displayBootStep("Pobieranie czasu (NTP)");
    }
    requestNtp();
    ntpStart = ntpRequest = now;
    state = State::WaitNtp;
  } else if (state == State::WaitNtp && ntpDone) {
    finish(true);
  } else if (state == State::WaitNtp && now - ntpRequest > NTP_RETRY_MS) {
    Serial.println("[CZAS] Brak odpowiedzi NTP - ponawianie z innym serwerem");
    ntpFirst = (ntpFirst + 1) % NTP_SERVER_COUNT;
    requestNtp();
    ntpRequest = now;
  }
}

void timeSyncBegin() {
  setenv("TZ", TZ_INFO, 1);
  tzset();
  WiFi.persistent(false);     // nie zapisuj danych sieci we flash
  WiFi.setAutoReconnect(false); // po rozłączeniu zostań na kanale ESP-NOW
  sntp_set_time_sync_notification_cb(onNtpSync);
  // Serwer NTP podany przez DHCP routera nie może nadpisać naszej listy
  // (bywa nieosiągalny, np. adres samego routera bez usługi NTP).
  esp_sntp_servermode_dhcp(false);

  booting = true;
  startSync();
  uint32_t lastShown = 0;
  while (state != State::Idle) {
    step();
    // Upływający czas oczekiwania - widać, że odbiornik nie zawiesił się
    uint32_t s = (millis() - attemptStart) / 1000;
    if (state != State::Idle && s >= 1 && s != lastShown) {
      lastShown = s;
      char buf[16];
      snprintf(buf, sizeof(buf), "%lu s", (unsigned long)s);
      displayBootResult(buf, BootState::Pending);
    }
    delay(50);
  }
  booting = false;
}

void timeSyncLoop(bool packetReceived) {
  // Synchronizacja startuje chwilę po pakiecie, a nie od razu: odbiornik
  // musi jeszcze na kanale ESP-NOW odesłać nadajnikowi czas i odebrać
  // ewentualne ponowienia ramki.
  static bool pending = false;
  static uint32_t packetAt = 0;
  if (packetReceived) {
    pending = true;
    packetAt = millis();
  }
  step();
  if (state != State::Idle || !pending || millis() - packetAt < SYNC_DELAY_MS)
    return;
  pending = false;
  uint32_t interval = lastOk ? RESYNC_INTERVAL_MS : RETRY_INTERVAL_MS;
  if (millis() - lastAttempt >= interval)
    startSync();
}

bool timeIsValid() { return time(nullptr) > VALID_AFTER; }

time_t timeNow() { return timeIsValid() ? time(nullptr) : 0; }

void timeFormat(time_t t, char *buf, size_t len) {
  if (t == 0) {
    buf[0] = 0;
    return;
  }
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}
