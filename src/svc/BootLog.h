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
  uint32_t upSec = 0;   // durata della sessione precedente, se registrata
  char reason[24] = {0};
};

class BootLog {
 public:
  static constexpr const char* kPath = "/boot.json";
  static constexpr uint8_t kMax = 8;

  uint8_t count() const { return n_; }
  const BootEntry* entry(uint8_t i) const { return i < n_ ? &e_[i] : nullptr; }
  const char* lastReason() const { return n_ ? e_[n_ - 1].reason : ""; }

  bool load();
  // Registra l'avvio corrente. epoch può essere 0: verrà riempito da stampEpoch()
  // appena l'ora diventa nota, così un riavvio prima della sincronizzazione NTP
  // non resta senza data.
  bool record(const String& reason, uint32_t epoch, uint32_t prevUpSec);
  // Assegna l'ora all'ultimo avvio se era ignota. Ritorna true se ha scritto.
  bool stampEpoch(uint32_t epoch);
  bool save() const;

 private:
  BootEntry e_[kMax];
  uint8_t n_ = 0;
};
