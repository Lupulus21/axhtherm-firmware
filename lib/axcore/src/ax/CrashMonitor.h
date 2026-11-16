#pragma once
#include <stdint.h>

namespace ax {

// Andamento del cold crash: discesa REALE del mosto nelle ultime 6 ore. Serve a due cose:
// - la stima del tempo d'arrivo alla temperatura di cold crash (dashboard);
// - l'allarme se il mosto scende meno di una soglia minima in 6 ore (frigo che non ce la fa,
//   porta aperta, sonda fuori posto). Una discesa piu' lenta di quella programmata va bene;
//   sotto la soglia no.
// Un campione ogni 10 minuti: con la massa del mosto la temperatura cambia lentamente, e
// sei ore di campioni costano 37 float. Finestra persa al riavvio (si riparte da zero).
class CrashMonitor {
 public:
  static constexpr float kSampleSec = 600.0f;   // un campione ogni 10 minuti
  static constexpr uint8_t kN = 37;             // 36 intervalli = 6 ore
  static constexpr float kMaxGapSec = 1800.0f;  // sonda assente oltre 30 min: si riparte
  static constexpr float kMinEtaHours = 1.0f;   // sotto un'ora di dati la stima e' programmata

  void reset() { n_ = head_ = 0; acc_ = gap_ = 0.0f; }

  void tick(float dtSec, bool valid, float tempC) {
    // Il tempo conta anche con la sonda assente: i campioni restano a 10 minuti di tempo
    // REALE, o con buchi ripetuti la finestra "di 6 ore" ne coprirebbe di piu' e la
    // discesa sembrerebbe piu' veloce di quella vera.
    acc_ += dtSec;
    if (!valid) {
      gap_ += dtSec;
      if (gap_ > kMaxGapSec) reset();
      return;
    }
    gap_ = 0.0f;
    if (n_ && acc_ < kSampleSec) return;
    acc_ = 0.0f;
    buf_[head_] = tempC;
    head_ = (uint8_t)((head_ + 1) % kN);
    if (n_ < kN) n_++;
  }

  // Ore coperte dai campioni (0 con meno di due campioni).
  float windowHours() const { return n_ < 2 ? 0.0f : (n_ - 1) * kSampleSec / 3600.0f; }
  bool full() const { return n_ == kN; }
  // Discesa nella finestra (positiva = il mosto e' sceso).
  float drop() const {
    if (n_ < 2) return 0.0f;
    return buf_[(head_ + kN - n_) % kN] - buf_[(head_ + kN - 1) % kN];
  }
  // Velocita' di discesa misurata (°C/h), 0 se la finestra e' sotto un'ora.
  float rateCph() const {
    const float h = windowHours();
    return h >= kMinEtaHours ? drop() / h : 0.0f;
  }

  // Discesa troppo lenta: sei ore di dati, mosto ancora sopra target + tolleranza, e in
  // quelle sei ore e' sceso meno di minDropC.
  bool slow(float tempC, float targetC, float tolC, float minDropC) const {
    return full() && tempC > targetC + tolC && drop() < minDropC;
  }

  // Minuti all'arrivo: dalla discesa misurata se c'e' almeno un'ora di dati e il mosto
  // scende davvero, altrimenti dalla velocita' programmata. -1 se gia' arrivato o se la
  // stima non ha senso (mosto fermo o in salita dopo un'ora di dati). *measured dice quale.
  int32_t etaMin(float tempC, float targetC, float tolC, float programmedCph,
                 bool* measured) const {
    if (measured) *measured = false;
    const float manca = tempC - targetC;
    if (manca <= tolC) return -1;
    float v = programmedCph;
    if (windowHours() >= kMinEtaHours) {
      v = rateCph();
      if (measured) *measured = true;
      if (v < 0.05f) return -1;  // non scende: nessuna stima onesta
    }
    if (v < 0.01f) return -1;
    const float min = manca / v * 60.0f;
    return min > 14.0f * 24.0f * 60.0f ? -1 : (int32_t)(min + 0.5f);
  }

 private:
  float buf_[kN] = {};
  uint8_t n_ = 0, head_ = 0;
  float acc_ = 0.0f, gap_ = 0.0f;
};

}  // namespace ax
