#pragma once
#include <stdint.h>

namespace ax {

// Freddo non comandato: il relè del frigo incollato. SOLO ALLARME, nessun blocco: con il
// frigo che non si ferma il riscaldatore e' l'unico contrasto al gelo, e il firmware non
// puo' aprire un relè saldato. Serve a far intervenire una persona.
//
// Il discriminante: con il freddo comandato spento, solo un frigo che gira tiene l'aria
// della camera ben piu' fredda sia del mosto sia del setpoint sia (se c'e' la sonda) della
// stanza. Riferimento = min(setpoint, mosto): con il mosto sotto il setpoint (inoculo
// freddo, step di riscaldamento) conta il mosto, a cui l'aria di un frigo fermo si avvicina.
// Senza setpoint (modo fermo) conta il mosto da solo: un relè che si incolla a fine cotta,
// con la birra ancora in camera, va segnalato lo stesso.
//
// Due firme, con il freddo COMANDATO spento da almeno settleSec:
// 1. dopo uno spegnimento VERO (il freddo girava poco fa) l'aria NON e' risalita: dopo uno
//    stop vero il mosto e le pareti la riscaldano di qualche grado in pochi minuti.
//    Valutata una volta per spegnimento, al passaggio dei settleSec. E' il guasto tipico,
//    il relè che si incolla mentre il frigo gira;
// 2. l'aria crolla di dropC in windowSec (finestra scorrevole a passi di 30 s): il relè che
//    si chiude da solo a frigo fermo. Serve la sonda ambiente.
// L'allarme si spegne quando il freddo torna comandato o quando l'aria risale vicino al
// riferimento: un relè davvero incollato la tiene fredda, un falso allarme si smentisce.
class CoolWatch {
 public:
  struct Cfg {
    uint32_t settleSec = 600;   // freddo comandato spento da almeno 10 minuti
    float notRisenC = 1.0f;     // firma 1: aria risalita meno di cosi' dallo spegnimento
    uint32_t windowSec = 300;   // firma 2: finestra del crollo
    float dropC = 3.0f;
    float belowRefC = 2.0f;     // aria almeno cosi' sotto min(setpoint, mosto)
    float belowAmbC = 1.0f;     // e, se c'e' la sonda ambiente, sotto la stanza
    float clearC = 1.0f;        // l'allarme si spegne con l'aria risalita a ref - belowRefC + clearC
  };

  void configure(const Cfg& c) { cfg_ = c; }
  void reset() {
    offSec_ = 0.0f;
    hasAirAtOff_ = alarm_ = hasLastAirOn_ = false;
    resetWindow();
  }

  // coolOn: freddo COMANDATO (stato effettivo dopo pipeline e guardia). Ritorna l'allarme.
  bool tick(float dtSec, bool active, bool coolOn, bool hasSetpoint, float setpointC,
            bool wortValid, float wortC, bool airValid, float airC, bool ambValid, float ambC) {
    if (!active) { reset(); return false; }
    if (coolOn) {
      offSec_ = 0.0f;
      hasAirAtOff_ = alarm_ = false;
      resetWindow();
      if (airValid) { lastAirOn_ = airC; hasLastAirOn_ = true; }
      return false;
    }
    // Sonda dell'aria non valida: si congela, e la finestra del crollo riparte (un massimo
    // vecchio, dopo un buco, non va confrontato con l'aria di adesso).
    if (!airValid) { resetWindow(); return alarm_; }
    if (!hasSetpoint && !wortValid) return alarm_;  // nessun riferimento: congelato
    float ref = hasSetpoint ? setpointC : wortC;
    if (wortValid && wortC < ref) ref = wortC;
    const float soglia = ref - cfg_.belowRefC;
    const bool benSotto = airC < soglia && (!ambValid || airC < ambC - cfg_.belowAmbC);

    if (!hasAirAtOff_) {
      // Spegnimento VERO solo se il freddo era acceso poco fa: la firma 1 vale solo allora.
      stopVero_ = hasLastAirOn_;
      airAtOff_ = lastAirOn_;
      hasAirAtOff_ = true;
      hasLastAirOn_ = false;
    }
    const float prima = offSec_;
    offSec_ += dtSec;
    if (alarm_ && airC >= soglia + cfg_.clearC) alarm_ = false;
    if (offSec_ < (float)cfg_.settleSec) return alarm_;

    // Firma 1: una sola valutazione, al passaggio dei settleSec.
    if (stopVero_ && prima < (float)cfg_.settleSec && benSotto &&
        airC <= airAtOff_ + cfg_.notRisenC)
      alarm_ = true;

    // Firma 2: massimo dell'aria negli ultimi windowSec, a passi di kStepSec.
    if (nStep_ == 0) { stepMax_[0] = airC; nStep_ = 1; stepSec_ = 0.0f; }
    if (airC > stepMax_[head_]) stepMax_[head_] = airC;
    stepSec_ += dtSec;
    float maxFin = stepMax_[0];
    for (uint8_t k = 1; k < nStep_; k++) if (stepMax_[k] > maxFin) maxFin = stepMax_[k];
    if (benSotto && ambValid && maxFin - airC >= cfg_.dropC) alarm_ = true;
    if (stepSec_ >= kStepSec) {
      const uint8_t passi = stepsInWindow();
      head_ = (uint8_t)((head_ + 1) % passi);
      stepMax_[head_] = airC;
      if (nStep_ < passi) nStep_++;
      stepSec_ = 0.0f;
    }
    return alarm_;
  }

  bool alarm() const { return alarm_; }

 private:
  static constexpr float kStepSec = 30.0f;
  static constexpr uint8_t kMaxSteps = 20;  // fino a 10 minuti di finestra
  uint8_t stepsInWindow() const {
    const uint32_t n = cfg_.windowSec / (uint32_t)kStepSec;
    return n < 1 ? 1 : (n > kMaxSteps ? kMaxSteps : (uint8_t)n);
  }
  void resetWindow() { nStep_ = head_ = 0; stepSec_ = 0.0f; }

  Cfg cfg_;
  float offSec_ = 0.0f, airAtOff_ = 0.0f, lastAirOn_ = 0.0f;
  float stepMax_[kMaxSteps] = {};
  float stepSec_ = 0.0f;
  uint8_t nStep_ = 0, head_ = 0;
  bool hasAirAtOff_ = false, hasLastAirOn_ = false, alarm_ = false, stopVero_ = false;
};

}  // namespace ax
