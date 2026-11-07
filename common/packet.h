#pragma once

// Format ramki wspólny dla nadajnika i odbiornika.
// Ładunek to tekst JSON (bez kończącego '\0'), np.:
//   {"Temperature":23.5,"Humidity":45,"Pressure":1013,
//    "Battery":87,"BatteryVoltage":4.08}
// Battery - poziom naładowania w %, BatteryVoltage - napięcie w V.
// Pole ma wartość null, gdy odczyt danego czujnika się nie powiódł.

// Maksymalny rozmiar ładunku ESP-NOW to 250 bajtów.
#define PACKET_MAX_LEN 250

// Kanał Wi-Fi - musi być taki sam po obu stronach.
#define ESPNOW_CHANNEL 1

#include <stdint.h>

// MAC nadajnika (płytka XIAO ESP32S3)
static const uint8_t SENDER_MAC[6] = {0xB8, 0xF8, 0x62, 0xFB, 0x91, 0x7C};

// Klucze JSON
#define KEY_TEMPERATURE "Temperature"
#define KEY_HUMIDITY "Humidity"
#define KEY_PRESSURE "Pressure"
#define KEY_BATTERY "Battery"
#define KEY_BATTERY_VOLTAGE "BatteryVoltage"
