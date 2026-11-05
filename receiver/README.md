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
  przez 60 s).
- Stopka: czas od ostatniego odczytu, licznik pakietów, RSSI.
- Ekran informacji (dotknięcie ekranu przełącza widok): MAC odbiornika
  i nadajnika, kanał ESP-NOW, liczba pakietów, RSSI, czas pracy, wolna pamięć.
- Suwak jasności podświetlenia na ekranie informacji – wartość zapisywana
  w NVS.
- Kalibracja dotyku przy pierwszym uruchomieniu (zapisywana w NVS);
  ponowną kalibrację wymusza przytrzymanie ekranu podczas ekranu startowego.

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
pio run                  # kompilacja
pio run -t upload        # wgranie
pio device monitor       # monitor portu szeregowego (115200 bd)
```

## Obsługa

| Akcja                                        | Efekt                                 |
|----------------------------------------------|---------------------------------------|
| Przytrzymanie ekranu podczas ekranu startowego | Ponowna kalibracja dotyku           |
| Dotknięcie ekranu                            | Przełączenie: ekran główny ↔ informacje |
| Przeciąganie po suwaku (ekran informacji)    | Zmiana jasności podświetlenia         |
