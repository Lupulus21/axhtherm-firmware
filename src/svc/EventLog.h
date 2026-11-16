#pragma once
#include <Arduino.h>
#include "hal/At24c32.h"
#include "ax/EventRing.h"

// Registro eventi (3.15.0): gli ultimi 256 eventi nella EEPROM AT24C32 del Tiny RTC, che fino
// alla 3.13.0 non era usata. Risponde a «cosa e' successo stanotte?» senza toccare la
// LittleFS. Se la EEPROM non risponde (vecchia scheda senza Tiny RTC) il registro resta
// spento, senza allarme.
//
// Codici scritti nella EEPROM: mai riordinarli ne' riusarli.
enum class Ev : uint8_t {
  Boot = 1,          // a = causa del reset (rst_info.reason), b = riavvio voluto (RebootIntent)
  Arm = 2,           // a = rele' del freddo, b = rele' del caldo
  Disarm = 3,
  Mode = 4,          // a = 0 fermo, 1 termostato, 2 profilo
  Step = 5,          // a = step (da 1), b = step totali
  AlarmOn = 6,       // a = AlarmId (svc/Alarms.h)
  AlarmOff = 7,      // a = AlarmId
  HeatLock = 8,      // a = motivo (ax::HeatLock)
  HeatUnlock = 9,
  ProfileDone = 10,
  Pause = 11,
  Resume = 12,
  CrashReached = 13,
  Config = 14,       // configurazione salvata
  Ota = 15,          // aggiornamento accettato, segue il riavvio
  ClockJump = 16,    // v = salto in minuti (saturato a ±32767)
  CmdRejected = 17,  // a = comando (fase 12)
  FermDone = 18,     // fermentazione finita (fase 12)
  FermStuck = 19,    // fermentazione ferma? (fase 12)
  Confirm = 20,      // step confermato dall'utente (fase 12)
};

class EventLog {
 public:
  bool begin(uint8_t addr);
  bool on() const { return ring_.ok(); }
  bool add(uint32_t epoch, Ev code, uint8_t a = 0, uint16_t b = 0, int16_t v = 0);
  uint32_t lastSeq() const { return ring_.lastSeq(); }
  bool read(uint32_t seq, ax::EventRec& out) { return ring_.readSeq(seq, out); }
  uint16_t failures() const { return fails_; }

  static const __FlashStringHelper* name(uint8_t code);

 private:
  At24c32 ee_;
  ax::EventRing<At24c32> ring_;
  uint16_t fails_ = 0;
};
