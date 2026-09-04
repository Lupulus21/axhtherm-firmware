#pragma once
#include <Arduino.h>
#include <functional>
#include "ax_dimensions.h"

// Log storico su LittleFS: record binari da 16 byte in file giornalieri.
// Append-only (wear-friendly); la retention cancella i file più vecchi.
//
// UN FILE PER CAMERA E PER GIORNO. La camera 0 usa /log/d<giorno>.bin — lo stesso
// percorso della v2.23.2, quindi lo storico esistente resta leggibile senza alcuna
// conversione — e le altre /log/d<giorno>_<camera>.bin. Il suffisso, invece di un
// prefisso diverso, è deliberato: cleanup() filtra i file per `startsWith("d")` e ne
// ricava il giorno con toInt(), che si ferma al primo carattere non numerico. Così i
// file delle altre camere restano VISIBILI alla retention. Con un prefisso diverso
// sarebbero stati invisibili e sarebbero cresciuti senza limite.
//
// Il record resta di 16 byte, senza campo camera: l'identità sta nel nome del file.
// Estendere il record avrebbe invece invalidato tutti gli storici già scritti.
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
  bool append(const Sample& s, uint8_t chamber);
  // Il tetto di dimensione vale sul TOTALE di tutte le camere: con più camere il log
  // cresce proporzionalmente e il budget di flash è uno solo.
  void cleanup(uint16_t retentionDays, uint32_t nowEpoch);
  static String dayPath(uint32_t epochDay, uint8_t chamber);

  // Itera i campioni con epoch in [fromEpoch, toEpoch]; cb ritorna false per fermarsi.
  void forEach(uint32_t fromEpoch, uint32_t toEpoch, uint8_t chamber,
               const std::function<bool(const Sample&)>& cb);

  static int16_t encodeTemp(float c, bool valid) {
    return valid ? (int16_t)lroundf(c * 100.0f) : kMissing;
  }
};
