#pragma once
#include <stdint.h>
#include "ax/Types.h"

namespace ax {

// Arbitro caldo/freddo: mai i due insieme, più un tempo morto minimo quando si
// inverte il verso (il caldo non parte subito dopo il freddo e viceversa).
class Interlock {
 public:
  struct Result {
    OutputRequest out;
    bool conflict = false;       // richiesta contemporanea caldo+freddo (anomalia)
    bool switchBlocked = false;  // inversione di verso trattenuta dal tempo morto
  };

  void configure(uint32_t minSwitchSec) { minSwitchSec_ = minSwitchSec; }

  Result arbitrate(uint32_t now, OutputRequest want) {
    Result r;
    if (want.heat && want.cool) {
      // Anomalia del controller: stato sicuro e segnalazione.
      want.heat = want.cool = false;
      r.conflict = true;
    }
    // Con tempo morto richiesto, il verso opposto è bloccato sia mentre l'altro lato
    // è ANCORA fisicamente attivo (transizione nello stesso tick), sia per
    // minSwitchSec dopo il suo spegnimento.
    if (minSwitchSec_ > 0) {
      if (want.heat && (coolOn_ || (coolEverOn_ &&
          elapsed(now, coolOffAt_) < (int64_t)minSwitchSec_))) {
        want.heat = false;
        r.switchBlocked = true;
      }
      if (want.cool && (heatOn_ || (heatEverOn_ &&
          elapsed(now, heatOffAt_) < (int64_t)minSwitchSec_))) {
        want.cool = false;
        r.switchBlocked = true;
      }
    }
    r.out = want;
    return r;
  }

  // Registra lo stato finale applicato (dopo guardia compressore e failsafe).
  void update(uint32_t now, bool heatActual, bool coolActual) {
    if (heatOn_ && !heatActual) heatOffAt_ = now;
    if (coolOn_ && !coolActual) coolOffAt_ = now;
    if (heatActual) heatEverOn_ = true;
    if (coolActual) coolEverOn_ = true;
    heatOn_ = heatActual;
    coolOn_ = coolActual;
  }

  // Salto d'orologio: trasla i timestamp di spegnimento come motore e guardia, così
  // il tempo morto residuo resta invariato. Senza, un salto all'indietro terrebbe
  // switchBlocked attivo (o, con elapsed negativo, ne falserebbe la durata) per
  // l'ampiezza del salto. 0 = mai spento: non si trasla (è gated da *EverOn_).
  void shiftClock(int64_t deltaSec) {
    auto shift = [deltaSec](uint32_t& e) {
      if (e == 0) return;
      int64_t v = (int64_t)e + deltaSec;
      e = v > 1 ? (uint32_t)v : 1;
    };
    shift(heatOffAt_);
    shift(coolOffAt_);
  }

 private:
  static int64_t elapsed(uint32_t now, uint32_t since) {
    return (int64_t)now - (int64_t)since;
  }
  uint32_t minSwitchSec_ = 120;
  bool heatOn_ = false, coolOn_ = false;
  bool heatEverOn_ = false, coolEverOn_ = false;
  uint32_t heatOffAt_ = 0, coolOffAt_ = 0;
};

}  // namespace ax
