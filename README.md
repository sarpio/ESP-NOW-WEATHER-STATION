# Stacja pogodowa ESP-NOW

Bezprzewodowa stacja pogodowa złożona z dwóch urządzeń ESP32 komunikujących się
przez **ESP-NOW** (bez routera i sieci Wi-Fi):

- **Nadajnik** (`sender/`) – zasilana bateryjnie płytka **XIAO ESP32S3**
  z czujnikami SHT41 (temperatura, wilgotność) i LPS22HH (ciśnienie). O pełnej
  godzinie oraz +15, +30 i +45 min mierzy dane i napięcie baterii, wysyła je
  i przechodzi w deep sleep. Zegar synchronizuje z odbiornikiem przy każdej wysyłce.
- **Odbiornik** (`receiver/`) – **ESP32-S3 N16R8** z dotykowym wyświetlaczem
  4.0" TFT (ST7796S + XPT2046). Odbiera pakiety i prezentuje odczyty na ekranie.
  W odpowiedzi na każdy pakiet odsyła nadajnikowi czas (zegar odbiornika jest
  synchronizowany z serwerem NTP przez Wi-Fi).

```
┌───────────────────────┐        ESP-NOW         ┌────────────────────────┐
│  Nadajnik             │   (kanał Wi-Fi 1,      │  Odbiornik             │
│  XIAO ESP32S3         │    JSON ≤ 250 B)       │  ESP32-S3 N16R8        │
│  SHT41 + LPS22HH      │ ─────────────────────► │  TFT 4.0" 480×320      │
│  bateria Li-Po        │  :00 :15 :30 :45       │  dotyk XPT2046         │
│                       │ ◄───────────────────── │  zegar z NTP           │
│                       │   odpowiedź: czas      │                        │
└───────────────────────┘                        └────────────────────────┘
```

## Struktura repozytorium

```
ESP-NOW/
├── common/
│   └── packet.h     # wspólne: format ramek (dane, czas), kanał ESP-NOW, MAC nadajnika, klucze JSON, strefa czasowa
├── sender/          # projekt PlatformIO nadajnika     → sender/README.md
└── receiver/        # projekt PlatformIO odbiornika    → receiver/README.md
```

Oba projekty dołączają katalog `common/` przez `build_flags = -I../common`.
Szczegóły sprzętu, połączeń i konfiguracji opisują README w poszczególnych
katalogach:

- [sender/README.md](sender/README.md) – czujniki, pomiar baterii, deep sleep, zworka trybu pracy,
- [receiver/README.md](receiver/README.md) – połączenia wyświetlacza i dotyku, interfejs, obsługa.

## Format danych

Ładunek ramki ESP-NOW to tekst JSON, np.:

```json
{"Temperature":23.5,"Humidity":45,"Pressure":1013,"Battery":87,"BatteryVoltage":4.08}
```

| Pole             | Jednostka | Opis                              |
|------------------|-----------|-----------------------------------|
| `Temperature`    | °C        | temperatura (1 miejsce po przecinku) |
| `Humidity`       | %         | wilgotność względna               |
| `Pressure`       | hPa       | ciśnienie atmosferyczne           |
| `Battery`        | %         | poziom naładowania baterii        |
| `BatteryVoltage` | V         | napięcie baterii                  |

Pole ma wartość `null`, gdy odczyt danego czujnika się nie powiódł.

W odpowiedzi odbiornik (o ile ma czas z NTP) od razu odsyła nadajnikowi
bieżący czas – czas Unix (UTC) w sekundach i część mikrosekundową:

```json
{"Time":1762700400,"Us":123456}
```

## Konfiguracja par urządzeń

Urządzenia są sparowane adresami MAC – po wymianie płytki trzeba je zaktualizować:

| Gdzie                     | Parametr         | Znaczenie                                        |
|---------------------------|------------------|--------------------------------------------------|
| `common/packet.h`         | `SENDER_MAC`     | MAC nadajnika (odbiornik odrzuca inne ramki)     |
| `common/packet.h`         | `ESPNOW_CHANNEL` | kanał Wi-Fi – identyczny po obu stronach         |
| `sender/src/main.cpp`     | `RECEIVER_MAC`   | MAC odbiornika, do którego wysyłane są dane      |

Każda płytka wypisuje swój MAC na porcie szeregowym (115200 bd) przy starcie.

## Budowanie i wgrywanie

Wymagany [PlatformIO](https://platformio.org/). Każdy projekt buduje się
osobno ze swojego katalogu:

```bash
cd sender      # lub: cd receiver
pio run                  # kompilacja
pio run -t upload        # wgranie
pio device monitor       # monitor portu szeregowego (115200 bd)
```

| Projekt    | Środowisko     | Platforma                                  | Biblioteki                       |
|------------|----------------|--------------------------------------------|----------------------------------|
| `sender`   | `xiao-esp32s3` | pioarduino (Arduino core 3.x)              | ArduinoJson ^7.2.0               |
| `receiver` | `esp32-s3`     | espressif32                                | ArduinoJson ^7.2.0, LovyanGFX ^1.2.7 |

> Nadajnik w deep sleep nie jest widoczny jako port USB – przed wgraniem
> naciśnij RESET (płytka czuwa wtedy 5 min) lub załóż zworkę D3 ↔ 3V3.
> Szczegóły w [sender/README.md](sender/README.md#jak-wyprowadzić-płytkę-z-deep-sleep).
