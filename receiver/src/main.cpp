#include "display.h"
#include "packet.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// Dane współdzielone między callbackiem ESP-NOW (task Wi-Fi) a loop()
static SensorData g_data;
static bool g_dataChanged = false;
static portMUX_TYPE g_dataMux = portMUX_INITIALIZER_UNLOCKED;

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

void setup() {
  Serial.begin(115200);
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

  if (esp_now_init() != ESP_OK) {
    Serial.println("Blad esp_now_init()");
    while (true)
      delay(1000);
  }
  esp_now_register_recv_cb(onReceive);
}

void loop() {
  portENTER_CRITICAL(&g_dataMux);
  SensorData d = g_data;
  bool changed = g_dataChanged;
  g_dataChanged = false;
  portEXIT_CRITICAL(&g_dataMux);

  displayUpdate(d, changed);
  delay(10);
}
