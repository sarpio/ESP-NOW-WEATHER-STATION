# Nadajnik ESP-NOW – stacja pomiarowa (XIAO ESP32S3)

Nadajnik o pełnych kwadransach zegara (`:00`, `:15`, `:30`, `:45`) mierzy
temperaturę, wilgotność, ciśnienie i napięcie baterii, a następnie wysyła wynik
jako JSON przez ESP-NOW do odbiornika (`../receiver`). W odpowiedzi odbiornik
odsyła czas z NTP, według którego nadajnik synchronizuje swój zegar.
Między pomiarami płytka może przechodzić w deep sleep, aby oszczędzać baterię.

## Sekwencja po podaniu zasilania

1. Wysyłka pierwszych odczytów – od razu po starcie. Odbiornik odpowiada
   bieżącym czasem (z NTP) i nadajnik ustawia według niego swój zegar.
2. Czuwanie przez 5 min (`STARTUP_AWAKE_MS`) – kwadranse wypadające w tym
   czasie są obsługiwane bez usypiania.
3. Deep sleep do najbliższego pełnego kwadransa (`:00`, `:15`, `:30`, `:45`).
4. Wybudzenie timerem, wysyłka odczytu (z ponowną synchronizacją zegara)
   i deep sleep do kolejnego kwadransa.
5. Krok 4 powtarza się w nieskończoność.

Wysyłki wypadają więc o stałych porach zegara, niezależnie od chwili włączenia.
Dotyczy trybu z deep sleep (zworka D3 niezałożona). Przy zworce D3 ↔ 3V3 kroki 1–2
są takie same, a dalej płytka nie usypia i wysyła o pełnych kwadransach.

Jeśli odbiornik nie odeśle czasu (np. sam nie ma jeszcze czasu z NTP), nadajnik
działa jak dawniej: druga wysyłka po 5 min, a dalej co 15 min (`SEND_INTERVAL_MS`)
liczone od poprzedniej. Na wyrównanie do zegara przechodzi przy pierwszej
odpowiedzi z czasem.

## Synchronizacja zegara

- Odbiornik synchronizuje swój zegar z serwerem NTP (Wi-Fi), a w odpowiedzi na
  każdą ramkę z danymi odsyła przez ESP-NOW ramkę `{"Time":<s>,"Us":<µs>}`.
- Nadajnik po potwierdzeniu ramki czeka na czas maks. `TIME_REPLY_TIMEOUT_MS`
  i ustawia zegar (`settimeofday`). Zegar przetrwa deep sleep; po odłączeniu
  zasilania jest kasowany i odtwarzany przy wysyłce startowej.
- Deep sleep odmierza niedokładny wewnętrzny generator RC (błąd rzędu ułamka
  procenta, czyli kilku sekund na 15 min). Nadajnik mierzy ten błąd przy każdej
  synchronizacji (`rtcDrift`, w pamięci RTC) i koryguje o niego czas snu oraz
  zegar po wybudzeniu. Monitor pokazuje korektę i bieżący błąd RTC.
- Budzik ustawiany jest `WAKE_ADVANCE_MS` przed kwadransem (czas startu płytki),
  resztę program odczekuje, więc pomiar wychodzi o pełnym kwadransie.

## Sprzęt

| Element            | Podłączenie (XIAO ESP32S3)         | Uwagi                                  |
|--------------------|------------------------------------|----------------------------------------|
| SHT41 (temp./wilg.)| I2C: SDA = D4, SCL = D5            | adres `0x44`                           |
| LPS22HH (ciśnienie)| I2C: SDA = D4, SCL = D5            | adres `0x5D`                           |
| Pomiar baterii     | A0 (D0)                            | dzielnik 2 × 200 kΩ z BAT+ do A0       |
| Zworka trybu pracy | D3 ↔ 3V3                           | wewnętrzny pull-down, patrz niżej      |

## Działanie

1. **Start**: inicjalizacja I2C, czujników, Wi-Fi (kanał `ESPNOW_CHANNEL`, moc TX 8.5 dBm)
   i ESP-NOW. Na Serial (115200) wypisywany jest MAC płytki oraz stan trybu deep sleep.
   Jeśli MAC różni się od `SENDER_MAC` z `common/packet.h`, pojawia się ostrzeżenie –
   odbiornik odrzuca ramki od nieznanych nadawców.
2. **Zimny start** (włączenie zasilania / RESET): jednorazowa wysyłka od razu
   (`Wysylka startowa`), bez usypiania po niej – przy okazji synchronizuje zegar.
   Następnie płytka czuwa `STARTUP_AWAKE_MS` (domyślnie 5 min), co
   `STARTUP_LOG_INTERVAL_MS` wypisując czas do kolejnej wysyłki. To okno służy do
   wgrania firmware i podglądu monitora. Po wybudzeniu z deep sleep wysyłka
   następuje o kwadransie, na który ustawiono budzik (bez wysyłki startowej
   i bez okna czuwania).
3. **Pomiar i wysyłka**: odczyt SHT41, LPS22HH (tryb one-shot) i napięcia baterii
   (średnia z 16 próbek), zbudowanie JSON i wysłanie do `RECEIVER_MAC`. Program czeka
   na potwierdzenie maks. `SEND_CB_TIMEOUT_MS`, a potem na czas od odbiornika
   (patrz [Synchronizacja zegara](#synchronizacja-zegara)). Przykładowa ramka:

   ```json
   {"Temperature":23.5,"Humidity":45,"Pressure":1013,"Battery":87,"BatteryVoltage":4.08}
   ```

   Pole ma wartość `null`, gdy odczyt się nie powiódł (lub napięcie baterii
   < `BATTERY_MIN_VALID_V`, czyli bateria/dzielnik niepodłączone).
4. **Po wysyłce**:
   - deep sleep włączony – płytka zasypia do kolejnego pełnego kwadransa
     (poza oknem czuwania po zimnym starcie), budzi ją timer;
   - deep sleep wyłączony – płytka pracuje dalej i wysyła o pełnych kwadransach.
5. **Błąd inicjalizacji ESP-NOW**: ponowna próba przy najbliższym kwadransie
   (bez zegara – po `SEND_INTERVAL_MS`), przez deep sleep albo opóźnienie + restart.

Licznik ramek `seq` jest w pamięci RTC – przetrwa deep sleep, zeruje się po
odłączeniu zasilania.

## Tryb pracy – zworka D3

| Stan D3                    | Tryb                                         |
|----------------------------|----------------------------------------------|
| niepodłączony lub GND      | deep sleep między wysyłkami (praca bateryjna)|
| 3.3V (zworka D3 ↔ 3V3)     | praca ciągła, bez usypiania                  |

Stan pinu jest sprawdzany przy każdej decyzji o uśpieniu, więc zmiana nie wymaga
restartu – zadziała przy najbliższej wysyłce (lub po wybudzeniu). Pin można zmienić
stałą `DEEP_SLEEP_DISABLE_PIN`; unikaj pinów strappingowych (GPIO0, 3, 45, 46).

## Parametry konfigurowalne

W `src/main.cpp`:

| Parametr                  | Domyślnie        | Opis                                                        |
|---------------------------|------------------|-------------------------------------------------------------|
| `RECEIVER_MAC`            | `80:B5:4E:FA:C3:14` | MAC odbiornika                                           |
| `SEND_INTERVAL_MS`        | 15 min           | okres wysyłek – wyrównanych do zegara (pełne kwadranse), a bez zegara liczonych od poprzedniej wysyłki |
| `WAKE_ADVANCE_MS`         | 2 s              | o ile wcześniej budzić się przed kwadransem (czas startu)   |
| `TIME_REPLY_TIMEOUT_MS`   | 300 ms           | maks. czas oczekiwania na czas od odbiornika                |
| `DEEP_SLEEP_DISABLE_PIN`  | `D3`             | pin zworki wyłączającej deep sleep (HIGH = brak usypiania)  |
| `STARTUP_AWAKE_MS`        | 5 min            | czas czuwania (bez deep sleep) po zimnym starcie            |
| `STARTUP_LOG_INTERVAL_MS` | 30 s             | co ile wypisywać w oknie czuwania czas do kolejnej wysyłki  |
| `SEND_CB_TIMEOUT_MS`      | 200 ms           | maks. czas oczekiwania na potwierdzenie ESP-NOW             |
| `BATTERY_PIN`             | `A0`             | wejście pomiaru baterii                                     |
| `BATTERY_DIVIDER`         | 2.0              | przelicznik dzielnika napięcia                              |
| `BATTERY_MIN_VALID_V`     | 2.5 V            | poniżej – bateria uznana za niepodłączoną (`null`)          |
| `SHT41_ADDR`, `LPS22HH_ADDR` | `0x44`, `0x5D` | adresy I2C czujników                                     |
| `WiFi.setTxPower(...)`    | 8.5 dBm          | moc nadawania (w `setup()`)                                 |

Krzywa napięcie → % baterii jest w `batteryPercent()` (ogniwo Li-Po/Li-Ion).

W `common/packet.h` (wspólne z odbiornikiem – zmieniaj po obu stronach):

| Parametr          | Opis                                        |
|-------------------|---------------------------------------------|
| `ESPNOW_CHANNEL`  | kanał Wi-Fi (musi być identyczny po obu stronach) |
| `SENDER_MAC`      | MAC nadajnika akceptowany przez odbiornik   |
| `PACKET_MAX_LEN`  | maks. rozmiar ładunku (limit ESP-NOW: 250 B)|
| `KEY_*`           | nazwy kluczy JSON                           |
| `TIME_ZONE`       | strefa czasowa (tylko do wypisywania czasu) |

## Jak wyprowadzić płytkę z deep sleep

W deep sleep port USB znika z systemu, więc nie da się wgrać firmware ani otworzyć
monitora. Płytkę budzi tylko timer. Sposoby:

1. **RESET (zalecane)** – krótko naciśnij przycisk RESET (R). Następuje zimny start
   – płytka wysyła odczyt i czuwa `STARTUP_AWAKE_MS` (5 min) – w tym czasie
   port USB jest dostępny, można wgrać firmware (`pio run -t upload`) lub otworzyć
   monitor (`pio device monitor`).
2. **Zworka D3 ↔ 3V3** – załóż zworkę i naciśnij RESET (albo poczekaj na najbliższe
   wybudzenie timerem). Płytka przestanie usypiać na stałe, dopóki zworka jest założona.
   Wygodne przy dłuższym debugowaniu.
3. **Tryb bootloadera** – gdy firmware nie odpowiada lub wgrywanie się nie udaje:
   przytrzymaj BOOT (B), naciśnij i puść RESET, puść BOOT. Płytka czeka w bootloaderze
   na wgranie programu (po wgraniu naciśnij RESET).

Uwaga: wgrywaj przy płytce podłączonej bezpośrednio do komputera – przez zewnętrzny
hub USB pojawiają się błędy (timeout zapisu / brak portu).
