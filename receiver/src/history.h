#pragma once

#include "display.h"
#include <stdint.h>

// Historia odczytów nadajnika w pliku na LittleFS (partycja "spiffs"),
// bufor cykliczny o stałym rozmiarze. Nadajnik wysyła co 15 min, więc
// HISTORY_SIZE wpisów = ostatni miesiąc. Najstarszy wpis jest nadpisywany,
// gdy bufor jest pełny. Dane przetrwają restart, odłączenie zasilania
// i wgranie nowego programu.
#define HISTORY_DAYS 31
#define HISTORY_PER_DAY 96 // co 15 min
#define HISTORY_SIZE (HISTORY_DAYS * HISTORY_PER_DAY)

// Jeden zapisany odczyt (16 B). Zmiana układu wymaga zmiany
// HISTORY_VERSION w history.cpp.
struct HistoryEntry {
  uint32_t time;  // czas odbioru (Unix, UTC); z HF_UPTIME - sekundy od startu
  int16_t t10;    // temperatura * 10 [°C]
  uint16_t p;     // ciśnienie [hPa]
  uint16_t batMv; // napięcie baterii [mV]
  uint16_t boot;  // numer uruchomienia odbiornika
  uint8_t h;      // wilgotność [%]
  uint8_t bat;    // poziom baterii [%]
  int8_t rssi;    // [dBm]
  uint8_t flags;  // HF_* - które pola są ważne
};
static_assert(sizeof(HistoryEntry) == 16, "HistoryEntry musi miec 16 B");

enum : uint8_t {
  HF_T = 1 << 0,
  HF_H = 1 << 1,
  HF_P = 1 << 2,
  HF_BAT = 1 << 3,
  HF_RSSI = 1 << 4,
  HF_UPTIME = 1 << 5, // zegar niezsynchronizowany - time to sekundy od startu
};

// Montuje LittleFS i otwiera plik historii (tworzy go przy pierwszym
// starcie, przenosząc wpisy z dawnej historii w NVS). false = błąd pamięci.
bool historyBegin();

// Dopisuje odczyt. Wywoływać z loop(), nie z callbacku ESP-NOW.
void historyAdd(const SensorData &d);

// Obsługa komend z portu szeregowego ('h' - wypisz historię).
// Wywoływać cyklicznie w loop().
void historyLoop();

// Liczba zapisanych wpisów (0..HISTORY_SIZE).
uint16_t historyCount();

// Wpis nr i, gdzie 0 = najstarszy, historyCount()-1 = najnowszy.
bool historyGet(uint16_t i, HistoryEntry &out);

// Wypisuje całą historię na Serial (CSV).
void historyPrint();
