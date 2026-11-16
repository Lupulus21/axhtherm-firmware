#include "svc/EventLog.h"

bool EventLog::begin(uint8_t addr) {
  if (!ee_.begin(addr)) return false;
  const uint32_t t0 = millis();
  const bool ok = ring_.begin(&ee_);
  Serial.printf_P(PSTR("[eventi] EEPROM 0x%02X %s, ultimo evento #%lu (lettura %lu ms)\n"), addr,
                  ok ? "pronta" : "illeggibile", (unsigned long)ring_.lastSeq(),
                  (unsigned long)(millis() - t0));
  return ok;
}

bool EventLog::add(uint32_t epoch, Ev code, uint8_t a, uint16_t b, int16_t v) {
  if (!ring_.ok()) return false;
  // Un'ora non ancora attendibile (prima di NTP/RTC) si scrive come 0 = sconosciuta.
  if (epoch < 1500000000UL) epoch = 0;
  if (ring_.add(epoch, (uint8_t)code, a, b, v)) return true;
  if (fails_ < 0xFFFF) fails_++;
  return false;
}

const __FlashStringHelper* EventLog::name(uint8_t code) {
  switch ((Ev)code) {
    case Ev::Boot: return F("avvio");
    case Ev::Arm: return F("uscite armate");
    case Ev::Disarm: return F("uscite disarmate");
    case Ev::Mode: return F("modo");
    case Ev::Step: return F("step");
    case Ev::AlarmOn: return F("allarme");
    case Ev::AlarmOff: return F("fine allarme");
    case Ev::HeatLock: return F("caldo bloccato");
    case Ev::HeatUnlock: return F("caldo sbloccato");
    case Ev::ProfileDone: return F("profilo completato");
    case Ev::Pause: return F("pausa");
    case Ev::Resume: return F("ripresa");
    case Ev::CrashReached: return F("cold crash raggiunto");
    case Ev::Config: return F("configurazione salvata");
    case Ev::Ota: return F("aggiornamento firmware");
    case Ev::ClockJump: return F("salto d'orologio");
    case Ev::CmdRejected: return F("comando rifiutato");
    case Ev::FermDone: return F("fermentazione finita");
    case Ev::FermStuck: return F("fermentazione ferma?");
    case Ev::Confirm: return F("conferma dello step (0 attesa, 1 confermato)");
  }
  return F("?");
}
