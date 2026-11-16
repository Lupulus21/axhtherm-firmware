#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <functional>
#include "svc/AppStatus.h"

// MQTT opzionale (spento di default) con auto-discovery Home Assistant.
// Monitoraggio completo (sensori + sensori binari + entità climate) e controllo
// (climate off/auto + setpoint, pulsanti profilo). Interamente non-bloccante:
// un broker assente non ferma mai il loop di controllo del termostato.
//
// Comandi in ingresso, su <base>/cmd/<x>:
//   climate_mode : "off" -> ferma tutto ; "auto" -> se fermo, avvia il termostato
//   climate_temp : float -> override del setpoint corrente / setpoint termostato
//   profile      : "start"|"stop"|"pause"|"resume"|"skip"
class MqttSvc {
 public:
  void begin(bool enabled, const String& host, uint16_t port, const String& user,
             const String& pass, const String& baseTopic, bool haDiscovery,
             const String& deviceName);
  void loop(uint32_t nowMs, bool staUp, const AppStatus& st);
  // Dispatcher unico dei comandi: (nome, payload). Implementato in main.
  void setCommandHandler(std::function<void(const String&, const String&)> f) {
    onCmd_ = std::move(f);
  }
  bool connected() { return enabled_ && cli_.connected(); }
  bool enabled() const { return enabled_; }
  uint16_t failures() const { return fails_; }  // tentativi falliti di fila (0 = ok)
  // Da chiamare dopo un comando/transizione per riflettere subito lo stato in HA.
  void requestPublish() { lastPublishMs_ = 0; }
  // Rinvia il failsafe hardware durante la discovery: pubblica ~29 entita' in un solo giro
  // di loop, ognuna con una write TCP bloccante, e nel caso peggiore (broker lento) si
  // avvicinerebbe alla finestra del failsafe su un'operazione del tutto legittima.
  void setProgress(void (*fn)()) { progress_ = fn; }
  // Unita' della densita' per la discovery (3.16.0): SG o °P sulle entita' di HA.
  void setGravityUnit(bool plato) { plato_ = plato; }
  // Esito di un comando ricevuto (3.16.0): un comando rifiutato non si perde in silenzio.
  // reason: 1 setpoint ignorato (scheda ferma o profilo finito), 2 avvio rifiutato (profilo
  // gia' in corso), 3 avvio fallito, 4 valore non valido, 5 niente da confermare.
  void publishResult(const String& cmd, bool ok, uint8_t reason);

 private:
  void publishDiscovery();
  void publishClimate(const String& devId);
  void publishState(const AppStatus& st);
  void pubSensor(const String& devId, const char* key, const char* label, const char* unit,
                 const char* devClass, const char* stateClass);
  void pubBinary(const String& devId, const char* key, const char* label, const char* devClass);
  void pubButton(const String& devId, const char* key, const char* label, const char* payload);
  void addDevice(JsonObject& doc, const String& devId);
  void noteFailure();

  static constexpr uint32_t kConnectTimeoutMs = 500;
  static constexpr uint32_t kRetryMinMs = 30000;
  static constexpr uint32_t kRetryMaxMs = 300000;
  static constexpr uint16_t kReresolveAfter = 5;

  bool enabled_ = false;
  String host_, user_, pass_, base_, name_;
  uint16_t port_ = 1883;
  bool haDiscovery_ = true;
  WiFiClient net_;
  PubSubClient cli_{net_};
  uint32_t lastAttemptMs_ = 0;
  uint32_t lastPublishMs_ = 0;
  bool discoveryDone_ = false;
  bool triedOnce_ = false;      // per non ritardare il primo tentativo di 30 s
  IPAddress serverIp_;          // IP risolto: niente DNS bloccante ad ogni retry
  bool ipResolved_ = false;
  bool hostIsIp_ = false;       // broker indicato come IP: mai DNS
  uint16_t fails_ = 0;          // fallimenti di fila
  uint32_t retryMs_ = kRetryMinMs;  // attesa attuale fra i tentativi
  std::function<void(const String&, const String&)> onCmd_;
  void (*progress_)() = nullptr;
  bool plato_ = false;
};
