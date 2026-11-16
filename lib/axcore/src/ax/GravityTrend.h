#pragma once
#include <stdint.h>
#include <math.h>

namespace ax {

// °Plato <-> SG (formule usate anche dalla UI e dal calcolo dell'attenuazione).
inline float platoToSg(float p) { return 1.0f + p / (258.6f - p / 258.2f * 227.1f); }
inline float sgToPlato(float sg) {
  return -616.868f + 1111.14f * sg - 630.272f * sg * sg + 135.997f * sg * sg * sg;
}

// Andamento della densita' (3.16.0). Tiene le medie orarie delle letture FRESCHE delle ultime
// kBins ore e ne calcola la pendenza ai minimi quadrati: le medie orarie assorbono il rumore
// del densimetro (±0,001 SG su un iSpindel), che sulle singole letture farebbe sembrare
// ferma una birra che cala o in calo una birra ferma. Valori in SG: chi lavora in °Plato
// converte prima (platoToSg). 4 byte per ora.
//
// Stato su 48 ore:
//   Stable  - in 48 ore cambia meno di kStableSg (0,001 SG, circa 0,25 °P): la regola
//             classica del birraio, «due giorni con la stessa lettura»;
//   Active  - nelle ultime 24 ore cala di almeno kActiveSgDay al giorno;
//   Slowing - cala, ma piu' piano;
//   Unknown - dati insufficienti (meno di 36 ore coperte) o l'ultima lettura ha piu' di 3 ore.
class GravityTrend {
 public:
  static constexpr uint8_t kBins = 72;
  static constexpr float kStableSg = 0.001f;
  static constexpr float kActiveSgDay = 0.004f;
  enum class State : uint8_t { Unknown = 0, Active = 1, Slowing = 2, Stable = 3 };

  void reset() {
    n_ = 0;
    last_ = 0;
    curN_ = 0;
    curSum_ = 0.0f;
    lastEpoch_ = 0;
  }

  // Una lettura fresca. Le letture fuori ordine (ora precedente all'ultima) si ignorano.
  void add(uint32_t epoch, float sg) {
    if (epoch < 1500000000UL || !(sg > 0.9f && sg < 1.3f)) return;
    const uint16_t h = (uint16_t)((epoch / 3600UL) & 0xFFFF);
    if (n_ && h == bins_[last_].hour) {
      if (curN_ == 255) return;  // densimetro impazzito: la media dell'ora basta cosi'
      curSum_ += sg;
      curN_++;
    } else {
      if (n_ && (uint16_t)(h - bins_[last_].hour) > 0x8000) return;  // nel passato
      last_ = n_ ? (uint8_t)((last_ + 1) % kBins) : 0;
      if (n_ < kBins) n_++;
      bins_[last_].hour = h;
      curSum_ = sg;
      curN_ = 1;
    }
    bins_[last_].sg10k = (uint16_t)lroundf(curSum_ / curN_ * 10000.0f);
    lastEpoch_ = epoch;
  }

  uint32_t lastEpoch() const { return lastEpoch_; }
  uint8_t bins() const { return n_; }

  // Pendenza in SG/giorno (negativa = in calo) sulle ultime windowH ore. False se le ore
  // coperte sono meno di 3/4 della finestra, se i punti sono pochi o se l'ultima lettura
  // e' piu' vecchia di 3 ore.
  bool slopePerDay(uint32_t nowEpoch, uint16_t windowH, float& out) const {
    if (!n_ || windowH < 2) return false;
    const uint16_t nowH = (uint16_t)((nowEpoch / 3600UL) & 0xFFFF);
    if ((uint16_t)(nowH - bins_[last_].hour) > 3) return false;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    uint16_t cnt = 0, maxAge = 0, minAge = 0xFFFF;
    for (uint8_t k = 0; k < n_; k++) {
      const Bin& b = bins_[(uint8_t)((last_ + kBins - k) % kBins)];
      const uint16_t age = (uint16_t)(nowH - b.hour);
      if (age >= windowH) break;  // i bin sono in ordine: i successivi sono piu' vecchi
      const double x = -(double)age;
      const double y = b.sg10k / 10000.0;
      sx += x; sy += y; sxx += x * x; sxy += x * y;
      cnt++;
      if (age > maxAge) maxAge = age;
      if (age < minAge) minAge = age;
    }
    const uint16_t minCnt = windowH / 3 > 6 ? windowH / 3 : 6;
    if (cnt < minCnt || (uint32_t)(maxAge - minAge) * 4 < (uint32_t)windowH * 3) return false;
    const double den = cnt * sxx - sx * sx;
    if (den <= 0) return false;
    out = (float)((cnt * sxy - sx * sy) / den * 24.0);
    return true;
  }

  State state(uint32_t nowEpoch) const {
    float s48, s24;
    const bool ok48 = slopePerDay(nowEpoch, 48, s48);
    if (ok48 && fabsf(s48) * 2.0f < kStableSg) return State::Stable;
    if (slopePerDay(nowEpoch, 24, s24)) return -s24 >= kActiveSgDay ? State::Active : State::Slowing;
    return State::Unknown;
  }

 private:
  struct Bin {
    uint16_t hour;   // (epoch / 3600) modulo 65536: bastano differenze su poche ore
    uint16_t sg10k;  // media dell'ora, SG x 10000
  };
  Bin bins_[kBins] = {};
  uint8_t n_ = 0, last_ = 0;
  uint8_t curN_ = 0;
  float curSum_ = 0.0f;
  uint32_t lastEpoch_ = 0;
};

}  // namespace ax
