#include "history.h"
#include "timesync.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

// Plik historii: nagłówek + HISTORY_SIZE slotów po 16 B (ok. 47 KB),
// tworzony od razu w pełnym rozmiarze. Nowy wpis trafia do slotu "head",
// potem zapisywany jest nagłówek - zanik zasilania między tymi zapisami
// gubi najwyżej bieżący odczyt. LittleFS sam rozkłada zużycie flash.
static const char *PATH = "/history.bin";
static constexpr uint32_t MAGIC = 0x54534948; // "HIST"
static constexpr uint16_t HISTORY_VERSION = 1;

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t recSize;
  uint16_t capacity;
  uint16_t head;  // slot, do którego trafi następny wpis
  uint16_t count; // liczba zapisanych wpisów
  uint16_t boot;  // licznik uruchomień
};

static File file;
static Header hdr;
static bool ready = false;

static size_t slotOffset(uint16_t slot) {
  return sizeof(Header) + (size_t)slot * sizeof(HistoryEntry);
}

static bool writeHeader() {
  bool ok = file.seek(0) && file.write((const uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr);
  file.flush();
  return ok;
}

static bool readSlot(uint16_t slot, HistoryEntry &e) {
  return file.seek(slotOffset(slot)) &&
         file.read((uint8_t *)&e, sizeof(e)) == sizeof(e);
}

// Tworzy nowy plik i zapisuje w nim ostatnie (najnowsze) wpisy z "entries"
// (posortowanych od najstarszego).
static bool createFile(const HistoryEntry *entries, size_t n, uint16_t boot) {
  if (file)
    file.close();
  size_t keep = n > HISTORY_SIZE ? HISTORY_SIZE : n;
  hdr = {MAGIC, HISTORY_VERSION, sizeof(HistoryEntry), HISTORY_SIZE,
         (uint16_t)(keep % HISTORY_SIZE), (uint16_t)keep, boot};

  File f = LittleFS.open(PATH, "w");
  if (!f)
    return false;
  bool ok = f.write((const uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr);
  if (keep)
    ok &= f.write((const uint8_t *)(entries + n - keep), keep * sizeof(HistoryEntry)) ==
          keep * sizeof(HistoryEntry);
  static const uint8_t zeros[512] = {};
  size_t rest = (HISTORY_SIZE - keep) * sizeof(HistoryEntry);
  while (ok && rest) {
    size_t chunk = rest < sizeof(zeros) ? rest : sizeof(zeros);
    ok = f.write(zeros, chunk) == chunk;
    rest -= chunk;
  }
  f.close();
  if (!ok) {
    LittleFS.remove(PATH);
    return false;
  }
  file = LittleFS.open(PATH, "r+");
  return (bool)file;
}

// Dawna historia z NVS (wersja 2, 96 wpisów) - jednorazowe przeniesienie.
struct NvsEntryV2 {
  uint32_t time, seq, boot, uptime;
  int16_t t10;
  uint16_t p, batMv;
  uint8_t h, bat;
  int8_t rssi;
  uint8_t flags;
};

static bool sameReading(const HistoryEntry &a, const HistoryEntry &b) {
  return a.t10 == b.t10 && a.p == b.p && a.batMv == b.batMv && a.h == b.h &&
         a.bat == b.bat && a.rssi == b.rssi &&
         (a.flags & ~HF_UPTIME) == (b.flags & ~HF_UPTIME);
}

static size_t migrateFromNvs(HistoryEntry *out, size_t maxN, uint16_t &boot) {
  Preferences prefs;
  if (!prefs.begin("history", true)) // brak przestrzeni = nic do przeniesienia
    return 0;
  size_t n = 0;
  if (prefs.getUChar("ver", 0) == 2) {
    uint16_t head = prefs.getUShort("head", 0) % 96;
    uint16_t count = min<uint16_t>(prefs.getUShort("count", 0), 96);
    boot = (uint16_t)prefs.getUInt("boot", 0);
    for (uint16_t i = 0; i < count && n < maxN; i++) {
      char key[4];
      snprintf(key, sizeof(key), "e%02u", (head + 96 - count + i) % 96);
      NvsEntryV2 o;
      if (prefs.getBytes(key, &o, sizeof(o)) != sizeof(o))
        continue;
      HistoryEntry e = {};
      e.time = o.time ? o.time : o.uptime;
      e.flags = (o.flags & 0x1F) | (o.time ? 0 : HF_UPTIME);
      e.t10 = o.t10;
      e.p = o.p;
      e.batMv = o.batMv;
      e.boot = (uint16_t)o.boot;
      e.h = o.h;
      e.bat = o.bat;
      e.rssi = o.rssi;
      // Pomijamy duplikat zapisany przez starą wersję przy restarcie
      // (ten sam odczyt ponownie, w ciągu minuty)
      if (n && sameReading(out[n - 1], e) && e.time - out[n - 1].time < 60)
        continue;
      out[n++] = e;
    }
  }
  prefs.end();
  prefs.begin("history", false);
  prefs.clear(); // zwalnia miejsce w NVS
  prefs.end();
  Serial.printf("[HIST] Przeniesiono %u odczytow z NVS\n", (unsigned)n);
  return n;
}

// Wczytuje wszystkie wpisy istniejącego pliku (od najstarszego) - do
// przebudowy po zmianie HISTORY_SIZE.
static size_t loadAll(HistoryEntry *out, size_t maxN) {
  size_t n = 0;
  for (uint16_t i = 0; i < hdr.count && n < maxN; i++) {
    uint16_t slot = (hdr.head + hdr.capacity - hdr.count + i) % hdr.capacity;
    if (readSlot(slot, out[n]))
      n++;
  }
  return n;
}

bool historyBegin() {
  uint32_t t0 = millis();
  if (!LittleFS.begin(true)) { // true = sformatuj partycję przy pierwszym użyciu
    Serial.println("[HIST] Blad montowania LittleFS");
    return false;
  }

  bool valid = false;
  file = LittleFS.open(PATH, "r+");
  if (file) {
    valid = file.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr) &&
            hdr.magic == MAGIC && hdr.version == HISTORY_VERSION &&
            hdr.recSize == sizeof(HistoryEntry) && hdr.capacity > 0 &&
            hdr.count <= hdr.capacity && hdr.head < hdr.capacity;
    if (!valid)
      Serial.println("[HIST] Nieprawidlowy plik historii - tworzenie od nowa");
  }

  if (!valid || hdr.capacity != HISTORY_SIZE) {
    // Bufor tymczasowy na wpisy przenoszone do nowego pliku (PSRAM, jeśli jest)
    size_t maxN = valid ? max<size_t>(hdr.count, HISTORY_SIZE) : HISTORY_SIZE;
    HistoryEntry *buf = (HistoryEntry *)ps_malloc(maxN * sizeof(HistoryEntry));
    if (!buf)
      buf = (HistoryEntry *)malloc(maxN * sizeof(HistoryEntry));
    size_t n = 0;
    uint16_t boot = 0;
    if (buf && valid) {
      Serial.printf("[HIST] Zmiana rozmiaru historii %u -> %u\n", hdr.capacity,
                    HISTORY_SIZE);
      n = loadAll(buf, maxN);
      boot = hdr.boot;
    } else if (buf && !file) {
      n = migrateFromNvs(buf, maxN, boot);
    }
    bool ok = createFile(buf, n, boot);
    free(buf);
    if (!ok) {
      Serial.println("[HIST] Nie mozna utworzyc pliku historii");
      return false;
    }
  }

  hdr.boot++;
  writeHeader();
  ready = true;
  Serial.printf("[HIST] Otwarcie historii: %lu ms\n", (unsigned long)(millis() - t0));

  Serial.printf("[HIST] Uruchomienie nr %u, zapisanych odczytow: %u/%u\n",
                hdr.boot, hdr.count, HISTORY_SIZE);
  Serial.printf("[HIST] LittleFS: zajete %u z %u KB\n",
                (unsigned)(LittleFS.usedBytes() / 1024),
                (unsigned)(LittleFS.totalBytes() / 1024));
  // Bez podłączonego terminala nie ma sensu wysyłać ~3000 wierszy
  if (Serial)
    historyPrint();
  Serial.println("[HIST] Wpisz 'h' w monitorze portu, aby wypisac historie");
  return true;
}

void historyAdd(const SensorData &d) {
  if (!ready)
    return;
  HistoryEntry e = {};
  e.boot = hdr.boot;
  // Zapis następuje kilka ms po odbiorze - korekta do chwili odbioru
  time_t now = timeNow();
  if (now) {
    e.time = (uint32_t)(now - (millis() - d.rxMillis) / 1000);
  } else {
    e.time = d.rxMillis / 1000;
    e.flags |= HF_UPTIME;
  }
  if (d.hasT) {
    e.flags |= HF_T;
    e.t10 = (int16_t)lroundf(d.t * 10);
  }
  if (d.hasH) {
    e.flags |= HF_H;
    e.h = (uint8_t)constrain(d.h, 0, 255);
  }
  if (d.hasP) {
    e.flags |= HF_P;
    e.p = (uint16_t)constrain(d.p, 0, 65535);
  }
  if (d.hasBat) {
    e.flags |= HF_BAT;
    e.bat = (uint8_t)constrain(d.bat, 0, 255);
    e.batMv = (uint16_t)lroundf(d.batV * 1000);
  }
  if (d.hasRssi) {
    e.flags |= HF_RSSI;
    e.rssi = (int8_t)constrain(d.rssi, -128, 127);
  }

  if (!file.seek(slotOffset(hdr.head)) ||
      file.write((const uint8_t *)&e, sizeof(e)) != sizeof(e)) {
    Serial.println("[HIST] Blad zapisu do pliku historii");
    return;
  }
  hdr.head = (hdr.head + 1) % HISTORY_SIZE;
  if (hdr.count < HISTORY_SIZE)
    hdr.count++;
  if (!writeHeader())
    Serial.println("[HIST] Blad zapisu naglowka historii");

  char ts[24] = "";
  if (!(e.flags & HF_UPTIME))
    timeFormat(e.time, ts, sizeof(ts));
  Serial.printf("[HIST] Zapisano odczyt %s(%u/%u)\n", ts[0] ? strcat(ts, " ") : "",
                hdr.count, HISTORY_SIZE);
}

void historyLoop() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'h' || c == 'H')
      historyPrint();
  }
}

uint16_t historyCount() { return ready ? hdr.count : 0; }

bool historyGet(uint16_t i, HistoryEntry &out) {
  if (!ready || i >= hdr.count)
    return false;
  return readSlot((hdr.head + HISTORY_SIZE - hdr.count + i) % HISTORY_SIZE, out);
}

void historyPrint() {
  Serial.printf("[HIST] Historia: %u odczytow\n", historyCount());
  if (historyCount() == 0)
    return;
  Serial.println("time,boot,uptime_s,temperature_c,humidity_pct,pressure_hpa,"
                 "battery_pct,battery_v,rssi_dbm");
  HistoryEntry e;
  char ts[24];
  for (uint16_t i = 0; i < historyCount(); i++) {
    if (!historyGet(i, e))
      continue;
    // Puste pole = brak odczytu danego czujnika / brak czasu.
    // Bez zsynchronizowanego zegara zamiast czasu jest uptime_s.
    if (e.flags & HF_UPTIME)
      Serial.printf(",%u,%lu,", e.boot, (unsigned long)e.time);
    else {
      timeFormat(e.time, ts, sizeof(ts));
      Serial.printf("%s,%u,,", ts, e.boot);
    }
    if (e.flags & HF_T)
      Serial.printf("%.1f", e.t10 / 10.0f);
    Serial.print(',');
    if (e.flags & HF_H)
      Serial.print(e.h);
    Serial.print(',');
    if (e.flags & HF_P)
      Serial.print(e.p);
    Serial.print(',');
    if (e.flags & HF_BAT)
      Serial.printf("%u,%.2f", e.bat, e.batMv / 1000.0f);
    else
      Serial.print(',');
    Serial.print(',');
    if (e.flags & HF_RSSI)
      Serial.print(e.rssi);
    Serial.println();
  }
}
