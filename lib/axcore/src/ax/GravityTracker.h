#pragma once
#include <stdint.h>

namespace ax {

// Ultima lettura valida del densimetro (iSpindel) con timestamp assoluto.
// Una lettura più vecchia della soglia è "stantia": allarme e mai avanzamento di step.
class GravityTracker {
 public:
  void configure(uint32_t staleSec, float correction) {
    staleSec_ = staleSec;
    correction_ = correction;
  }

  void update(float gravity, float battery, float temperature, uint32_t epoch,
              float angle = 0, int rssi = 0, uint32_t deviceId = 0) {
    deviceId_ = deviceId;
    raw_ = gravity;  // valore come arrivato: la calibrazione va fatta sul grezzo
    gravity_ = gravity + correction_;
    battery_ = battery;
    temperature_ = temperature;
    angle_ = angle;
    rssi_ = rssi;
    updatedEpoch_ = epoch;
    hasValue_ = true;
  }

  bool hasValue() const { return hasValue_; }
  float gravity() const { return gravity_; }
  float rawGravity() const { return raw_; }
  float battery() const { return battery_; }
  float temperature() const { return temperature_; }
  float angle() const { return angle_; }
  // Chi ha inviato l'ultima lettura: con più densimetri accesi serve a non
  // attribuire a uno strumento l'angolo misurato dall'altro.
  uint32_t deviceId() const { return deviceId_; }
  int rssi() const { return rssi_; }
  uint32_t updatedEpoch() const { return updatedEpoch_; }

  bool isFresh(uint32_t now) const {
    if (!hasValue_) return false;
    int64_t age = (int64_t)now - (int64_t)updatedEpoch_;
    return age >= 0 && age <= (int64_t)staleSec_;
  }

  uint32_t ageSec(uint32_t now) const {
    if (!hasValue_) return 0;
    int64_t age = (int64_t)now - (int64_t)updatedEpoch_;
    return age > 0 ? (uint32_t)age : 0;
  }

  // Età col segno: negativa se la lettura è "nel futuro" (orologio andato avanti e
  // poi tornato indietro). Chi decide se un dato è utilizzabile deve usare questa,
  // non ageSec(), che clampa a 0 e farebbe sembrare freschissima una lettura vecchia.
  int64_t signedAgeSec(uint32_t now) const {
    if (!hasValue_) return INT64_MAX;
    return (int64_t)now - (int64_t)updatedEpoch_;
  }

  // Salto d'orologio: trasla l'istante della lettura come si fa per profilo e
  // guardia compressore, così l'età resta veritiera.
  void shiftClock(int64_t deltaSec) {
    if (!hasValue_ || updatedEpoch_ == 0) return;
    int64_t v = (int64_t)updatedEpoch_ + deltaSec;
    updatedEpoch_ = v > 1 ? (uint32_t)v : 1;
  }

 private:
  uint32_t staleSec_ = 3600;
  float correction_ = 0.0f;
  float gravity_ = 0.0f, raw_ = 0.0f, battery_ = 0.0f, temperature_ = 0.0f, angle_ = 0.0f;
  int rssi_ = 0;
  uint32_t deviceId_ = 0;
  uint32_t updatedEpoch_ = 0;
  bool hasValue_ = false;
};

}  // namespace ax
