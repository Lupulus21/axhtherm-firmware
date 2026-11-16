#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Registro degli avvii. Serve a rispondere a una domanda che altrimenti resta senza
// risposta: "perché il termostato si è riavviato stanotte?". Senza questo, un riavvio
// non sorvegliato lascia solo un buco nel registro delle temperature, indistinguibile
// da un blackout, da un crash o da un aggiornamento.
//
// Una riga per avvio, otto avvii conservati: la scrittura avviene una sola volta per
// accensione, quindi l'usura della flash è irrilevante.
struct BootEntry {
  uint32_t epoch = 0;   // 0 se l'ora non era ancora nota all'avvio
  uint32_t upSec = 0;   // durata della sessione PRECEDENTE (0 = ignota, es. dopo un blackout)
  uint32_t epc = 0;     // indirizzo dell'eccezione, se il riavvio e' stato un blocco
  uint8_t intent = 0;   // RebootIntent: riavvio voluto, e da chi
  uint8_t exc = 0;      // causa dell'eccezione (codice Xtensa)
  char reason[24] = {0};
};

class BootLog {
 public:
  static constexpr const char* kPath = "/boot.json";
  static constexpr uint8_t kMax = 8;

  uint8_t count() const { return n_; }
  uint32_t total() const { return total_; }  // avvii registrati da sempre (non solo gli ultimi 8)
  const BootEntry* entry(uint8_t i) const { return i < n_ ? &e_[i] : nullptr; }
  const char* lastReason() const { return n_ ? e_[n_ - 1].reason : ""; }

  bool load();
  // Registra l'avvio corrente. epoch può essere 0: verrà riempito da stampEpoch()
  // appena l'ora diventa nota, così un riavvio prima della sincronizzazione NTP
  // non resta senza data.
  bool record(const String& reason, uint32_t epoch, uint32_t prevUpSec, uint8_t intent = 0,
              uint8_t exc = 0, uint32_t epc = 0);
  // Assegna l'ora all'ultimo avvio se era ignota. Ritorna true se ha scritto.
  bool stampEpoch(uint32_t epoch);
  bool save() const;

 private:
  BootEntry e_[kMax];
  uint8_t n_ = 0;
  uint32_t total_ = 0;
};
