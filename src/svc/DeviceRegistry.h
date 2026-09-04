#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Registro dei densimetri che hanno trasmesso almeno una volta. Serve a una cosa
// concreta: scegliere quale strumento pilota la fermentazione da un elenco, invece
// di doverne ricordare il nome esatto — che altrimenti costringerebbe ad aprire il
// tubo e rientrare nel portale dell'iSpindel solo per rileggerlo.
struct KnownDevice {
  char name[33] = {0};  // 32 byte: due iSpindel con nome che collide nei primi 16 byte non devono confondersi
  uint32_t id = 0;
  uint32_t lastSeen = 0;
};

class DeviceRegistry {
 public:
  static constexpr const char* kPath = "/devices.json";
  static constexpr uint8_t kMax = 4;

  uint8_t count() const { return n_; }
  const KnownDevice* device(uint8_t i) const { return i < n_ ? &devs_[i] : nullptr; }

  // Registra una trasmissione. Ritorna true se il registro è cambiato in modo
  // significativo (strumento nuovo): solo allora vale la pena scrivere su flash.
  bool seen(const String& name, uint32_t id, uint32_t epoch) {
    for (uint8_t i = 0; i < n_; i++) {
      if (devs_[i].id == id || (name.length() && name.equalsIgnoreCase(devs_[i].name))) {
        devs_[i].lastSeen = epoch;
        devs_[i].id = id;
        if (name.length()) strlcpy(devs_[i].name, name.c_str(), sizeof(devs_[i].name));
        return false;  // già noto: nessuna scrittura
      }
    }
    KnownDevice d;
    strlcpy(d.name, name.c_str(), sizeof(d.name));
    d.id = id;
    d.lastSeen = epoch;
    if (n_ < kMax) {
      devs_[n_++] = d;
    } else {
      // Registro pieno: sostituisce il meno recente (un iSpindel dismesso non deve
      // occupare per sempre un posto nell'elenco).
      uint8_t old = 0;
      for (uint8_t i = 1; i < n_; i++)
        if (devs_[i].lastSeen < devs_[old].lastSeen) old = i;
      devs_[old] = d;
    }
    return true;
  }

  bool load();
  bool save() const;

 private:
  KnownDevice devs_[kMax];
  uint8_t n_ = 0;
};
