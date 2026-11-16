#pragma once
#include <stdint.h>

namespace ax {

// Riduzione dei punti del grafico che NON perde i picchi (3.14.0).
//
// Prima si teneva un campione ogni N: un ciclo breve del compressore o un picco d'aria della
// camera spariva dal grafico se cadeva fra due campioni tenuti. Qui ogni gruppo di campioni
// diventa DUE punti e, per ciascuna serie, il primo porta l'estremo (minimo o massimo) che
// arriva prima nel tempo e il secondo quello che arriva dopo. L'inviluppo resta intero e
// l'ordine nel tempo della singola serie e' rispettato.
//
// I flag (caldo/freddo) sono in OR sull'intero gruppo: un'accensione breve resta visibile.
// Valori interi (°C x100, SG x10000...); kMissing = valore assente.
template <uint8_t N>
class MinMaxGroup {
 public:
  static constexpr int32_t kMissing = INT32_MIN;

  void reset() {
    count_ = 0;
    flags_ = 0;
    for (uint8_t i = 0; i < N; i++) s_[i] = Serie{};
  }

  void add(uint32_t epoch, const int32_t (&v)[N], uint8_t flags) {
    if (count_ == 0) firstEpoch_ = epoch;
    lastEpoch_ = epoch;
    flags_ |= flags;
    for (uint8_t i = 0; i < N; i++) {
      if (v[i] == kMissing) continue;
      Serie& s = s_[i];
      if (!s.has) {
        s.has = true;
        s.min = s.max = v[i];
        s.minAt = s.maxAt = count_;
        continue;
      }
      if (v[i] < s.min) { s.min = v[i]; s.minAt = count_; }
      if (v[i] > s.max) { s.max = v[i]; s.maxAt = count_; }
    }
    if (count_ < UINT16_MAX) count_++;
  }

  uint16_t count() const { return count_; }
  uint32_t firstEpoch() const { return firstEpoch_; }
  uint32_t lastEpoch() const { return lastEpoch_; }
  uint8_t flags() const { return flags_; }

  // Valore della serie i nel primo (second=false) o nel secondo punto del gruppo.
  int32_t value(uint8_t i, bool second) const {
    const Serie& s = s_[i];
    if (!s.has) return kMissing;
    const bool minPrima = s.minAt <= s.maxAt;
    if (!second) return minPrima ? s.min : s.max;
    return minPrima ? s.max : s.min;
  }

 private:
  struct Serie {
    bool has = false;
    int32_t min = 0, max = 0;
    uint16_t minAt = 0, maxAt = 0;
  };
  Serie s_[N];
  uint16_t count_ = 0;
  uint32_t firstEpoch_ = 0, lastEpoch_ = 0;
  uint8_t flags_ = 0;
};

}  // namespace ax
