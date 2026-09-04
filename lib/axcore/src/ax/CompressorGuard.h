#pragma once
#include <stdint.h>

namespace ax {

// Protezione del compressore: riposo minimo fra due avvii e tempo minimo di marcia.
// Lavora su epoch UTC assoluti persistiti: la protezione vale anche attraverso i riavvii.
// Se l'orologio va all'indietro (RTC inaffidabile) il tempo trascorso risulta negativo
// e l'avvio resta bloccato: comportamento conservativo voluto.
class CompressorGuard {
 public:
  struct Persist {
    uint32_t lastOnEpoch = 0;
    uint32_t lastOffEpoch = 0;
  };

  void configure(uint32_t restSec, uint32_t minOnSec) { restSec_ = restSec; minOnSec_ = minOnSec; }

  // Sfasamento per camera dell'istante di avvio consentito. ALLUNGA il riposo, non lo
  // accorcia mai: e' strettamente conservativo e non puo' indebolire la protezione.
  // Serve a impedire che i compressori di due camere diventino avviabili nello stesso
  // secondo (dopo un blackout partono tutti dallo stesso epoch di riferimento).
  void setStartStagger(uint32_t sec) { staggerSec_ = sec; }
  void restore(const Persist& p, bool wasRunning) { st_ = p; on_ = wasRunning; }
  Persist persist() const { return st_; }

  // Da chiamare al boot quando l'RTC non è affidabile: il ritardo si assume NON trascorso.
  void assumeJustStopped(uint32_t nowEpoch) {
    st_.lastOffEpoch = nowEpoch;
    st_.lastOnEpoch = nowEpoch;
    on_ = false;
  }

  bool isOn() const { return on_; }

  bool canStart(uint32_t now) const {
    const int64_t req = (int64_t)restSec_ + (int64_t)staggerSec_;
    return elapsed(now, st_.lastOffEpoch) >= req &&
           elapsed(now, st_.lastOnEpoch) >= req;
  }

  bool mustStayOn(uint32_t now) const {
    return on_ && elapsed(now, st_.lastOnEpoch) < (int64_t)minOnSec_;
  }

  // Registra lo stato effettivo applicato all'uscita freddo.
  void update(uint32_t now, bool coolActual) {
    if (coolActual && !on_) st_.lastOnEpoch = now;
    if (!coolActual && on_) st_.lastOffEpoch = now;
    on_ = coolActual;
  }

  // L'orologio di sistema ha fatto un salto: trasla gli epoch così che il tempo di
  // riposo già maturato (o mancante) resti invariato.
  void shiftClock(int64_t deltaSec) {
    auto shift = [deltaSec](uint32_t& e) {
      if (e == 0) return;
      int64_t v = (int64_t)e + deltaSec;
      e = v > 1 ? (uint32_t)v : 1;
    };
    shift(st_.lastOnEpoch);
    shift(st_.lastOffEpoch);
  }

  uint32_t restSec() const { return restSec_; }
  // Secondi mancanti al prossimo avvio consentito (0 = subito).
  uint32_t blockedForSec(uint32_t now) const {
    const int64_t req = (int64_t)restSec_ + (int64_t)staggerSec_;
    int64_t a = req - elapsed(now, st_.lastOffEpoch);
    int64_t b = req - elapsed(now, st_.lastOnEpoch);
    int64_t m = a > b ? a : b;
    return m > 0 ? (uint32_t)m : 0;
  }

 private:
  static int64_t elapsed(uint32_t now, uint32_t since) {
    if (since == 0) return INT64_MAX;  // mai avvenuto: nessun vincolo
    return (int64_t)now - (int64_t)since;
  }
  uint32_t restSec_ = 300;
  uint32_t minOnSec_ = 60;
  uint32_t staggerSec_ = 0;  // 0 = nessuno sfasamento (camera 0 / camera singola)
  Persist st_;
  bool on_ = false;
};

}  // namespace ax
