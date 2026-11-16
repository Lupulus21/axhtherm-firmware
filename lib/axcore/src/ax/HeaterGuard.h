#pragma once
#include <stdint.h>

namespace ax {

// Motivo del blocco del caldo. Il valore numerico e' persistito in /state.json: non
// riordinare, aggiungere solo in fondo.
enum class HeatLock : uint8_t {
  None = 0,
  ProbeStuck = 1,       // caldo acceso di continuo e la sonda di controllo non sale
  HeatTimeout = 2,      // caldo acceso di continuo oltre il tempo massimo
  RiseHeatOff = 3,      // temperatura in salita rapida a caldo spento: relè/SSR incollato?
  CoolIneffective = 4,  // freddo acceso a lungo, caldo spento, temperatura che non scende
};

// Rilevatori di guasto del riscaldatore. Logica pura: decide QUANDO bloccare; il blocco
// vero (latch salvato su flash, riarmo solo manuale) lo tiene l'applicazione, che toglie
// il consenso al caldo e lascia consentito il freddo.
//
// Perché servono: la regolazione si fida della sonda e del relè. Una sonda che legge un
// valore congelato sotto il setpoint porta il PID al 100% per sempre; un SSR guasto in
// conduzione (il guasto tipico degli SSR) scalda a relè comandato spento. In entrambi i casi
// nessun allarme esistente scatta finché lo scarto resta sotto i 5 °C. Questi rilevatori
// tolgono il comando al caldo, ma nessun firmware puo' aprire un contatto incollato: senza
// una protezione indipendente sul circuito del riscaldatore (scelta dell'utente, 28/10/2026)
// quel caso resta scoperto, e conta l'allarme che fa intervenire una persona.
//
// I tempi sono ACCUMULATI (dtSec dei tick), non differenze di millis(): immuni al wrap e a
// un loop rallentato.
class HeaterGuard {
 public:
  struct Cfg {
    uint32_t maxOnSec = 12UL * 3600UL;  // caldo continuo oltre questo -> HeatTimeout (0 = mai)
    uint32_t stuckSec = 3600;           // finestra della sonda ferma (0 = rilevatore spento)
    float stuckDeltaC = 0.2f;           // salita minima attesa in quella finestra a caldo pieno
    uint32_t settleSec = 600;           // dopo lo spegnimento: il riscaldatore rilascia calore
    uint32_t riseWindowSec = 1200;      // finestra della salita a caldo spento
    float riseC = 1.0f;                 // salita che il mosto non fa da solo in quella finestra
    uint32_t coolWindowSec = 7200;      // finestra del freddo inefficace
    float coolDropC = 0.3f;             // discesa minima attesa col freddo acceso
    // Solo se la temperatura e' ben sopra il setpoint: 2 °C tengono fuori il cold crash a
    // -1 °C con un frigo al limite, che si avvicina al target in modo asintotico.
    float coolAboveSpC = 2.0f;
    // Un salto oltre questa soglia fra due tick non e' un riscaldatore (che produce una
    // rampa) ma la sonda spostata, un offset cambiato o una sonda sostituita: la finestra
    // della salita a caldo spento riparte invece di scattare.
    float jumpC = 0.5f;
  };

  void configure(const Cfg& c) { cfg_ = c; }
  void reset() {
    heatOnSec_ = stuckWinSec_ = offSec_ = riseWinSec_ = coolWinSec_ = 0.0f;
    stuckRefT_ = riseMinT_ = coolRefT_ = 0.0f;
    hasLastT_ = false;
  }

  // active: camera armata e uscite consentite. Fuori da quel contesto i rilevatori non
  // hanno senso (le uscite sono spente per altri motivi) e si azzerano.
  // heatOn/coolOn: stato EFFETTIVO dei relè dopo pipeline, guardia e interlock.
  HeatLock tick(float dtSec, bool active, bool tempValid, float tempC, bool hasSetpoint,
                float setpointC, bool heatOn, bool coolOn) {
    if (!active) { reset(); return HeatLock::None; }
    // Sonda non valida: si CONGELA (non si accumula e non si azzera). Una sonda guasta
    // toglie gia' il consenso alle uscite; uno sfarfallio non deve buttare il tempo maturato.
    // Per questo `active` NON deve dipendere dalla validita' della sonda.
    if (!tempValid) return HeatLock::None;
    const bool salto = hasLastT_ && (tempC - lastT_ > cfg_.jumpC || lastT_ - tempC > cfg_.jumpC);
    lastT_ = tempC;
    hasLastT_ = true;

    if (heatOn) {
      offSec_ = riseWinSec_ = coolWinSec_ = 0.0f;
      if (heatOnSec_ == 0.0f) { stuckRefT_ = tempC; stuckWinSec_ = 0.0f; }
      heatOnSec_ += dtSec;
      if (cfg_.maxOnSec && heatOnSec_ >= (float)cfg_.maxOnSec) return HeatLock::HeatTimeout;
      if (cfg_.stuckSec) {
        stuckWinSec_ += dtSec;
        if (stuckWinSec_ >= (float)cfg_.stuckSec) {
          if (tempC - stuckRefT_ < cfg_.stuckDeltaC) return HeatLock::ProbeStuck;
          stuckRefT_ = tempC;  // sale davvero: nuova finestra dalla temperatura attuale
          stuckWinSec_ = 0.0f;
        }
      }
      return HeatLock::None;
    }

    // Caldo spento.
    heatOnSec_ = stuckWinSec_ = 0.0f;
    offSec_ += dtSec;
    if (offSec_ >= (float)cfg_.settleSec) {
      if (salto) riseWinSec_ = 0.0f;  // sonda spostata o offset cambiato: si riparte da qui
      if (riseWinSec_ == 0.0f || tempC < riseMinT_) riseMinT_ = tempC;
      riseWinSec_ += dtSec;
      if (tempC - riseMinT_ >= cfg_.riseC) return HeatLock::RiseHeatOff;
      if (riseWinSec_ >= (float)cfg_.riseWindowSec) riseWinSec_ = 0.0f;  // nuova finestra
    }
    if (coolOn && hasSetpoint && tempC > setpointC + cfg_.coolAboveSpC) {
      if (coolWinSec_ == 0.0f) coolRefT_ = tempC;
      coolWinSec_ += dtSec;
      if (coolWinSec_ >= (float)cfg_.coolWindowSec) {
        if (coolRefT_ - tempC < cfg_.coolDropC) return HeatLock::CoolIneffective;
        coolRefT_ = tempC;  // scende: nuova finestra
        coolWinSec_ = 0.0f;
      }
    } else {
      coolWinSec_ = 0.0f;
    }
    return HeatLock::None;
  }

 private:
  Cfg cfg_;
  float heatOnSec_ = 0.0f, stuckWinSec_ = 0.0f, stuckRefT_ = 0.0f;
  float offSec_ = 0.0f, riseWinSec_ = 0.0f, riseMinT_ = 0.0f;
  float coolWinSec_ = 0.0f, coolRefT_ = 0.0f;
  float lastT_ = 0.0f;
  bool hasLastT_ = false;
};

}  // namespace ax
