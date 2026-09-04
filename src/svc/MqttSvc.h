#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <functional>
#include "svc/AppStatus.h"
#include "ax_dimensions.h"

// MQTT opzionale (spento di default) con auto-discovery Home Assistant.
// Monitoraggio completo (sensori + sensori binari + entità climate) e controllo
// (climate off/auto + setpoint, pulsanti profilo). Interamente non-bloccante:
// un broker assente non ferma mai il loop di controllo del termostato.
//
// Comandi in ingresso, su <base>/cmd/<x>:
//   climate_mode : "off" -> ferma tutto ; "auto" -> se fermo, avvia il termostato
//   climate_temp : float -> override del setpoint corrente / setpoint termostato
//   profile      : "start"|"stop"|"pause"|"resume"|"skip"
//
// Con piu' camere ogni camera ha il suo ramo: la camera 1 resta su <base>/... — quindi
// entita' e automazioni Home Assistant gia' configurate NON cambiano — e le successive
// stanno su <base>/ch<N>/... . Il topic di disponibilita' (<base>/status, il LWT) resta
// unico: e' il dispositivo a essere online o offline, non la singola camera.
class MqttSvc {
 public:
  void begin(bool enabled, const String& host, uint16_t port, const String& user,
             const String& pass, const String& baseTopic, bool haDiscovery,
             const String& deviceName);
  // Getter dello stato per camera: puntatore a funzione, non std::function, per non
  // allocare a ogni giro di loop e non copiare gli AppStatus (176 B l'uno).
  using StatusGetter = const AppStatus& (*)(uint8_t);
  void loop(uint32_t nowMs, bool staUp, uint8_t chambers, StatusGetter get);
  // Dispatcher unico dei comandi: (nome, payload). Implementato in main.
  // (camera, comando, payload)
  void setCommandHandler(std::function<void(uint8_t, const String&, const String&)> f) {
    onCmd_ = std::move(f);
  }
  bool connected() { return enabled_ && cli_.connected(); }
  // Da chiamare dopo un comando/transizione per riflettere subito lo stato in HA.
  void requestPublish() { lastPublishMs_ = 0; }
  // Rinvia il failsafe hardware durante la discovery: pubblica ~29 entita' PER CAMERA in
  // un solo giro di loop, ognuna con una write TCP bloccante. Con due camere sono 58
  // publish, che nel caso peggiore superano la finestra del failsafe — lo farebbe
  // scattare a ogni riconnessione MQTT, su un'operazione del tutto legittima.
  void setProgress(void (*fn)()) { progress_ = fn; }

 private:
  void publishDiscovery();
  void publishState(const AppStatus& st, uint8_t ch);
  // Ramo dei topic di una camera: la 0 usa base_ nudo (retrocompatibile).
  String chamberBase(uint8_t ch) const { return ch ? base_ + "/ch" + String(ch + 1) : base_; }
  // Suffisso di identita' per le entita' Home Assistant: vuoto sulla camera 0, cosi'
  // gli entity_id gia' in uso restano invariati.
  static String chamberSuffix(uint8_t ch) { return ch ? "_ch" + String(ch + 1) : String(); }
  void pubSensor(const String& devId, uint8_t ch, const char* key, const char* label,
                 const char* unit, const char* devClass, const char* stateClass);
  void pubBinary(const String& devId, uint8_t ch, const char* key, const char* label,
                 const char* devClass);
  void pubButton(const String& devId, uint8_t ch, const char* key, const char* label,
                 const char* payload);
  void addDevice(JsonObject& doc, const String& devId);

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
  IPAddress serverIp_;          // IP risolto una volta: niente DNS bloccante ad ogni retry
  bool ipResolved_ = false;
  std::function<void(uint8_t, const String&, const String&)> onCmd_;
  void (*progress_)() = nullptr;
};
