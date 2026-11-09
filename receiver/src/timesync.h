#pragma once

#include <time.h>

// Synchronizacja zegara przez Wi-Fi + NTP.
//
// ESP-NOW pracuje na stałym kanale ESPNOW_CHANNEL, a połączenie z routerem
// przestawia radio na kanał routera. Dlatego odbiornik łączy się z Wi-Fi
// tylko na czas synchronizacji (kilka sekund), potem rozłącza się i wraca
// na kanał ESP-NOW. Między synchronizacjami czas odmierza zegar ESP.

// Pierwsza synchronizacja przy starcie (blokująca, maks. ok. 40 s).
// Wywołać przed esp_now_init().
void timeSyncBegin();

// Wywoływać cyklicznie w loop(). packetReceived = właśnie przyszedł pakiet -
// kolejna synchronizacja startuje tylko wtedy (z kilkusekundowym
// opóźnieniem na odesłanie czasu nadajnikowi), bo do następnego pakietu
// jest najwięcej czasu i nic nie zostanie przegapione.
void timeSyncLoop(bool packetReceived);

// Czy zegar był choć raz zsynchronizowany.
bool timeIsValid();

// Aktualny czas (Unix, UTC) lub 0, gdy zegar nie jest zsynchronizowany.
time_t timeNow();

// Formatuje czas lokalny jako "RRRR-MM-DD GG:MM:SS"; dla 0 zwraca "".
void timeFormat(time_t t, char *buf, size_t len);
