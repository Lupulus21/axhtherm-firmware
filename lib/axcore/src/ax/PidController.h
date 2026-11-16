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

  // Traduce il duty in ON/OFF sulla finestra corrente. nowMs può essere qualsiasi
  // base monotona (millis); on/off minimi di 1 s per non stressare il relè.
  // Al wrap di millis() la fase salta di 2^32 mod windowMs (17.296 ms col default di
  // 30 s): una finestra un po' più corta ogni 49 giorni, innocua.
  bool windowOutput(uint32_t nowMs) const {
    if (duty_ <= 1.0f) return false;
    if (duty_ >= 99.0f) return true;
    uint32_t phase = nowMs % cfg_.windowMs;
    uint32_t onMs = (uint32_t)(duty_ / 100.0f * (float)cfg_.windowMs);
    if (onMs < 1000) return false;
    if (cfg_.windowMs - onMs < 1000) return true;
    return phase < onMs;
  }

  float duty() const { return duty_; }
  void reset() { integral_ = 0.0f; duty_ = 0.0f; hasLast_ = false; }

 private:
  Cfg cfg_;
  float integral_ = 0.0f;
  float duty_ = 0.0f;
  float lastTemp_ = 0.0f;
  bool hasLast_ = false;
};

}  // namespace ax
