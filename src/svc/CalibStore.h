#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Punti di calibrazione del densimetro (iSpindel): coppie angolo/densità raccolte
// dall'utente con soluzioni di riferimento. Il calcolo della regressione NON avviene
// qui: i punti vanno alla UI, che fa la regressione in JavaScript. Sull'ESP8266
// risolvere un sistema 4x4 costerebbe flash e RAM per un calcolo che si fa una volta
// ogni due anni, e il browser lo fa gratis.
struct CalibPoint {
  float angle = 0;         // gradi di inclinazione riportati dall'iSpindel
  float gravity = 0;       // densità misurata dall'utente col densimetro
  float temp = 0;          // temperatura della soluzione al momento della misura
  uint32_t epoch = 0;      // quando l'utente ha registrato il punto
  // Epoch della LETTURA iSpindel da cui viene l'angolo. Senza questo, più punti
  // registrati fra due trasmissioni (che distano 15 minuti) condividerebbero lo
  // stesso angolo e la regressione sarebbe priva di senso.
  uint32_t readEpoch = 0;
  bool plato = false;      // unità con cui è stato raccolto: mescolare SG e °P è un errore
  // Densimetro di provenienza: con due strumenti nella stessa soluzione ogni
  // diluizione produce due punti, che vanno tenuti su curve separate.
  uint32_t deviceId = 0;
  char deviceName[33] = {0};
};

class CalibStore {
 public:
  static constexpr const char* kPath = "/calib.json";
  static constexpr uint8_t kMaxPoints = 12;  // 7 bastano, 12 lascia margine

  uint8_t count() const { return n_; }
  // Ritorna nullptr fuori range: un indice sbagliato non deve spacciarsi per il punto 0.
  const CalibPoint* point(uint8_t i) const { return i < n_ ? &pts_[i] : nullptr; }

  bool hasReadEpoch(uint32_t re) const {
    for (uint8_t i = 0; i < n_; i++)
      if (pts_[i].readEpoch == re) return true;
    return false;
  }
  uint8_t countForDevice(uint32_t id) const {
    uint8_t k = 0;
    for (uint8_t i = 0; i < n_; i++)
      if (pts_[i].deviceId == id) k++;
    return k;
  }
  // Unità dei punti già raccolti (il primo detta la regola); false se vuoto.
  bool storedPlato() const { return n_ > 0 && pts_[0].plato; }

  bool add(const CalibPoint& p) {
    if (n_ >= kMaxPoints) return false;
    pts_[n_++] = p;
    return true;
  }

  bool remove(uint8_t idx) {
    if (idx >= n_) return false;
    for (uint8_t i = idx; i + 1 < n_; i++) pts_[i] = pts_[i + 1];
    n_--;
    return true;
  }

  void clear() { n_ = 0; }

  bool load();
  bool save() const;

 private:
  CalibPoint pts_[kMaxPoints];
  uint8_t n_ = 0;
};
