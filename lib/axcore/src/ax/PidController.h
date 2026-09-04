#pragma once
#include <stdint.h>

namespace ax {

// PID per il riscaldamento (carico resistivo) con uscita 0..100% applicata a finestra
// di time-proportioning. Semantica delle soglie come nel firmware originale (asimmetrica):
// errore >= fullPowerBelow -> piena potenza; errore <= -cutAbove -> tutto spento.
class PidController {
 public:
  struct Cfg {
    float kp = 2.0f, ki = 1.0f, kd = 5.0f;
    uint32_t windowMs = 30000;
    float fullPowerBelow = 5.0f;  // gradi sotto il set point
    float cutAbove = 0.5f;        // gradi sopra il set point
  };

  void configure(const Cfg& c) { cfg_ = c; if (cfg_.windowMs < 2000) cfg_.windowMs = 2000; }

  // Da chiamare a periodo fisso (dtSec, tipicamente 1 s). Ritorna il duty 0..100.
  float update(float setpointC, float tempC, float dtSec) {
    const float e = setpointC - tempC;
    if (e >= cfg_.fullPowerBelow) { duty_ = 100.0f; lastTemp_ = tempC; hasLast_ = true; return duty_; }
    if (e <= -cfg_.cutAbove)      { duty_ = 0.0f; integral_ = 0.0f; lastTemp_ = tempC; hasLast_ = true; return duty_; }

    integral_ += cfg_.ki * e * dtSec;
    if (integral_ > 100.0f) integral_ = 100.0f;
    if (integral_ < 0.0f) integral_ = 0.0f;
    const float deriv = hasLast_ ? -(tempC - lastTemp_) / dtSec : 0.0f;  // derivata sulla misura
    lastTemp_ = tempC;
    hasLast_ = true;
    float out = cfg_.kp * e + integral_ + cfg_.kd * deriv;
    if (out > 100.0f) out = 100.0f;
    if (out < 0.0f) out = 0.0f;
    duty_ = out;
    return duty_;
  }

  // Sfasamento della finestra di time-proportioning, espresso come FRAZIONE idx/n.
  // Serve a impedire che più camere accendano il proprio riscaldante nello stesso
  // istante: la fase è derivata da millis(), che è globale, quindi due istanze con lo
  // stesso windowMs sarebbero altrimenti perfettamente sincrone e sommerebbero i loro
  // spunti di corrente sullo stesso alimentatore a ogni finestra.
  // La frazione (non un offset assoluto in ms) mantiene le camere equidistanti anche se
  // windowMs cambia da configurazione. idx=0/n=1 => offset 0 => comportamento invariato.
  //
  // LIMITE INTRINSECO, da conoscere: lo sfasamento agisce solo a duty PARZIALE. Ai due
  // estremi windowOutput ignora la fase per costruzione e ritorna sempre ON. La soglia
  // che conta NON e' duty >= 99 ma la guardia sul minimo tempo di OFF:
  // windowMs - onMs < 1000, cioe' duty > (1 - 1000/windowMs) * 100 — col default
  // windowMs = 30000 significa gia' da duty > ~96,7%. Sopra quella soglia due camere
  // restano accese INSIEME, ed e' fisicamente inevitabile: non si multiplexa il 100%. Lo sfasamento riduce la sovrapposizione nel funzionamento
  // normale, NON esime dal dimensionare l'alimentatore per tutti i carichi contemporanei
  // (vedi docs/analysis/feasibility-2ch.md, margine dell'alimentatore).
  void setPhaseFraction(uint8_t idx, uint8_t n) {
    phaseNum_ = n ? (idx % n) : 0;
    phaseDen_ = n ? n : 1;
  }

  // Traduce il duty in ON/OFF sulla finestra corrente. nowMs può essere qualsiasi
  // base monotona (millis); on/off minimi di 1 s per non stressare il relè.
  bool windowOutput(uint32_t nowMs) const {
    if (duty_ <= 1.0f) return false;
    if (duty_ >= 99.0f) return true;
    // uint64 nel prodotto: windowMs non ha tetto superiore in configurazione (a differenza
    // di minStateSec/minSwitchSec/compressorMinOnSec, capped a 1800 s in ConfigStore), quindi
    // windowMs * phaseNum_ potrebbe traboccare a 32 bit con valori estremi.
    //
    // La somma (nowMs + offset) SI wrappa a 32 bit, ed e' benigno. Verificato:
    // (a) la discontinuita' della fase al wrap di millis() esiste GIA' nella baseline
    //     (nowMs % windowMs): vale 2^32 mod windowMs, cioe' 17.296 ms col default di
    //     30 s. L'offset non la crea e non la peggiora;
    // (b) cio' che conta e' lo sfasamento RELATIVO fra camere, ed e' INVARIANTE al wrap:
    //     tutte le fasi saltano della stessa quantita', quindi la desincronizzazione non
    //     si perde mai (misurato: scostamento 0 ms attraverso il wrap).
    const uint32_t offset =
        phaseDen_ > 1
            ? (uint32_t)((uint64_t)cfg_.windowMs * phaseNum_ / phaseDen_)
            : 0u;
    uint32_t phase = (nowMs + offset) % cfg_.windowMs;
    uint32_t onMs = (uint32_t)(duty_ / 100.0f * (float)cfg_.windowMs);
    if (onMs < 1000) return false;
    if (cfg_.windowMs - onMs < 1000) return true;
    return phase < onMs;
  }

  float duty() const { return duty_; }
  void reset() { integral_ = 0.0f; duty_ = 0.0f; hasLast_ = false; }

 private:
  Cfg cfg_;
  uint8_t phaseNum_ = 0, phaseDen_ = 1;  // sfasamento finestra = windowMs * num/den
  float integral_ = 0.0f;
  float duty_ = 0.0f;
  float lastTemp_ = 0.0f;
  bool hasLast_ = false;
};

}  // namespace ax
