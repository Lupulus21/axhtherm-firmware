#pragma once
#include <stdint.h>

// Salute del loop di controllo dall'avvio. Il tick e' di 1 s: un ritardo vuol dire che
// qualcosa (un handler web, MQTT, la flash) ha tenuto fermo il controllo. Gli stalli
// lunghi li ferma il failsafe hardware (60 s); questi contatori rendono visibili quelli
// piu' brevi, che altrimenti comparivano solo sul seriale.
struct LoopStats {
  static constexpr uint32_t kLateMs = 2000;  // tick oltre 2 s: in ritardo
  uint32_t late = 0;           // tick in ritardo
  uint32_t maxLateMs = 0;      // ritardo massimo oltre il secondo previsto
  uint32_t lastLateMs = 0;     // ultimo ritardo
  uint32_t lastLateEpoch = 0;  // quando
  uint32_t kicks = 0;          // rinvii del failsafe da percorsi lunghi legittimi (export, OTA, discovery)
  uint32_t legitLong = 0;      // tick lunghi dovuti a quei percorsi: non sono stalli

  // Un tick lungo durante il quale il failsafe e' stato rinviato era un'operazione lecita
  // che avanzava (export, OTA, discovery MQTT): si conta a parte, non come ritardo, cosi'
  // il massimo misura solo gli stalli veri.
  void tick(uint32_t dtMs, uint32_t nowEpoch) {
    const bool lecito = kicks != kicksAtLastTick_;
    kicksAtLastTick_ = kicks;
    if (dtMs <= kLateMs) return;
    if (lecito) { legitLong++; return; }
    late++;
    lastLateMs = dtMs - 1000;
    lastLateEpoch = nowEpoch;
    if (lastLateMs > maxLateMs) maxLateMs = lastLateMs;
  }

 private:
  uint32_t kicksAtLastTick_ = 0;
};
