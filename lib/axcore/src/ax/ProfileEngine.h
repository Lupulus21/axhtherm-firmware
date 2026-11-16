#pragma once
#include <stdint.h>
#include "ax/Types.h"

namespace ax {

// Macchina a stati del profilo di fermentazione (fino a kMaxSteps = 12 step, 4 tipi).
// Tutto il tempo è in epoch UTC assoluti: la ripresa dopo un riavvio è un semplice
// restore dello snapshot, e le rampe a tempo ripartono dalla posizione corrente.
class ProfileEngine {
 public:
  // "Attendi il target": attesa massima prima di far partire comunque il tempo dello step.
  static constexpr int64_t kWaitMaxSec = 24 * 3600;
  // Ripresa dopo un'interruzione: uno step scaduto da piu' di cosi' e' scaduto mentre il
  // controllo era spento (blackout, scheda ferma), non in un tick normale.
  static constexpr int64_t kResumeGapSec = 600;

  struct TickInput {
    uint32_t nowEpoch = 0;
    float tempC = 0.0f;
    bool tempValid = false;
    float gravity = 0.0f;
    bool gravityValid = false;  // esiste una lettura
    bool gravityFresh = false;  // ...ed è recente (non stantia)
    // Attenuazione apparente (%), calcolata FUORI dal motore (dipende dall'unità
    // SG/°P e dall'OG catturata): il motore la confronta e basta.
    float attenuationPct = 0.0f;
    bool attenuationValid = false;
    // Istante della lettura di densita' (3.16.0): distingue una lettura NUOVA dalla stessa
    // ripetuta a ogni tick. 0 = non noto (ogni tick conta come lettura nuova).
    uint32_t gravityEpoch = 0;
  };

  struct TickOutput {
    bool hasSetpoint = false;
    float setpointC = 0.0f;
    bool stepChanged = false;
    bool profileCompleted = false;   // transizione a COMPLETED avvenuta in questo tick
    bool waitingGravity = false;     // step a densità fermo per dato assente/stantio
    bool waitingDelayedStart = false;
    // Step RampRate (°C/ora) fermo: il setpoint ha raggiunto il target ma la
    // temperatura misurata non entra in tolleranza — l'impianto non ci arriva.
    // Senza timeout il profilo resterebbe appeso per sempre: va reso VISIBILE.
    bool waitingRampStall = false;
    // Step "attendi il target": il mosto non e' ancora arrivato, il tempo non conta.
    bool waitingTarget = false;
    // Politica di attuazione dello step corrente (sempre true fuori dal profilo).
    bool allowHeat = true;
    bool allowCool = true;
    // Step finito che aspetta la conferma dell'utente (StepDef::confirm).
    bool waitingConfirm = false;
  };

  void configure(float toleranceC) { tol_ = toleranceC < 0.1f ? 0.1f : toleranceC; }

  void setSteps(const StepDef* steps, uint8_t count);
  const StepDef* steps() const { return steps_; }
  uint8_t stepCount() const { return count_; }

  // Comandi (dalla UI/API).
  bool startProfile(uint32_t now, float tempC, bool tempValid, uint32_t delayedStartEpoch = 0);
  void stop();
  void pause(uint32_t now);
  void resume(uint32_t now);
  bool skip(uint32_t now, float tempC, bool tempValid);
  // Conferma dell'utente su uno step che la aspetta: si passa al successivo (o a fine
  // profilo). False se non c'era nulla da confermare.
  bool confirm(uint32_t now, float tempC, bool tempValid);
  bool waitingConfirm() const {
    return s_.mode == RunMode::Profile && !s_.completed && s_.waitingConfirm;
  }
  void setThermostat(bool on, float setpointC);
  void overrideSetpoint(float setpointC);
  void clearOverride() { s_.overrideActive = false; }

  TickOutput tick(const TickInput& in);

  // Persistenza/ripresa.
  EngineSnapshot snapshot() const { return s_; }
  void restore(const EngineSnapshot& snap);

  // Ripresa con orologio inaffidabile: ri-àncora la rampa in corso alla temperatura REALE
  // corrente (disponibile solo al primo tick valido dopo il boot). Così la rampa continua da
  // dove la birra si trova ora, invece di ripartire dall'origine (freddo abbandonato) o di
  // essere congelata al target (che romperebbe il rilevamento di stallo, span=0). Copre tutti
  // i tipi di rampa: per RampTime-fromCurrent=false aggiorna startC nella copia RAM dello step
  // (non lo step persistito). Da chiamare solo su uno step di tipo rampa.
  void reanchorCurrentRamp(uint32_t now, float tempC) {
    // Solo su una rampa ATTIVA (non completata). Il tipo va ri-verificato QUI, al momento del
    // consumo: fra il boot e il primo tick valido lo step può essere avanzato a uno non-rampa
    // (gli step a tempo scadono anche senza sonda). Non tocca la definizione dello step
    // (steps_): l'origine ri-ancorata vive in stepStartTemp + rampReanchored.
    if (s_.mode != RunMode::Profile || s_.completed || s_.stepIdx >= count_) return;
    if (s_.delayedStartEpoch != 0) return;  // in attesa di partenza ritardata: lo step non è iniziato
    // NB: un profilo IN PAUSA su una rampa va ri-ancorato eccome. reanchorCurrentRamp è chiamata
    // SOLO al boot (via rebaseRampPending), e lì pausedAtEpoch è già ri-basato a now insieme a
    // stepStartEpoch (main.cpp): niente desync. Saltare il ri-ancoraggio in pausa lascerebbe la
    // rampa a ripartire dall'origine STANTIA (es. riscaldando la birra al vecchio start) — peggio.
    const StepType t = steps_[s_.stepIdx].type;
    if (t != StepType::RampRate && t != StepType::RampTime) return;
    s_.stepStartEpoch = now;
    s_.stepStartTemp = tempC;
    s_.stepStartTempValid = true;
    s_.rampReanchored = true;
  }

  // L'orologio di sistema ha fatto un salto di deltaSec (sync NTP dopo un boot con RTC
  // inaffidabile, o impostazione manuale): trasla gli epoch àncora così che il tempo
  // trascorso negli step resti invariato. NON si traslano: delayedStartEpoch (istante
  // assoluto scelto dall'utente) e profileStartEpoch (istante REALE di inizio cotta):
  // traslare quest'ultimo dopo un blackout con RTC morto lo spingerebbe nel futuro
  // (~2081) e i giorni di fermentazione sparirebbero; lasciandolo, dopo il sync NTP
  // resta l'inizio vero e i giorni tornano corretti.
  void shiftClock(int64_t deltaSec) {
    auto shift = [deltaSec](uint32_t& e) {
      if (e == 0) return;
      int64_t v = (int64_t)e + deltaSec;
      e = v > 1 ? (uint32_t)v : 1;
    };
    shift(s_.stepStartEpoch);
    shift(s_.pausedAtEpoch);
    shift(s_.stableSinceEpoch);
  }

  RunMode mode() const { return s_.mode; }
  bool running() const { return s_.mode == RunMode::Profile && !s_.completed; }
  bool paused() const { return s_.paused; }
  bool completed() const { return s_.completed; }
  uint8_t stepIdx() const { return s_.stepIdx; }
  uint32_t stepStartEpoch() const { return s_.stepStartEpoch; }
  float thermostatSetpoint() const { return s_.thermostatSetpoint; }
  bool overrideActive() const { return s_.overrideActive; }
  // Fine prevista dello step corrente (0 = non determinabile: rampa °C/h o densità).
  uint32_t stepEndsAtEpoch() const;

 private:
  float stepSetpoint(const StepDef& st, int64_t elapsedSec) const;
  bool stepDone(const StepDef& st, int64_t elapsedSec, const TickInput& in, float sp,
                bool* waitingGravity);
  bool gravityConditionMet(const StepDef& st, const TickInput& in);
  void enterStep(uint8_t idx, uint32_t now, float tempC, bool tempValid, float fallbackSp);
  int8_t nextEnabled(int8_t from) const;

  StepDef steps_[kMaxSteps];
  uint8_t count_ = 0;
  float tol_ = 0.5f;
  EngineSnapshot s_;
  // "Densita' stabile": letture fuori banda di fila non ancora confermate (3.16.0). Non
  // persistite: dopo un riavvio si riparte da zero, al massimo una lettura in piu'.
  uint8_t stableOut_ = 0;
  uint32_t stableLastReading_ = 0;
};

}  // namespace ax
