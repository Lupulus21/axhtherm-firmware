#pragma once
#include <stdint.h>
#include "ax/Types.h"

namespace ax {

// Isteresi a doppia soglia per verso: il freddo si eccita a set+tol e si spegne a set,
// il caldo si eccita a set-tol e si spegne a set. minStateSec evita il chattering.
class HysteresisController {
 public:
  void configure(float toleranceC, uint32_t minStateSec) {
    tol_ = toleranceC < 0.1f ? 0.1f : toleranceC;
    minStateSec_ = minStateSec;
  }

  OutputRequest evaluate(uint32_t nowEpoch, float setpointC, float tempC) {
    bool wantHeat = heatOn_;
    bool wantCool = coolOn_;
    if (tempC >= setpointC + tol_) wantCool = true;
    else if (tempC <= setpointC) wantCool = false;
    if (tempC <= setpointC - tol_) wantHeat = true;
    else if (tempC >= setpointC) wantHeat = false;
    if (wantCool) wantHeat = false;  // coerenza interna: mai richieste opposte insieme

    if (wantHeat != heatOn_ && elapsed(nowEpoch, heatChangedAt_) >= minStateSec_) {
      heatOn_ = wantHeat;
      heatChangedAt_ = nowEpoch;
    }
    if (wantCool != coolOn_ && elapsed(nowEpoch, coolChangedAt_) >= minStateSec_) {
      coolOn_ = wantCool;
      coolChangedAt_ = nowEpoch;
    }
    return {heatOn_, coolOn_};
  }

  // Spegnimento anticipato deciso a monte (spegnimento predittivo): azzera il latch
  // come se il setpoint fosse stato attraversato, così la riaccensione richiede di
  // nuovo la soglia piena set±tol e non riparte a metà banda. L'anti-chattering vale
  // anche qui: sotto minStateSec lo stop è rifiutato (si ritenta ai tick successivi).
  // Ritorna true se applicato.
  bool forceCoolOff(uint32_t nowEpoch) {
    if (!coolOn_ || elapsed(nowEpoch, coolChangedAt_) < minStateSec_) return false;
    coolOn_ = false;
    coolChangedAt_ = nowEpoch;
    return true;
  }
  bool forceHeatOff(uint32_t nowEpoch) {
    if (!heatOn_ || elapsed(nowEpoch, heatChangedAt_) < minStateSec_) return false;
    heatOn_ = false;
    heatChangedAt_ = nowEpoch;
    return true;
  }

  void reset() { heatOn_ = coolOn_ = false; heatChangedAt_ = coolChangedAt_ = 0; }

  // L'orologio di sistema ha fatto un salto: trasla i timestamp àncora dei latch
  // come si fa per motore e guardia compressore, così il tempo di stato già maturato
  // resta invariato. Senza, un salto all'indietro renderebbe elapsed() negativo e
  // congelerebbe il latch (relè bloccato) per l'ampiezza del salto. 0 = mai commutato:
  // nessun vincolo, non si trasla.
  void shiftClock(int64_t deltaSec) {
    auto shift = [deltaSec](uint32_t& e) {
      if (e == 0) return;
      int64_t v = (int64_t)e + deltaSec;
      e = v > 1 ? (uint32_t)v : 1;
    };
    shift(heatChangedAt_);
    shift(coolChangedAt_);
  }

 private:
  static int64_t elapsed(uint32_t now, uint32_t since) {
    if (since == 0) return INT64_MAX;  // mai commutato: nessun vincolo
    return (int64_t)now - (int64_t)since;
  }
  float tol_ = 0.5f;
  uint32_t minStateSec_ = 60;
  bool heatOn_ = false, coolOn_ = false;
  uint32_t heatChangedAt_ = 0, coolChangedAt_ = 0;
};

}  // namespace ax
