#pragma once
#include <stdint.h>

namespace ax {

constexpr uint8_t kMaxSteps = 12;  // 8 fino alla v2.11: i profili lager completi ne chiedono di più

enum class StepType : uint8_t { Fixed = 0, RampTime = 1, RampRate = 2, Gravity = 3 };

// Condizione di avanzamento degli step a densità.
enum class GravityMode : uint8_t {
  Below = 0,        // densità <= gravityTarget (SG o °P, com'è configurato lo iSpindel)
  Attenuation = 1,  // attenuazione apparente >= gravityTarget (in %; richiede OG nota)
  Stable = 2,       // densità ferma entro stableBand per stableHours consecutive
};

struct StepDef {
  StepType type = StepType::Fixed;
  bool enabled = false;
  float targetC = 18.0f;
  bool fromCurrent = true;      // RampTime: partenza dalla temperatura misurata a inizio step
  float startC = 18.0f;         // RampTime con fromCurrent=false
  uint32_t durationSec = 0;     // Fixed / RampTime
  float degPerHour = 0.5f;      // RampRate (valore assoluto; la direzione la dà il target)
  float gravityTarget = 1.010f; // Gravity: soglia (Below) o % attenuazione (Attenuation)
  GravityMode gravityMode = GravityMode::Below;
  uint32_t maxDurationSec = 0;  // Gravity: timeout di sicurezza (0 = nessuno) — un densimetro
                                // fermo/incrostato non deve bloccare il profilo per sempre
  float stableBand = 0.002f;    // Stable: variazione massima (unità della densità)
  float stableHours = 48.0f;    // Stable: finestra di stabilità richiesta
  bool allowHeat = true;        // politica di attuazione per step: false = niente caldo
  bool allowCool = true;        // (free-rise: allowHeat=false; crash naturale: allowCool=false)
  // Fixed: il tempo dello step parte quando il mosto entra nella tolleranza del target, non
  // all'ingresso nello step ("attendi il target"); al massimo dopo un giorno di attesa.
  bool waitTarget = false;
  // "Attendi conferma" (3.16.0, CT-14): a fine step il profilo NON avanza da solo, tiene il
  // setpoint di fine step e aspetta l'utente (dry hop, campione, travaso).
  bool confirm = false;
};

enum class RunMode : uint8_t { Off = 0, Thermostat = 1, Profile = 2 };

struct OutputRequest {
  bool heat = false;
  bool cool = false;
};

// Stato minimo persistito per la ripresa dopo reset/blackout (epoch UTC assoluti).
struct EngineSnapshot {
  RunMode mode = RunMode::Off;
  uint8_t stepIdx = 0;
  bool paused = false;
  bool completed = false;
  uint32_t stepStartEpoch = 0;
  float stepStartTemp = 0.0f;
  bool stepStartTempValid = false;
  uint32_t profileStartEpoch = 0;
  uint32_t delayedStartEpoch = 0;  // 0 = nessuna partenza ritardata
  uint32_t pausedAtEpoch = 0;
  float thermostatSetpoint = 18.0f;
  bool overrideActive = false;
  float overrideSetpoint = 0.0f;
  // Rilevamento "densità stabile" (GravityMode::Stable): riferimento corrente e da
  // quando la densità gli resta entro banda. Persistiti: la stabilità maturata
  // sopravvive a un riavvio (epoch assoluti, traslati sui salti d'orologio).
  float stableRefGravity = 0.0f;
  uint32_t stableSinceEpoch = 0;
  // Ripresa con orologio inaffidabile: quando true, l'origine della rampa è stepStartTemp
  // (ri-ancorata alla temperatura reale, o al target per un profilo completato) invece di
  // startC — così la copia RAM degli step resta identica al profilo persistito.
  bool rampReanchored = false;
  // "Attendi il target": lo step Fixed corrente aspetta ancora che il mosto arrivi
  // (stepStartEpoch e' l'ingresso nello step, poi diventa l'arrivo), e se il tempo e'
  // partito per scadenza dell'attesa invece che per arrivo.
  bool waitingTarget = false;
  bool waitTimedOut = false;
  // "Attendi conferma": lo step corrente e' finito e aspetta l'utente. Persistito: un
  // riavvio non deve far avanzare il profilo al posto suo.
  bool waitingConfirm = false;
};

}  // namespace ax
