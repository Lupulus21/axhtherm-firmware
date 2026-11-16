#pragma once
#include <stdint.h>

namespace ax {

// Ventola della camera (3.17.0, CT-22/PH-V02). Decide QUANDO accenderla; il relè lo scrive
// OutputBus. Tempi in secondi MONOTONI (durata della sessione), non in epoch: un salto
// d'orologio non deve accendere o spegnere la ventola.
//
// Modo «segue» (default): accesa mentre caldo o freddo lavorano, piu' un post-funzionamento
// (l'aria fredda dell'evaporatore o il calore del cavo arrivano anche al mosto), e un
// ricircolo opzionale: almeno cycleOnSec ogni cycleEverySec, contando anche le accensioni
// per caldo e freddo. Modo «sempre»: accesa finche' la regolazione e' attiva.
// Spenta quando la regolazione non e' attiva (scheda disarmata o ferma).
// I cambi sono di minuti: niente lampeggi sui GPIO dei relè.
class FanControl {
 public:
  enum : uint8_t { kSegue = 0, kSempre = 1 };
  struct Cfg {
    uint8_t mode = kSegue;
    uint32_t postRunSec = 120;
    uint32_t cycleOnSec = 0;     // 0 = niente ricircolo
    uint32_t cycleEverySec = 0;
  };

  void configure(const Cfg& c) { c_ = c; }

  bool tick(uint32_t now, bool active, bool heat, bool cool) {
    if (!active) {
      init_ = false;
      ranDemand_ = false;
      cycleUntil_ = 0;
      return false;
    }
    if (!init_) {
      init_ = true;
      lastOn_ = now;  // il primo ricircolo arriva dopo un periodo intero
    }
    bool on = false;
    if (c_.mode == kSempre) {
      on = true;
    } else if (heat || cool) {
      on = true;
      ranDemand_ = true;
      lastDemand_ = now;
    } else if (ranDemand_ && now - lastDemand_ < c_.postRunSec) {
      on = true;
    } else if (cycleUntil_ && (int32_t)(now - cycleUntil_) < 0) {
      on = true;
    } else {
      cycleUntil_ = 0;
      if (c_.cycleOnSec && c_.cycleEverySec > c_.cycleOnSec &&
          now - lastOn_ >= c_.cycleEverySec - c_.cycleOnSec) {
        cycleUntil_ = now + c_.cycleOnSec;
        on = true;
      }
    }
    if (on) lastOn_ = now;
    return on;
  }

 private:
  Cfg c_;
  bool init_ = false, ranDemand_ = false;
  uint32_t lastDemand_ = 0, lastOn_ = 0, cycleUntil_ = 0;
};

}  // namespace ax
