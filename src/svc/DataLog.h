#pragma once
#include <Arduino.h>
#include <functional>

// Log storico su LittleFS: record binari da 16 byte in file giornalieri /log/d<epochday>.bin.
// Append-only (wear-friendly); la retention cancella i file più vecchi.
//
// Il firmware a due camere (3.0.0) scriveva anche /log/d<giorno>_1.bin: sul dispositivo
// possono restare file così. cleanup() li vede (filtra per `startsWith("d")` e ricava il
// giorno con toInt(), che si ferma al primo carattere non numerico) e li smaltisce come gli
// altri, per età e per tetto di dimensione.
class DataLog {
 public:
  struct __attribute__((packed)) Sample {
    uint32_t epoch;
    int16_t tControl;  // °C x100; INT16_MIN = mancante
    int16_t tAux1;
    int16_t tAux2;
    int16_t setpoint;
    uint8_t flags;      // bit0 heat, bit1 cool, bit2 armed, bit3 allarme, bit4 setpoint valido
    uint16_t gravity;   // SG x10000 (o °P x100); 0 = mancante
    uint8_t battery;    // V x20; 0 = mancante
  };
  static_assert(sizeof(Sample) == 16, "record del log = 16 byte");

  static constexpr int16_t kMissing = INT16_MIN;
  // Tetto ASSOLUTO sulla dimensione totale del log su flash, indipendente da retention-per-giorni
  // e dalla validità dell'orologio: garantisce che il log non riempia mai la LittleFS (~1,95 MB
  // usabili) affamando i salvataggi di config/stato/profilo. ~0,5 MB restano per tutto il resto.
  static constexpr uint32_t kMaxTotalBytes = 1400000;

  void begin();
  bool append(const Sample& s);
  void cleanup(uint16_t retentionDays, uint32_t nowEpoch);
  static String dayPath(uint32_t epochDay);

  // Itera i campioni con epoch in [fromEpoch, toEpoch]; cb ritorna false per fermarsi.
  void forEach(uint32_t fromEpoch, uint32_t toEpoch,
               const std::function<bool(const Sample&)>& cb);

  // Lettura a pezzi (3.14.0, export CSV che non ferma la regolazione): giorno e posizione
  // nel file restano nel cursore fra una chiamata e l'altra, quindi ogni pezzo riparte da
  // dove si era fermato invece di rileggere il giorno dall'inizio.
  struct Cursor {
    uint32_t fromEpoch = 0, toEpoch = 0;
    uint32_t day = 0, toDay = 0, offset = 0;
    bool done = true;
  };
  static void cursorBegin(Cursor& c, uint32_t fromEpoch, uint32_t toEpoch);
  // Passa a cb al massimo maxOut campioni dell'intervallo, scorrendone al massimo maxScan
  // (cosi' anche un tratto senza campioni utili costa un tempo limitato). Ritorna quanti
  // ne ha passati; c.done diventa true a fine intervallo.
  uint16_t cursorRead(Cursor& c, uint16_t maxOut, uint16_t maxScan,
                      const std::function<void(const Sample&)>& cb);

  static int16_t encodeTemp(float c, bool valid) {
    return valid ? (int16_t)lroundf(c * 100.0f) : kMissing;
  }
};
