#pragma once
#include <stdint.h>

namespace ax {

// Limiti sull'aria della camera (sonda "camera"), con isteresi.
// - Freddo sospeso se l'aria scende sotto minAirC: nel cold crash evita che il mosto vicino
//   alle pareti geli (default -5 °C, compatibile con il cold crash a -1; -10 = nessun minimo).
//   Facoltativo anche "setpoint - belowSpC" (default 0 = spento): limita il pull-down, a costo
//   di renderlo piu' lento e di far ciclare il compressore sull'aria.
// - Caldo sospeso se l'aria sale oltre setpoint + aboveSpC: un cavo scaldante in aria non
//   deve cuocere la camera mentre il mosto, piu' lento, insegue il target.
// L'isteresi e' ampia (3 °C) apposta: l'aria cambia in fretta, e un margine stretto farebbe
// ripartire il compressore ogni pochi minuti (lo frena solo la sua guardia).
// Sonda della camera non valida: limiti inattivi (il controllo non deve fermarsi per una
// sonda ausiliaria), e l'applicazione lo segnala.
class AirLimit {
 public:
  struct Cfg {
    float minAirC = -5.0f;   // minimo assoluto dell'aria col freddo (-10 o meno = nessuno)
    float belowSpC = 0.0f;   // aria al massimo cosi' sotto il setpoint (0 = nessun limite)
    float aboveSpC = 10.0f;  // aria al massimo cosi' sopra il setpoint (0 = nessun limite)
    float hystC = 3.0f;      // si riparte con questo margine
  };

  void configure(const Cfg& c) { cfg_ = c; }
  void reset() { coolBlk_ = heatBlk_ = false; }

  void tick(bool hasSetpoint, float setpointC, bool airValid, float airC) {
    if (!hasSetpoint || !airValid) { reset(); return; }
    float minAria = cfg_.minAirC > -10.0f ? cfg_.minAirC : -1000.0f;
    if (cfg_.belowSpC > 0.0f && setpointC - cfg_.belowSpC > minAria)
      minAria = setpointC - cfg_.belowSpC;
    if (!coolBlk_ && airC < minAria) coolBlk_ = true;
    else if (coolBlk_ && airC > minAria + cfg_.hystC) coolBlk_ = false;
    if (cfg_.aboveSpC > 0.0f) {
      const float maxAria = setpointC + cfg_.aboveSpC;
      if (!heatBlk_ && airC > maxAria) heatBlk_ = true;
      else if (heatBlk_ && airC < maxAria - cfg_.hystC) heatBlk_ = false;
    } else {
      heatBlk_ = false;
    }
  }

  bool coolBlocked() const { return coolBlk_; }
  bool heatBlocked() const { return heatBlk_; }

 private:
  Cfg cfg_;
  bool coolBlk_ = false, heatBlk_ = false;
};

}  // namespace ax
