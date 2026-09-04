#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "svc/AppConfig.h"

class ConfigStore {
 public:
  // Capacita' dei documenti di configurazione. Era 6144, cioe' ~3x il fabbisogno del caso
  // peggiore (tutti i campi alla lunghezza massima, segreti in chiaro: ~2,1 KB). Il numero
  // tondo non era gratis: su questo dispositivo il piu' grande blocco CONTIGUO misurato
  // scende fino a 5.848 B, quindi una richiesta da 6.152 non e' garantita — ed e' cosi'
  // che una lettura di profilo e' tornata troncata in campo. 4096 lascia comunque ~2x di
  // margine e sta sotto il minimo osservato.
  static constexpr size_t kConfigDoc = 4096;

  static constexpr const char* kPath = "/config.json";

  // Carica /config.json in cfg. Ritorna false (e lascia i default, valid=false)
  // se il file manca o è corrotto.
  bool load(AppConfig* cfg);

  // Scrittura atomica (tmp + rename).
  bool save(const AppConfig& cfg);

  void toJson(const AppConfig& cfg, JsonDocument& doc, bool maskSecrets) const;
  // Applica su cfg i campi presenti in doc; "***" nei campi segreti = mantieni il valore.
  void fromJson(AppConfig* cfg, JsonDocument& doc) const;
};
