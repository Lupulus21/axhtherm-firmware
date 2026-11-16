#pragma once
#include <stdint.h>
#include "ax/Types.h"
#include "ax/HysteresisController.h"
#include "ax/PidController.h"
#include "ax/CompressorGuard.h"
#include "ax/Interlock.h"
#include "ax/PeakEstimator.h"

namespace ax {

// Catena di controllo completa: controller -> interlock -> guardia compressore -> uscite.
// È L'UNICO punto che decide lo stato dei relè caldo/freddo. Qualunque condizione
// dubbia (setpoint assente, sonda non valida, non armato) produce TUTTO SPENTO.
class ControlPipeline {
 public:
  struct Cfg {
    bool heatUsePid = true;         // false: isteresi anche sul caldo
    bool predictive = true;         // spegnimento predittivo (freddo; caldo se a isteresi)
    float toleranceC = 0.5f;
    uint32_t minStateSec = 60;      // anti-chattering isteresi
    uint32_t minSwitchSec = 120;    // tempo morto inversione caldo<->freddo
    uint32_t compressorRestSec = 300;
    uint32_t compressorMinOnSec = 60;
    PidController::Cfg pid;
  };

  struct Status {
    bool heat = false;
    bool cool = false;
    float pidDuty = 0.0f;
    bool conflict = false;
    bool switchBlocked = false;
    bool predStop = false;        // stop anticipato accettato dal latch d'isteresi (il
                                  // relè freddo può restare fisicamente acceso finché
                                  // compressorMinOnSec non è trascorso)
    uint32_t coolBlockedSec = 0;  // secondi residui di protezione compressore
  };

  void configure(const Cfg& c) {
    cfg_ = c;
    hyst_.configure(c.toleranceC, c.minStateSec);
    pid_.configure(c.pid);
    guard_.configure(c.compressorRestSec, c.compressorMinOnSec);
    lock_.configure(c.minSwitchSec);
    coolEst_.configureSide(true);
    heatEst_.configureSide(false);
    // I coefficienti appresi NON si toccano: sopravvivono ai salvataggi di config.
  }

  // Lo stato dei rele' e' stato forzato dall'ESTERNO (failsafe hardware): quello che la
  // pipeline ricorda non corrisponde piu' alla realta'. Si azzera come nel ramo di fault:
  // i coefficienti APPRESI restano, ma l'osservazione in corso va scartata — gli
  // stimatori avrebbero attribuito a un ciclo di marcia una deriva avvenuta a rele' spenti.
  void resetActuationState() {
    coolEst_.reset();
    heatEst_.reset();
    prevCool_ = prevHeat_ = false;
  }

  CompressorGuard& guard() { return guard_; }
  const CompressorGuard& guard() const { return guard_; }

  // Salto d'orologio: propaga la traslazione a TUTTO lo stato interno della pipeline
  // che vive su epoch — guardia compressore, latch d'isteresi, tempo morto interlock.
  // Punto unico per costruzione: così un salto non può dimenticare un latch e
  // congelarlo (è la forma stessa del difetto che questa correzione chiude). Gli
  // stimatori predittivi non usano epoch (contatori relativi): non vanno traslati.
  void shiftClock(int64_t deltaSec) {
    guard_.shiftClock(deltaSec);
    hyst_.shiftClock(deltaSec);
    lock_.shiftClock(deltaSec);
  }

  // Coefficienti dello spegnimento predittivo (°C per ora di marcia), per la
  // persistenza su /state.json e per la diagnostica in /api/status.
  float coolEstimatorCph() const { return coolEst_.coefficientCph(); }
  float heatEstimatorCph() const { return heatEst_.coefficientCph(); }
  void setEstimators(float coolCph, float heatCph) {
    coolEst_.setCoefficientCph(coolCph);
    heatEst_.setCoefficientCph(heatCph);
  }

  // allowHeat/allowCool: politica di attuazione dello step di profilo corrente
  // (free-rise = niente caldo; crash naturale = niente freddo). Default: entrambi.
  Status tick(uint32_t nowEpoch, uint32_t nowMs, bool hasSetpoint, float setpointC,
              bool tempValid, float tempC, bool outputsAllowed, float dtSec,
              bool allowHeat = true, bool allowCool = true) {
    Status st;
    if (!outputsAllowed || !hasSetpoint || !tempValid) {
      // Stato sicuro. La guardia registra comunque lo spegnimento (protezione valida).
      guard_.update(nowEpoch, false);
      lock_.update(nowEpoch, false, false);
      st.coolBlockedSec = guard_.blockedForSec(nowEpoch);
      // Fault/disarmo: lo stato del ciclo osservato dagli stimatori non è più
      // rappresentativo del relè (i coefficienti appresi restano).
      coolEst_.reset();
      heatEst_.reset();
      prevCool_ = prevHeat_ = false;
      return st;
    }

    // Spegnimento predittivo: gli stimatori osservano SEMPRE lo stato fisico del
    // tick precedente (dopo guardia e interlock), anche a predittivo spento o col
    // caldo in PID — così l'apprendimento resta sincrono col relè attraverso i
    // cambi di configurazione. I flag decidono solo se ONORARE lo stop anticipato.
    bool coolEarly = coolEst_.tick(prevCool_, tempC, setpointC, dtSec);
    bool heatEarly = heatEst_.tick(prevHeat_, tempC, setpointC, dtSec);
    if (!cfg_.predictive) coolEarly = heatEarly = false;
    if (cfg_.heatUsePid) heatEarly = false;

    OutputRequest want = hyst_.evaluate(nowEpoch, setpointC, tempC);
    if (cfg_.heatUsePid) {
      st.pidDuty = pid_.update(setpointC, tempC, dtSec);
      want.heat = pid_.windowOutput(nowMs);
    }
    // Lo stop anticipato passa dal latch dell'isteresi (riaccensione solo dalla
    // soglia piena set+tol) e ne rispetta l'anti-chattering: se minStateSec non è
    // trascorso il forceOff rifiuta e si ritenta al tick successivo.
    if (coolEarly && want.cool && hyst_.forceCoolOff(nowEpoch)) {
      want.cool = false;
      st.predStop = true;
    }
    if (heatEarly && want.heat && hyst_.forceHeatOff(nowEpoch)) {
      want.heat = false;
      st.predStop = true;
    }
    // Politica di attuazione dello step: il verso vietato non si eccita mai
    // (il latch d'isteresi resta libero di evolvere: al cambio step riprende).
    if (!allowHeat) {
      want.heat = false;
      if (cfg_.heatUsePid) pid_.reset();  // niente integrale accumulato mentre è vietato
      // Uno stop imposto da fuori (step, limiti, blocco) non e' l'inerzia dell'impianto:
      // lo stimatore predittivo non deve impararci sopra.
      heatEst_.cancelObservation();
    }
    if (!allowCool) {
      want.cool = false;
      coolEst_.cancelObservation();
    }
    if (want.cool) {
      want.heat = false;  // il freddo ha priorità: mai scaldare mentre si raffredda
      if (cfg_.heatUsePid) pid_.reset();
    }

    Interlock::Result lr = lock_.arbitrate(nowEpoch, want);
    st.conflict = lr.conflict;
    st.switchBlocked = lr.switchBlocked;

    bool coolFinal;
    if (guard_.isOn()) {
      coolFinal = lr.out.cool || guard_.mustStayOn(nowEpoch);
    } else {
      coolFinal = lr.out.cool && guard_.canStart(nowEpoch);
    }
    bool heatFinal = lr.out.heat && !coolFinal;

    guard_.update(nowEpoch, coolFinal);
    lock_.update(nowEpoch, heatFinal, coolFinal);
    // Un lato in marcia rende non-passiva la deriva che l'ALTRO stimatore stava
    // osservando: quel campione va scartato, non insegnerebbe nulla di affidabile.
    if (coolFinal) heatEst_.cancelObservation();
    if (heatFinal) coolEst_.cancelObservation();
    prevCool_ = coolFinal;
    prevHeat_ = heatFinal;

    st.heat = heatFinal;
    st.cool = coolFinal;
    st.coolBlockedSec = guard_.blockedForSec(nowEpoch);
    return st;
  }

 private:
  Cfg cfg_;
  HysteresisController hyst_;
  PidController pid_;
  CompressorGuard guard_;
  Interlock lock_;
  PeakEstimator coolEst_, heatEst_;
  bool prevCool_ = false, prevHeat_ = false;
};

}  // namespace ax
