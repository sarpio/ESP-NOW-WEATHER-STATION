#include "display.h"
#include "history.h"
#include "packet.h"
#include "timesync.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <sys/time.h>

// Dane współdzielone między callbackiem ESP-NOW (task Wi-Fi) a loop()
static SensorData g_data;
static bool g_dataChanged = false;
static portMUX_TYPE g_dataMux = portMUX_INITIALIZER_UNLOCKED;

// Odsyła nadajnikowi bieżący czas - nadajnik ustawia według niego swój
// zegar i planuje wysyłki na pełne kwadranse. Wywoływane z callbacku
// odbioru, żeby odpowiedź wyszła od razu (nadajnik czeka na nią krótko,
// a loop() może być zajęta zapisem historii lub odświeżaniem ekranu).
static void sendTimeReply() {
  if (!timeIsValid())
    return;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "{\"%s\":%lld,\"%s\":%ld}", KEY_TIME,
                   (long long)tv.tv_sec, KEY_TIME_US, (long)tv.tv_usec);
  esp_err_t res = esp_now_send(SENDER_MAC, (const uint8_t *)buf, n);
  if (res != ESP_OK)
    Serial.printf("[CZAS] Blad wysylki czasu do nadajnika: %s\n",
                  esp_err_to_name(res));
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onReceive(const esp_now_recv_info_t *info, const uint8_t *data,
                      int len) {
  const uint8_t *mac = info->src_addr;
  const bool hasRssi = info->rx_ctrl != nullptr;
  const int rssi = hasRssi ? info->rx_ctrl->rssi : 0;
#else
static void onReceive(const uint8_t *mac, const uint8_t *data, int len) {
  const bool hasRssi = false;
  const int rssi = 0;
#endif
  if (memcmp(mac, SENDER_MAC, 6) != 0) { // tylko ramki od naszego nadajnika
    Serial.printf("Pominieto ramke (%d B) od nieznanego MAC "
                  "%02X:%02X:%02X:%02X:%02X:%02X\n",
                  len, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return;
  }
  if (len <= 0 || len > PACKET_MAX_LEN)
    return;
  sendTimeReply();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) {
    Serial.printf("Bledny JSON (%s, %d B) od %02X:%02X:%02X:%02X:%02X:%02X\n",
                  err.c_str(), len, mac[0], mac[1], mac[2], mac[3], mac[4],
                  mac[5]);
    return;
  }

  Serial.printf("Od %02X:%02X:%02X:%02X:%02X:%02X: %.*s\n", mac[0], mac[1],
                mac[2], mac[3], mac[4], mac[5], len, (const char *)data);

  JsonVariant t = doc[KEY_TEMPERATURE];
  JsonVariant h = doc[KEY_HUMIDITY];
  JsonVariant p = doc[KEY_PRESSURE];
  if (t.is<float>())
    Serial.printf("  T=%.1f C", t.as<float>());
  else
    Serial.print("  T=---");
  if (h.is<int>())
    Serial.printf(" H=%d %%", h.as<int>());
  else
    Serial.print(" H=---");
  JsonVariant b = doc[KEY_BATTERY];
  JsonVariant bv = doc[KEY_BATTERY_VOLTAGE];
  if (p.is<int>())
    Serial.printf(" P=%d hPa", p.as<int>());
  else
    Serial.print(" P=---");
  if (b.is<int>() && bv.is<float>())
    Serial.printf(" Bat=%d %% (%.2f V)\n", b.as<int>(), bv.as<float>());
  else
    Serial.println(" Bat=---");

  portENTER_CRITICAL(&g_dataMux);
  g_data.valid = true;
  g_data.ageUnknown = false;
  g_data.hasT = t.is<float>();
  g_data.t = g_data.hasT ? t.as<float>() : 0;
  g_data.hasH = h.is<int>();
  g_data.h = g_data.hasH ? h.as<int>() : 0;
  g_data.hasP = p.is<int>();
  g_data.p = g_data.hasP ? p.as<int>() : 0;
  g_data.hasBat = b.is<int>() && bv.is<float>();
  g_data.bat = g_data.hasBat ? b.as<int>() : 0;
  g_data.batV = g_data.hasBat ? bv.as<float>() : 0;
  g_data.hasRssi = hasRssi;
  g_data.rssi = rssi;
  g_data.rxMillis = millis();
  g_data.packets++;
  g_dataChanged = true;
  portEXIT_CRITICAL(&g_dataMux);
}

// Po restarcie pokazuje na ekranie ostatni odczyt z historii, zamiast czekać
// do 15 min na kolejny pakiet.
static void restoreLastReading() {
  HistoryEntry e;
  if (historyCount() == 0 || !historyGet(historyCount() - 1, e))
    return;
  SensorData d;
  d.valid = true;
  d.hasT = e.flags & HF_T;
  d.t = e.t10 / 10.0f;
  d.hasH = e.flags & HF_H;
  d.h = e.h;
  d.hasP = e.flags & HF_P;
  d.p = e.p;
  d.hasBat = e.flags & HF_BAT;
  d.bat = e.bat;
  d.batV = e.batMv / 1000.0f;
  d.hasRssi = e.flags & HF_RSSI;
  d.rssi = e.rssi;
  // Wiek odczytu ze znacznika czasu; millis() - rxMillis da wtedy właściwą
  // wartość (arytmetyka modulo 2^32). Bez znacznika wiek jest nieznany.
  time_t now = timeNow();
  if (!(e.flags & HF_UPTIME) && e.time && now >= (time_t)e.time)
    d.rxMillis = millis() - (uint32_t)(now - e.time) * 1000;
  else
    d.ageUnknown = true;

  portENTER_CRITICAL(&g_dataMux);
  g_data = d; // bez g_dataChanged - to nie nowy pakiet, nie trafia do historii
  portEXIT_CRITICAL(&g_dataMux);
  Serial.println("[HIST] Przywrocono ostatni odczyt");
}

void setup() {
  Serial.begin(115200);
  // Port USB podłączony do komputera, ale bez otwartego monitora: każdy
  // zapis czekałby domyślnie 100 ms na odbiór i start trwałby ponad minutę.
  // Z zerowym limitem nieodebrany tekst jest od razu pomijany.
  Serial.setTxTimeoutMs(0);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  // WiFi.macAddress() w core 3.x potrafi zwrócić same zera tuż po starcie -
  // czytamy z drivera
  uint8_t ownMac[6] = {};
  esp_wifi_get_mac(WIFI_IF_STA, ownMac);
  Serial.printf("\n[ODBIORNIK] MAC: %02X:%02X:%02X:%02X:%02X:%02X, kanal %d\n",
                ownMac[0], ownMac[1], ownMac[2], ownMac[3], ownMac[4],
                ownMac[5], ESPNOW_CHANNEL);

  displayBegin(ownMac);
  timeSyncBegin(); // przed historią - wypisywane wpisy dostają strefę czasową

  displayBootStep("Wczytywanie historii");
  char buf[32];
  if (historyBegin()) {
    restoreLastReading();
    snprintf(buf, sizeof(buf), "%u/%u odczytow", historyCount(), HISTORY_SIZE);
    displayBootResult(buf, BootState::Ok);
  } else {
    displayBootResult("blad pamieci", BootState::Error);
  }

  displayBootStep("Uruchamianie ESP-NOW");
  if (esp_now_init() != ESP_OK) {
    Serial.println("Blad esp_now_init()");
    displayBootResult("blad", BootState::Error);
    while (true)
      delay(1000);
  }
  esp_now_register_recv_cb(onReceive);
  // Nadajnik jako peer - odbiornik odsyła mu czas
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, SENDER_MAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&peer) != ESP_OK)
    Serial.println("Blad esp_now_add_peer() - nadajnik nie dostanie czasu");
  snprintf(buf, sizeof(buf), "OK (kanal %d)", ESPNOW_CHANNEL);
  displayBootResult(buf, BootState::Ok);
  delay(1500); // chwila na odczytanie podsumowania startu
  Serial.printf("[START] Gotowe po %lu ms\n", (unsigned long)millis());
}

void loop() {
  portENTER_CRITICAL(&g_dataMux);
  SensorData d = g_data;
  bool changed = g_dataChanged;
  g_dataChanged = false;
  portEXIT_CRITICAL(&g_dataMux);

  // Zapis do flash trwa zbyt długo na callback ESP-NOW - robimy go tutaj.
  // Pakiety przychodzą co 15 min, więc flaga nie gubi odczytów.
  // Przed displayUpdate(), żeby wykres od razu zawierał nowy odczyt.
  if (changed)
    historyAdd(d);
  displayUpdate(d, changed);
  timeSyncLoop(changed);
  historyLoop();
  delay(10);
}
