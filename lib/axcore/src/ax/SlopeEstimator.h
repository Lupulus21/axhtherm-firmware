#pragma once
#include <stdint.h>

namespace ax {

// Pendenza della temperatura (°C/h) su una finestra di 20 minuti, a piena risoluzione.
// Un campione al minuto in un anello di kN valori: la pendenza confronta la media dei 3
// campioni piu' recenti con quella dei 3 piu' vecchi. Con la DS18B20 a 0,0625 °C l'errore
// di quantizzazione resta sotto ~0,2 °C/h, mentre la derivata minuto per minuto oscillerebbe
// di +-3,75 °C/h. Serve al tempo d'arrivo al target mostrato in dashboard.
class SlopeEstimator {
 public:
  static constexpr uint8_t kN = 21;            // 21 campioni = 20 minuti
  static constexpr float kPeriodSec = 60.0f;   // un campione al minuto
  // Sonda invalida oltre 90 s: si riparte. Il calcolo assume campioni a passo regolare, e
  // un buco lungo lo falserebbe (pendenza sovrastimata finche' la finestra non si rinnova).
  static constexpr float kMaxGapSec = 90.0f;

  void reset() { n_ = 0; head_ = 0; acc_ = 0.0f; gap_ = 0.0f; }

  // Da chiamare a ogni tick con il tempo reale trascorso.
  void tick(float dtSec, bool valid, float tempC) {
    if (!valid) {
      gap_ += dtSec;
      if (gap_ > kMaxGapSec) reset();
      return;
    }
    gap_ = 0.0f;
    acc_ += dtSec;
    if (n_ && acc_ < kPeriodSec) return;
    acc_ = 0.0f;
    buf_[head_] = tempC;
    head_ = (uint8_t)((head_ + 1) % kN);
    if (n_ < kN) n_++;
  }

  bool valid() const { return n_ == kN; }

  // °C/h; 0 se la finestra non e' ancora piena.
  float cph() const {
    if (!valid()) return 0.0f;
    float nuovi = 0.0f, vecchi = 0.0f;
    for (uint8_t k = 0; k < 3; k++) {
      nuovi += buf_[(head_ + kN - 1 - k) % kN];
      vecchi += buf_[(head_ + k) % kN];
    }
    // Centri dei due gruppi distanti kN-3 periodi.
    const float ore = (float)(kN - 3) * kPeriodSec / 3600.0f;
    return (nuovi - vecchi) / 3.0f / ore;
  }

  // Minuti per arrivare a targetC, -1 se non si sta andando verso il target (o troppo
  // lentamente per una stima sensata: sotto 0,05 °C/h, o oltre una settimana).
  int32_t etaMin(float tempC, float targetC, float tolC) const {
    if (!valid()) return -1;
    const float diff = targetC - tempC;
    if (diff <= tolC && diff >= -tolC) return 0;
    const float v = cph();
    if ((diff > 0 && v < 0.05f) || (diff < 0 && v > -0.05f)) return -1;
    const float min = diff / v * 60.0f;
    return min > 7.0f * 24.0f * 60.0f ? -1 : (int32_t)(min + 0.5f);
  }

 private:
  float buf_[kN] = {};
  uint8_t n_ = 0, head_ = 0;
  float acc_ = 0.0f, gap_ = 0.0f;
};

}  // namespace ax
