#pragma once
#include <stdint.h>

namespace ax {

// Spegnimento predittivo in stile BrewPi: mentre l'attuatore è acceso si stima di
// quanto la temperatura continuerà a derivare DOPO lo spegnimento (inerzia termica
// del frigo o del riscaldatore) e si spegne in anticipo, così che la deriva residua
// "atterri" sul setpoint invece di superarlo. Il coefficiente di deriva (°C per ora
// di marcia continuativa) non si configura: si impara osservando il picco reale
// raggiunto dopo ogni ciclo (media mobile esponenziale sulla deriva misurata).
//
// Paletti sull'apprendimento (un ciclo = al massimo una lezione, mai una rivoluzione):
// il campione osservato è clampato a kMaxCoeffCph prima di entrare nell'EMA e il
// coefficiente si muove al più di kMaxStepCph per ciclo — servono molti cicli
// concordi per arrivare agli estremi. Un'osservazione chiusa prima di
// kMinObserveSec (da inversione — che può essere solo rumore di sonda — o da una
// nuova marcia) non insegna; una contaminata dall'attuatore opposto
// (cancelObservation) non insegna mai; il timeout insegna sempre.
//
// Proprietà di sicurezza: può solo ANTICIPARE uno spegnimento già deciso a monte.
// Mai tenere acceso più a lungo, mai accendere. Il fallimento peggiore è un ciclo
// che si ferma presto e riparte dalla soglia d'isteresi, sempre filtrato dalla
// guardia compressore. Con coefficiente 0 (stato iniziale) è del tutto inerte.
class PeakEstimator {
 public:
  static constexpr float kAlpha = 0.4f;         // peso della nuova osservazione
  static constexpr float kMaxCoeffCph = 10.0f;  // °C/h: clamp del coefficiente E del campione
  static constexpr float kMaxStepCph = 1.0f;    // variazione massima per singolo ciclo
  static constexpr float kMaxDriftC = 2.0f;     // deriva prevista massima applicata
  static constexpr float kMinRunSec = 120.0f;   // cicli più corti: né stop né lezione
  static constexpr float kMinObserveSec = 180.0f;  // osservazione interrotta prima: scartata
  static constexpr float kReverseBandC = 0.1f;  // inversione che chiude l'osservazione
  static constexpr float kMaxObserveSec = 1800.0f;

  void configureSide(bool cooling) { cooling_ = cooling; }

  float coefficientCph() const { return coeffCph_; }
  void setCoefficientCph(float c) { coeffCph_ = clampCoeff(c); }

  // Deriva prevista alla marcia attuale (0 se fermo o appena partito).
  float predictedDriftC() const {
    float d = coeffCph_ * (runSec_ / 3600.0f);
    return d > kMaxDriftC ? kMaxDriftC : d;
  }

  // Un passo del loop di controllo, SOLO con temperatura valida. `active` è lo stato
  // fisico del lato al tick precedente. Ritorna true se il lato è in marcia e
  // conviene spegnere ORA (atterraggio previsto sul setpoint).
  bool tick(bool active, float tempC, float setpointC, float dtSec) {
    if (active) {
      if (!wasActive_) {
        finishObservation(false);  // chiusa da una nuova marcia: vale solo se lunga
        runSec_ = 0.0f;
        wasActive_ = true;
      }
      runSec_ += dtSec;
      if (runSec_ < kMinRunSec || coeffCph_ <= 0.0f) return false;
      const float drift = predictedDriftC();
      return cooling_ ? (tempC - drift <= setpointC) : (tempC + drift >= setpointC);
    }
    if (wasActive_) {
      wasActive_ = false;
      if (runSec_ >= kMinRunSec) {
        observing_ = true;
        offTemp_ = extremeTemp_ = tempC;
        observeSec_ = 0.0f;
        obsRunSec_ = runSec_;
      }
    } else if (observing_) {
      observeSec_ += dtSec;
      if (cooling_ ? (tempC < extremeTemp_) : (tempC > extremeTemp_)) extremeTemp_ = tempC;
      const float reversal = cooling_ ? (tempC - extremeTemp_) : (extremeTemp_ - tempC);
      // L'inversione NON è "fidata" di per sé: 2 LSB di rumore DS18B20 (0.125 °C)
      // bastano a superare la banda pochi secondi dopo lo stop, e imparerebbero
      // una deriva ~0 erodendo il coefficiente. Vale il gate di durata minima.
      if (reversal >= kReverseBandC) finishObservation(false);
      else if (observeSec_ >= kMaxObserveSec) finishObservation(true);
    }
    return false;
  }

  // L'attuatore opposto è entrato in funzione: la deriva non è più passiva e il
  // campione in corso non insegna nulla di affidabile.
  void cancelObservation() { observing_ = false; }

  // Fault/disarmo: lo stato del ciclo non è più rappresentativo (il coefficiente
  // appreso invece resta).
  void reset() {
    wasActive_ = observing_ = false;
    runSec_ = obsRunSec_ = observeSec_ = 0.0f;
  }

 private:
  // `fullWindow` = chiusa dal timeout (deriva osservata per l'intera finestra);
  // false = chiusa presto da inversione o nuova marcia: insegna solo se è durata
  // almeno kMinObserveSec (un'inversione dopo pochi secondi è rumore, non fisica).
  void finishObservation(bool fullWindow) {
    if (!observing_) return;
    observing_ = false;
    if (!fullWindow && observeSec_ < kMinObserveSec) return;
    const float drift = cooling_ ? (offTemp_ - extremeTemp_) : (extremeTemp_ - offTemp_);
    if (drift < 0.0f || obsRunSec_ < kMinRunSec) return;
    const float obsCph = clampCoeff(drift / (obsRunSec_ / 3600.0f));
    float step = kAlpha * (obsCph - coeffCph_);
    if (step > kMaxStepCph) step = kMaxStepCph;
    if (step < -kMaxStepCph) step = -kMaxStepCph;
    coeffCph_ = clampCoeff(coeffCph_ + step);
  }

  static float clampCoeff(float c) {
    if (c < 0.0f) return 0.0f;
    return c > kMaxCoeffCph ? kMaxCoeffCph : c;
  }

  bool cooling_ = true;
  bool wasActive_ = false, observing_ = false;
  float runSec_ = 0.0f, obsRunSec_ = 0.0f, observeSec_ = 0.0f;
  float offTemp_ = 0.0f, extremeTemp_ = 0.0f;
  float coeffCph_ = 0.0f;  // 0 = mai appreso: comportamento identico all'isteresi pura
};

}  // namespace ax
