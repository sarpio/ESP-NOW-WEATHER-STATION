# Stacja pogodowa – odbiornik ESP-NOW

Odbiornik danych pogodowych oparty na **ESP32-S3** z dotykowym wyświetlaczem
**4.0" SPI TFT (ST7796S, 480×320) z kontrolerem dotyku XPT2046**.
Urządzenie odbiera przez ESP-NOW pakiety JSON wysyłane przez nadajnik
(płytka XIAO ESP32S3, projekt `../sender`) i pokazuje je na ekranie.

## Funkcje

- Odbiór pakietów ESP-NOW wyłącznie od zdefiniowanego nadajnika (filtr po MAC).
- Ekran główny z czterema kafelkami: temperatura, wilgotność, ciśnienie,
  poziom i napięcie baterii nadajnika.
- Pasek stanu: *Czekam na dane* / *Online* / *Brak danych* (brak pakietu
  przez 35 min – nadajnik wysyła co 15 min, więc zgubienie jednego pakietu
  jest tolerowane).
- Stopka: czas od ostatniego odczytu, licznik pakietów, RSSI.
- Po restarcie ekran od razu pokazuje ostatni odczyt z historii (z wiekiem
  liczonym ze znacznika czasu, a bez niego – *przed restartem*).
- Dolny pasek z 5 przyciskami przełączającymi ekrany: *Odczyty*, *Temp.*,
  *Wilg.*, *Cisn.*, *Info*.
- Wykresy dobowe (ostatnie 24 h z historii) temperatury, wilgotności
  i ciśnienia: oś czasu kończy się na najbliższej pełnej godzinie, siatka
  co 1 h (opis co 2 h, o północy data); oś wartości z siatką co 1 °C / 1 % /
  1 hPa i opisami co 1/2/5/10… jednostek (tak, żeby się nie nakładały),
  zakres w pełnych jednostkach dopasowany do odczytów (min. 2 jednostki).
  Nad wykresem min, max i bieżąca wartość. Linia to płynna krzywa
  (monotoniczna interpolacja sześcienna); wilgotność i ciśnienie są
  dodatkowo wygładzane (regresja lokalna z wagami Gaussa, σ = 30 min / 1 h),
  bo zapisywane są w pełnych jednostkach. Przerwa w linii = odstęp między
  odczytami ponad 35 min. Wykres wymaga zsynchronizowanego zegara (NTP).
- Ekran informacji i stanu baterii: poziom i napięcie baterii nadajnika
  (ze zmianą napięcia w ciągu ostatnich 24 h, gdy historia obejmuje min.
  1 h), czas od ostatniego odczytu, MAC odbiornika i nadajnika, kanał
  ESP-NOW, napięcie czujnika światła z bieżącą korektą jasności, liczba
  pakietów, RSSI, czas pracy, wolna pamięć.
- Suwak jasności na ekranie informacji ustawia poziom bazowy podświetlenia
  (zapisywany w NVS).
- Automatyczna jasność z fotorezystora (przycisk *Auto* obok suwaka, stan
  zapisywany w NVS): jasność = poziom bazowy z suwaka + korekta z czujnika
  (ciemniej w ciemnym pomieszczeniu, jaśniej w jasnym). Suwak działa też
  w trybie auto – przesuwa całą charakterystykę. Płynne przejścia, filtr
  i histereza przeciw migotaniu. Napięcie z czujnika i korektę widać na
  ekranie informacji – patrz [Fotorezystor](#fotorezystor).
- Historia odczytów z ostatniego miesiąca (2976 odczytów co 15 min) w pamięci
  flash ESP (LittleFS), ze znacznikiem czasu z NTP – patrz
  [Historia odczytów](#historia-odczytów).
- Kalibracja dotyku przy pierwszym uruchomieniu (zapisywana w NVS);
  ponowną kalibrację wymusza przytrzymanie ekranu przez pierwsze 1,5 s
  (napis *Przytrzymaj ekran, aby skalibrować dotyk*).
- Ekran startowy z listą kroków i ich wynikami: połączenie Wi-Fi, pobranie
  czasu NTP (z licznikiem sekund), wczytanie historii, uruchomienie ESP-NOW.
  Dotyk w tym czasie nie działa.

## Sprzęt

| Element      | Opis                                                        |
|--------------|-------------------------------------------------------------|
| MCU          | ESP32-S3 N16R8 (16 MB flash QIO, 8 MB PSRAM OPI), DevKitC-1 |
| Wyświetlacz  | 4.0" SPI TFT, sterownik ST7796S, 480×320                    |
| Dotyk        | Rezystancyjny, kontroler XPT2046                            |
| Zasilanie    | USB (natywny port USB-Serial/JTAG ESP32-S3)                 |

## Połączenia ESP32-S3 ↔ TFT

Wyświetlacz i kontroler dotyku dzielą jedną magistralę SPI (**SPI2 / FSPI**).

### Wyświetlacz (ST7796S)

| Pin modułu TFT | ESP32-S3   | Uwagi                                         |
|----------------|------------|-----------------------------------------------|
| VCC            | 3V3        |                                               |
| GND            | GND        |                                               |
| DC/RS          | GPIO 9     | Data / Command                                |
| CS             | GPIO 10    | Chip select wyświetlacza                      |
| SDI (MOSI)     | GPIO 11    | Wspólne z T_DIN                               |
| SCK            | GPIO 12    | Wspólne z T_CLK                               |
| RESET          | GPIO 14    |                                               |
| LED            | GPIO 21    | Podświetlenie – PWM (kanał 7, 12 kHz)         |
| SDO (MISO)     | **nie podłączać** | Patrz uwaga poniżej                    |

### Dotyk (XPT2046)

| Pin modułu TFT | ESP32-S3   | Uwagi                                         |
|----------------|------------|-----------------------------------------------|
| T_IRQ          | GPIO 5     | Opcjonalne – obecnie wyłączone w kodzie (`PIN_TOUCH_IRQ = -1`) |
| T_DO           | GPIO 13    | MISO magistrali – używane tylko przez dotyk   |
| T_DIN          | GPIO 11    | Wspólne z SDI (MOSI)                          |
| T_CS           | GPIO 4     | Chip select dotyku                            |
| T_CLK          | GPIO 12    | Wspólne z SCK                                 |

### Fotorezystor

Dzielnik napięcia podłączony do **GPIO 1** (ADC1 – ADC2 nie działa przy
włączonym Wi-Fi):

```
 3V3 ── fotorezystor (np. GL5528) ──┬── GPIO 1
                                    └── rezystor 10 kΩ ── GND
```

Więcej światła = wyższe napięcie = jaśniejszy ekran. W trybie *Auto* do
poziomu bazowego z suwaka dodawana jest korekta: przy napięciu w połowie
zakresu `LIGHT_DARK_MV`..`LIGHT_BRIGHT_MV` (300–2600 mV) zerowa, przy
`LIGHT_DARK_MV` i poniżej ok. −48 %, przy `LIGHT_BRIGHT_MV` i powyżej
ok. +48 % (`AUTO_SWING`). Stałe są w `src/display.cpp`; bieżące napięcie
i korektę pokazuje wiersz *Czujnik światła* na ekranie informacji.

### Schemat zbiorczy

```
 ESP32-S3                     Moduł TFT 4.0" (ST7796S + XPT2046)
 ────────                     ──────────────────────────────────
 3V3      ──────────────────  VCC
 GND      ──────────────────  GND
 GPIO 4   ──────────────────  T_CS
 GPIO 5   ─ ─ ─ ─ ─ ─ ─ ─ ─   T_IRQ   (opcjonalnie)
 GPIO 9   ──────────────────  DC/RS
 GPIO 10  ──────────────────  CS
 GPIO 11  ────────┬─────────  SDI (MOSI)
                  └─────────  T_DIN
 GPIO 12  ────────┬─────────  SCK
                  └─────────  T_CLK
 GPIO 13  ──────────────────  T_DO
 GPIO 14  ──────────────────  RESET
 GPIO 21  ──────────────────  LED
          (niepodłączone)     SDO (MISO)
```

### Uwagi do połączeń

- **SDO (MISO) wyświetlacza zostaw niepodłączone.** W modułach ST7796 ta linia
  nie przechodzi w stan wysokiej impedancji i zakłóca odczyt dotyku
  na wspólnej magistrali. Dlatego w konfiguracji panelu jest
  `readable = false`.
- **T_IRQ** to wyjście open-drain bez podciągnięcia na module. Jeśli zostanie
  włączone (`PIN_TOUCH_IRQ 5` w `src/lgfx_config.h`), kod ustawia na nim
  wewnętrzny pull-up.
- Zegar zapisu SPI to 40 MHz. Przy zakłóceniach (długie przewody, artefakty
  na obrazie) obniż `freq_write` do 27 MHz w `src/lgfx_config.h`.
- Kolory odwrócone → `cfg.invert = true`; zamienione R z B →
  `cfg.rgb_order = true`; obraz obrócony o 180° → `lcd.setRotation(3)`
  w `src/display.cpp`.

## Historia odczytów

Każdy odebrany pakiet zapisywany jest w pamięci flash ESP – w pliku
`/history.bin` na systemie plików LittleFS (partycja `spiffs`, 3,4 MB).
Plik jest buforem cyklicznym na **2976 odczytów (31 dni × 96)** – gdy jest
pełny, nowy odczyt nadpisuje najstarszy. Jeden odczyt zajmuje 16 B, cały
plik ok. 47 KB. Historia przetrwa restart, odłączenie zasilania i wgranie
nowego programu (`upload` nie nadpisuje partycji `spiffs`; robi to tylko
`uploadfs`).

Odczyt historii przez port szeregowy (115200 baud, `pio device monitor`):

- przy starcie – cała historia wypisywana jest automatycznie, jeśli monitor
  portu jest otwarty,
- w dowolnej chwili – wpisz `h` w monitorze portu.

```
time,boot,uptime_s,temperature_c,humidity_pct,pressure_hpa,battery_pct,battery_v,rssi_dbm
2026-11-07 14:30:12,3,,23.5,45,1013,87,4.08,-61
,4,35,23.4,45,1013,87,4.08,-62
```

- `time` – czas lokalny odbioru (strefa CET/CEST).
- `boot` – numer uruchomienia odbiornika.
- `uptime_s` – sekundy od startu; wypełniane zamiast `time`, gdy zegar nie
  był zsynchronizowany (patrz [Czas z Wi-Fi](#czas-z-wi-fi-ntp)).
- Puste pole oznacza brak odczytu danego czujnika.
- Okres przechowywania: `HISTORY_DAYS` w `src/history.h`. Po zmianie plik
  jest przebudowywany przy starcie z zachowaniem najnowszych odczytów.
- Uszkodzony plik (lub zmiana formatu wpisu) jest tworzony od nowa.
- Przy pierwszym starcie z LittleFS historia z dawnej wersji (zapisywana
  w NVS) jest jednorazowo przenoszona do pliku, a NVS czyszczone.
- Po restarcie ekran od razu pokazuje ostatni zapisany odczyt.

## Czas z Wi-Fi (NTP)

Odbiornik nie ma zegara czasu rzeczywistego, więc czas pobiera z serwera NTP
(`tempus1.gum.gov.pl`, zapasowo `pool.ntp.org` i `time.google.com`) przez
Wi-Fi. Dane sieci wpisz w `src/wifi_secrets.h` (plik jest w `.gitignore`;
skopiuj wzór `src/wifi_secrets.example.h`). **Bez tego pliku projekt się nie
skompiluje.**

```c
#define WIFI_SSID "MojaSiec"
#define WIFI_PASSWORD "haslo"
```

ESP-NOW działa na stałym kanale `ESPNOW_CHANNEL`, a połączenie z routerem
przestawia radio na kanał routera. Dlatego odbiornik łączy się z Wi-Fi
tylko na czas synchronizacji (kilka sekund), potem rozłącza się i wraca na
kanał ESP-NOW; między synchronizacjami czas odmierza zegar ESP.

- Synchronizacja przy starcie, przed uruchomieniem ESP-NOW (maks. ok. 40 s:
  do 15 s na połączenie z Wi-Fi i do 25 s na odpowiedź NTP, zapytanie
  ponawiane co 7 s), potem co 6 h; po nieudanej próbie ponowienie co 30 min.
- Kolejne synchronizacje startują zaraz po odebraniu pakietu – do następnego
  zostaje 15 min, więc żaden odczyt nie przepada.
- Pusty `WIFI_SSID` wyłącza synchronizację (odczyty zapisywane bez czasu).
- Router musi obsługiwać 2,4 GHz.

## Struktura projektu

```
ESP-NOW/
├── common/
│   └── packet.h        # format ramki, kanał, MAC nadajnika, klucze JSON
├── sender/             # projekt nadajnika
└── receiver/           # ten projekt
    ├── platformio.ini
    └── src/
        ├── main.cpp        # Wi-Fi/ESP-NOW, odbiór i parsowanie JSON
        ├── display.h/.cpp  # interfejs użytkownika, dotyk, jasność
        ├── history.h/.cpp  # historia odczytów (miesiąc) na LittleFS
        ├── timesync.h/.cpp # Wi-Fi + NTP, znacznik czasu
        ├── wifi_secrets.h  # SSID i hasło Wi-Fi (poza gitem)
        └── lgfx_config.h   # piny i konfiguracja LovyanGFX
```

## Format danych

Ładunek ramki ESP-NOW to tekst JSON (maks. 250 B), np.:

```json
{"Temperature":23.5,"Humidity":45,"Pressure":1013,"Battery":87,"BatteryVoltage":4.08}
```

Pole ma wartość `null`, gdy odczyt danego czujnika się nie powiódł – na ekranie
wyświetlane jest wtedy `---`.

## Konfiguracja

W pliku `../common/packet.h`:

- `SENDER_MAC` – adres MAC nadajnika (ramki od innych urządzeń są pomijane),
- `ESPNOW_CHANNEL` – kanał Wi-Fi (musi być taki sam w nadajniku i odbiorniku).

MAC odbiornika jest wypisywany na porcie szeregowym przy starcie oraz
widoczny na ekranie informacji.

## Budowanie i wgrywanie

Wymagany [PlatformIO](https://platformio.org/). Biblioteki (pobierane
automatycznie): `ArduinoJson ^7.2.0`, `LovyanGFX ^1.2.7`.

```bash
cp src/wifi_secrets.example.h src/wifi_secrets.h   # raz, potem wpisz SSID i hasło
pio run                  # kompilacja
pio run -t upload        # wgranie
pio device monitor       # monitor portu szeregowego (115200 bd)
```

## Obsługa

| Akcja                                        | Efekt                                 |
|----------------------------------------------|---------------------------------------|
| Przytrzymanie ekranu przy pierwszym napisie (1,5 s) | Ponowna kalibracja dotyku      |
| Przycisk na dolnym pasku                     | Przełączenie ekranu: *Odczyty*, *Temp.*, *Wilg.*, *Cisn.*, *Info* |
| Przeciąganie po suwaku (ekran *Info*)        | Zmiana bazowego poziomu jasności      |
| Przycisk *Auto* (ekran *Info*)               | Włączenie/wyłączenie jasności automatycznej |
| `h` w monitorze portu szeregowego            | Wypisanie historii (CSV)              |
