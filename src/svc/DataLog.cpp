#include "svc/DataLog.h"
#include <LittleFS.h>

String DataLog::dayPath(uint32_t epochDay) { return String("/log/d") + epochDay + ".bin"; }

void DataLog::begin() {
  if (!LittleFS.exists("/log")) LittleFS.mkdir("/log");
}

bool DataLog::append(const Sample& s) {
  File f = LittleFS.open(dayPath(s.epoch / 86400), "a");
  if (!f) return false;
  size_t n = f.write((const uint8_t*)&s, sizeof(s));
  f.close();
  return n == sizeof(s);
  // NB (residuo noto): f.write bufferizza e close() scarta l'esito del flush finale, quindi a FS
  // PIENA all'ultimo blocco questo append potrebbe ritornare true pur perdendo il record. Non lo si
  // verifica qui (una ri-apertura per campione aggiungerebbe pressione di heap e falsi allarmi su
  // OOM). Mitigato a monte: il tetto di dimensione in cleanup() impedisce che il LOG riempia la FS,
  // e un FS pieno per altre cause fa comunque fallire i save di config/stato (fsCommitAtomic) →
  // l'allarme storage scatta da lì.
}

void DataLog::cleanup(uint16_t retentionDays, uint32_t nowEpoch) {
  // 1) Retention per GIORNI (efficace solo con orologio affidabile).
  uint32_t minDay = nowEpoch / 86400 >= retentionDays ? nowEpoch / 86400 - retentionDays : 0;
  {
    Dir dir = LittleFS.openDir("/log");
    while (dir.next()) {
      yield();  // unica passata che ne era priva: con molti file va data aria al loop
      String name = dir.fileName();  // "dNNNN.bin"
      if (!name.startsWith("d") || !name.endsWith(".bin")) continue;
      uint32_t day = (uint32_t)name.substring(1, name.length() - 4).toInt();
      if (day < minDay) LittleFS.remove(String("/log/") + name);
    }
  }
  // 2) Tetto ASSOLUTO sulla dimensione totale, indipendente da giorni e orologio. Necessario perché:
  //    (a) campionamento fitto (1 min) x retention alta può superare lo spazio della FS; e
  //    (b) con orologio inaffidabile i campioni cadono in /log/d0,d1,... e la retention-per-giorni
  //        non rimuove nulla (minDay=0) → crescita illimitata. Qui si cancella il file del giorno
  //        PIÙ VECCHIO finché il totale non rientra nel budget, così il log non riempie mai la FS.
  //    Il guard limita le iterazioni (mai un ciclo infinito se un remove fallisce).
  for (uint16_t guard = 0; guard < 400; guard++) {
    yield();  // scansioni ripetute della directory: il loop di controllo non deve morire
    uint32_t total = 0, oldest = 0xFFFFFFFFUL;
    bool any = false;
    {
      Dir dir = LittleFS.openDir("/log");
      while (dir.next()) {
        String name = dir.fileName();
        if (!name.startsWith("d") || !name.endsWith(".bin")) continue;
        total += (uint32_t)dir.fileSize();
        uint32_t day = (uint32_t)name.substring(1, name.length() - 4).toInt();
        if (day < oldest) oldest = day;
        any = true;
      }
    }
    if (!any || total <= kMaxTotalBytes) break;
    // Si cancella UN file del giorno piu' vecchio, scegliendolo per NOME fra quelli
    // realmente presenti invece di ricostruirne il percorso.
    //
    // Ricostruirlo era un difetto reale: l'insieme CONTATO sopra accetta qualsiasi
    // /log/d*.bin — compresi i d<giorno>_1.bin lasciati dal firmware a due camere —
    // mentre il percorso ricostruito copriva solo d<giorno>.bin. Con un orfano oldest
    // restava fermo sul suo giorno, la remove falliva e il ciclo usciva: da quel momento
    // il tetto di dimensione non liberava PIU' NULLA e il registro cresceva fino a
    // riempire la LittleFS, facendo poi fallire anche i salvataggi di config e stato.
    // Scegliendo per nome i due insiemi coincidono per costruzione.
    String vittima;
    {
      Dir dir = LittleFS.openDir("/log");
      while (dir.next()) {
        String name = dir.fileName();
        if (!name.startsWith("d") || !name.endsWith(".bin")) continue;
        if ((uint32_t)name.substring(1, name.length() - 4).toInt() != oldest) continue;
        vittima = name;
        break;
      }
    }
    if (!vittima.length()) break;                               // nulla da cancellare
    if (!LittleFS.remove(String("/log/") + vittima)) break;     // non si riesce a liberare
  }
}

void DataLog::forEach(uint32_t fromEpoch, uint32_t toEpoch,
                      const std::function<bool(const Sample&)>& cb) {
  uint32_t fromDay = fromEpoch / 86400;
  uint32_t toDay = toEpoch / 86400;
  if (toDay - fromDay > 400) fromDay = toDay - 400;  // mai iterare epoche assurde
  Sample s;
  for (uint32_t day = fromDay; day <= toDay; day++) {
    yield();  // anche sui giorni senza file: il loop di controllo non deve morire
    File f = LittleFS.open(dayPath(day), "r");
    if (!f) continue;
    while (f.read((uint8_t*)&s, sizeof(s)) == sizeof(s)) {
      if (s.epoch < fromEpoch || s.epoch > toEpoch) continue;
      if (!cb(s)) {
        f.close();
        return;
      }
    }
    f.close();
  }
}

void DataLog::cursorBegin(Cursor& c, uint32_t fromEpoch, uint32_t toEpoch) {
  c.fromEpoch = fromEpoch;
  c.toEpoch = toEpoch;
  c.day = fromEpoch / 86400;
  c.toDay = toEpoch / 86400;
  c.offset = 0;
  c.done = fromEpoch > toEpoch;
  if (!c.done && c.toDay - c.day > 400) c.day = c.toDay - 400;  // come forEach
}

uint16_t DataLog::cursorRead(Cursor& c, uint16_t maxOut, uint16_t maxScan,
                             const std::function<void(const Sample&)>& cb) {
  uint16_t out = 0, scanned = 0;
  Sample s;
  while (!c.done && out < maxOut && scanned < maxScan) {
    if (c.day > c.toDay) {
      c.done = true;
      break;
    }
    File f = LittleFS.open(dayPath(c.day), "r");
    if (!f) {  // giorno senza file
      c.day++;
      c.offset = 0;
      scanned++;
      continue;
    }
    if (c.offset && !f.seek(c.offset, SeekSet)) {
      f.close();
      c.day++;
      c.offset = 0;
      continue;
    }
    bool eof = false;
    while (out < maxOut && scanned < maxScan) {
      if (f.read((uint8_t*)&s, sizeof(s)) != sizeof(s)) {
        eof = true;
        break;
      }
      c.offset += sizeof(s);
      scanned++;
      if (s.epoch < c.fromEpoch || s.epoch > c.toEpoch) continue;
      cb(s);
      out++;
    }
    f.close();
    if (eof) {
      c.day++;
      c.offset = 0;
    }
  }
  return out;
}
