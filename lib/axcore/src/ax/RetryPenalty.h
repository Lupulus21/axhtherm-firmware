#pragma once
#include <stdint.h>

namespace ax {

// Penalita' sui tentativi sbagliati (password, codice dei densimetri). Al posto della pausa
// fissa dentro la richiesta, che fermava anche il loop di controllo: dal kFree-esimo
// errore di fila ogni nuovo tentativo e' rifiutato SUBITO per una pausa che raddoppia a
// ogni errore (2, 4, 8, 16, 32 s, poi 60 s). I tentativi rifiutati durante la pausa non
// contano e non la allungano. Si azzera con un tentativo riuscito o dopo kForgetMs senza
// errori. Tempi in millis(), confronti per differenza: immuni al wrap.
class RetryPenalty {
 public:
  static constexpr uint8_t kFree = 5;              // errori concessi prima della pausa
  static constexpr uint32_t kMaxPauseMs = 60000;
  static constexpr uint32_t kForgetMs = 15UL * 60UL * 1000UL;

  // true = in pausa: rifiutare senza controllare.
  bool locked(uint32_t nowMs) {
    forget(nowMs);
    return fails_ >= kFree && (int32_t)(untilMs_ - nowMs) > 0;
  }
  // Secondi di pausa rimasti (arrotondati per eccesso), 0 se libero.
  uint32_t waitSec(uint32_t nowMs) const {
    const int32_t d = (int32_t)(untilMs_ - nowMs);
    return (fails_ >= kFree && d > 0) ? ((uint32_t)d + 999) / 1000 : 0;
  }
  void fail(uint32_t nowMs) {
    forget(nowMs);
    if (fails_ < 255) fails_++;
    lastFailMs_ = nowMs;
    if (fails_ >= kFree) {
      const uint8_t k = (uint8_t)(fails_ - kFree);
      const uint32_t ms = k >= 5 ? kMaxPauseMs : (2000UL << k);
      untilMs_ = nowMs + ms;
    }
  }
  void success() { fails_ = 0; }
  // Nessun errore che conti ancora: il posto si puo' riusare.
  bool idle(uint32_t nowMs) {
    forget(nowMs);
    return fails_ == 0;
  }
  uint32_t lastFailMs() const { return lastFailMs_; }

 private:
  void forget(uint32_t nowMs) {
    if (fails_ && nowMs - lastFailMs_ > kForgetMs) fails_ = 0;
  }
  uint8_t fails_ = 0;
  uint32_t lastFailMs_ = 0, untilMs_ = 0;
};

// Una penalita' per chiave (l'indirizzo IP di chi prova): uno script con la password
// vecchia, o un altro dispositivo, non deve chiudere fuori il proprietario che entra da un
// altro indirizzo. N posti; quando sono pieni si riusa quello inattivo o, se tutti sono
// attivi, quello con l'errore piu' vecchio. Chi controlla piu' di N indirizzi sulla LAN
// puo' quindi alternarli: e' fuori dal modello di minaccia (rete di casa).
template <uint8_t N>
class RetryPenaltyByKey {
 public:
  bool locked(uint32_t key, uint32_t nowMs) {
    RetryPenalty* p = find(key);
    return p && p->locked(nowMs);
  }
  uint32_t waitSec(uint32_t key, uint32_t nowMs) {
    RetryPenalty* p = find(key);
    return p ? p->waitSec(nowMs) : 0;
  }
  void success(uint32_t key) {
    RetryPenalty* p = find(key);
    if (p) p->success();
  }
  void fail(uint32_t key, uint32_t nowMs) {
    RetryPenalty* p = find(key);
    if (!p) {
      uint8_t pos = 0;
      for (uint8_t i = 0; i < N; i++) {
        if (!used_[i] || slot_[i].idle(nowMs)) { pos = i; break; }
        if (nowMs - slot_[i].lastFailMs() > nowMs - slot_[pos].lastFailMs()) pos = i;
      }
      used_[pos] = true;
      key_[pos] = key;
      slot_[pos] = RetryPenalty{};
      p = &slot_[pos];
    }
    p->fail(nowMs);
  }

 private:
  RetryPenalty* find(uint32_t key) {
    for (uint8_t i = 0; i < N; i++)
      if (used_[i] && key_[i] == key) return &slot_[i];
    return nullptr;
  }
  RetryPenalty slot_[N];
  uint32_t key_[N] = {0};
  bool used_[N] = {false};
};

}  // namespace ax
