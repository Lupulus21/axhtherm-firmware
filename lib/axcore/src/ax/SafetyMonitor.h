#pragma once
#include <stdint.h>

namespace ax {

// Aggregatore dei fault: decide se le uscite possono attuare.
// Regola: qualunque dubbio -> uscite spente.
struct SafetyMonitor {
  bool controlProbeFault = false;  // sonda di controllo assente/CRC/fuori range
  bool configInvalid = false;      // configurazione corrotta o non caricata
  bool rtcUntrusted = false;       // solo allarme: il controllo continua su millis()
  bool gravityStale = false;       // solo allarme: blocca l'avanzamento step a densità
  bool rampStalled = false;        // solo allarme: rampa °C/h che l'impianto non raggiunge
  bool storageFault = false;       // solo allarme: una scrittura su LittleFS è fallita
  bool thermalDeviation = false;   // solo allarme: T lontana dal setpoint stabile da ore
  bool controlProbeSuspect = false;  // solo allarme: la sonda di controllo diverge da TUTTE le
                                     // ausiliarie (legge plausibile-ma-sbagliato?); NON taglia le uscite
  bool outputsFault = false;       // driver delle uscite (MCP23008) non risponde
  bool notArmed = true;            // mapping relè non ancora confermato dall'utente

  bool outputsAllowed() const {
    return !controlProbeFault && !configInvalid && !outputsFault && !notArmed;
  }
  bool anyAlarm() const {
    return controlProbeFault || configInvalid || rtcUntrusted || gravityStale ||
           rampStalled || storageFault || thermalDeviation || controlProbeSuspect || outputsFault;
  }
};

}  // namespace ax
