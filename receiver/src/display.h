#pragma once

#include <stdint.h>

// Ostatnie dane odebrane od nadajnika (kopiowane z callbacku ESP-NOW do pętli głównej).
struct SensorData {
  bool valid = false;  // czy odebrano już jakikolwiek pakiet
  bool ageUnknown = false; // odczyt z historii bez znacznika czasu (sprzed restartu)
  bool hasT = false, hasH = false, hasP = false, hasBat = false, hasRssi = false;
  float t = 0;
  int h = 0;
  int p = 0;
  int bat = 0;
  float batV = 0;
  int rssi = 0;
  uint32_t rxMillis = 0;  // millis() w chwili odbioru
  uint32_t packets = 0;   // licznik poprawnych pakietów
};

// Inicjalizacja wyświetlacza i dotyku (przy pierwszym starcie - kalibracja).
void displayBegin(const uint8_t ownMac[6]);

// Ekran startowy: lista kroków wykonywanych przed pełnym uruchomieniem
// (dotyk w tym czasie nie działa). displayBootStep() dodaje nowy wiersz,
// displayBootResult() wpisuje wynik po prawej stronie ostatniego wiersza.
enum class BootState { Pending, Ok, Error };
void displayBootStep(const char *text);
void displayBootResult(const char *result, BootState state);

// Wywoływać cyklicznie w loop(). dataChanged = przyszedł nowy pakiet.
void displayUpdate(const SensorData &d, bool dataChanged);
