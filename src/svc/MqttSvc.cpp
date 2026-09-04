#include "svc/MqttSvc.h"
#include <ArduinoJson.h>

static String devIdOf() { return String("axhtherm_") + String(ESP.getChipId(), HEX); }

void MqttSvc::begin(bool enabled, const String& host, uint16_t port, const String& user,
                    const String& pass, const String& baseTopic, bool haDiscovery,
                    const String& deviceName) {
  enabled_ = enabled && host.length() > 0;
  host_ = host;
  port_ = port;
  user_ = user;
  pass_ = pass;
  base_ = baseTopic.length() ? baseTopic : String("axhtherm");
  haDiscovery_ = haDiscovery;
  name_ = deviceName.length() ? deviceName : String("AxHTherm");
  if (!enabled_) return;
  // Timeout stretti: un tentativo di connessione fallito non deve trattenere il loop di
  // controllo. Con l'IP già noto (niente DNS) un fallimento costa ~1,5 s di TCP al massimo.
  net_.setTimeout(1500);
  cli_.setBufferSize(1536);  // margine per discovery climate con base topic al cap
  cli_.setSocketTimeout(2);
  // Se il broker è indicato come IP (consigliato), niente risoluzione DNS: azzera il rischio
  // di blocco. Se è un hostname, lo risolviamo UNA volta al primo tentativo (vedi loop()).
  if (serverIp_.fromString(host_)) {
    ipResolved_ = true;
    cli_.setServer(serverIp_, port_);
  }
  cli_.setCallback([this](char* topic, uint8_t* payload, unsigned int len) {
    String t(topic);  // copia su heap: non aliasa il buffer interno di PubSubClient
    if (!onCmd_) return;
    // La camera si ricava dal RAMO del topic: <base>/cmd/... e' la camera 1 (forma
    // storica), <base>/ch<N>/cmd/... le successive.
    uint8_t ch = 0;
    String prefix;
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
      const String p = chamberBase(i) + "/cmd/";
      if (t.startsWith(p)) { ch = i; prefix = p; break; }
    }
    if (!prefix.length()) return;
    // Messaggio vuoto: è (anche) la nostra pulizia del retained qui sotto. Ignorarlo
    // non riesegue nulla e spezza l'eco della publish di pulizia (niente loop).
    if (len == 0) return;
    String cmd = t.substring(prefix.length());
    char buf[48];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, payload, len);
    buf[len] = 0;
    onCmd_(ch, cmd, String(buf));
    // I comandi non sono stato. MQTT non espone il flag retained alla callback: un
    // comando pubblicato con retain verrebbe riconsegnato — e rieseguito — a OGNI
    // riconnessione (start azzererebbe l'OG ogni volta). Cancelliamo il retained del
    // topic subito dopo l'esecuzione (publish vuoto retained): eseguito al più una
    // volta, mai a ripetizione. Si usa t.c_str() (copia) per non scrivere sul buffer
    // da cui 'topic' punta.
    cli_.publish(t.c_str(), (const uint8_t*)"", 0, true);
    requestPublish();  // riflette subito l'effetto del comando
  });
}

void MqttSvc::loop(uint32_t nowMs, bool staUp, uint8_t chambers, StatusGetter get) {
  if (!enabled_ || !staUp) return;
  if (!cli_.connected()) {
    if (triedOnce_ && nowMs - lastAttemptMs_ < 30000) return;  // primo tentativo subito
    lastAttemptMs_ = nowMs;
    triedOnce_ = true;
    // Risoluzione DNS UNA sola volta (solo se il broker è un hostname, non un IP).
    if (!ipResolved_) {
      IPAddress ip;
      if (WiFi.hostByName(host_.c_str(), ip, 2000) == 1) {
        serverIp_ = ip;
        ipResolved_ = true;
        cli_.setServer(serverIp_, port_);
      } else {
        return;  // DNS non risolto: riprova alla prossima cadenza, senza bloccare oltre
      }
    }
    String willTopic = base_ + "/status";
    bool ok;
    String clientId = String("axhtherm-") + String(ESP.getChipId(), HEX);
    if (user_.length())
      ok = cli_.connect(clientId.c_str(), user_.c_str(), pass_.c_str(), willTopic.c_str(), 0,
                        true, "offline");
    else
      ok = cli_.connect(clientId.c_str(), willTopic.c_str(), 0, true, "offline");
    if (!ok) return;
    cli_.publish(willTopic.c_str(), "online", true);
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++)
      cli_.subscribe((chamberBase(i) + "/cmd/+").c_str());
    if (haDiscovery_) publishDiscovery();  // ripubblica a ogni riconnessione (retained)
    lastPublishMs_ = 0;
  }
  cli_.loop();
  if (nowMs - lastPublishMs_ >= 15000) {
    lastPublishMs_ = nowMs;
    for (uint8_t i = 0; i < chambers && i < AX_N_CHAMBERS; i++) publishState(get(i), i);
  }
}

void MqttSvc::addDevice(JsonObject& doc, const String& devId) {
  JsonObject dev = doc.createNestedObject("dev");
  JsonArray ids = dev.createNestedArray("ids");
  ids.add(devId);
  dev["name"] = name_;
  dev["mf"] = "AxHTherm (firmware locale)";
  dev["mdl"] = "AxHTherm 2.5";
  dev["sw"] = AX_FW_VERSION;
}

void MqttSvc::pubSensor(const String& devId, uint8_t ch, const char* key, const char* label,
                        const char* unit, const char* devClass, const char* stateClass) {
  StaticJsonDocument<640> doc;
  // Sulla camera 0 il suffisso e' VUOTO: nome, uniq_id e topic restano identici a quelli
  // della v2.23.2, quindi gli entity_id gia' usati dalle automazioni non cambiano.
  const String sfx = chamberSuffix(ch);
  doc["name"] = ch ? String(label) + " C" + String(ch + 1) : String(label);
  doc["uniq_id"] = devId + sfx + "_" + key;
  doc["stat_t"] = chamberBase(ch) + "/state";
  doc["avty_t"] = base_ + "/status";
  doc["val_tpl"] = String("{{ value_json.") + key + " | default('') }}";
  if (unit) doc["unit_of_meas"] = unit;
  if (devClass) doc["dev_cla"] = devClass;
  if (stateClass) doc["stat_cla"] = stateClass;
  JsonObject o = doc.as<JsonObject>();
  addDevice(o, devId);
  String topic = String("homeassistant/sensor/") + devId + sfx + "_" + key + "/config";
  String payload;
  serializeJson(doc, payload);
  cli_.publish(topic.c_str(), payload.c_str(), true);
  if (progress_) progress_();  // la discovery e' lunga: non far scattare il failsafe
}

void MqttSvc::pubBinary(const String& devId, uint8_t ch, const char* key, const char* label,
                        const char* devClass) {
  StaticJsonDocument<640> doc;
  const String sfx = chamberSuffix(ch);
  doc["name"] = ch ? String(label) + " C" + String(ch + 1) : String(label);
  doc["uniq_id"] = devId + sfx + "_" + key;
  doc["stat_t"] = chamberBase(ch) + "/state";
  doc["avty_t"] = base_ + "/status";
  doc["val_tpl"] = String("{{ 'ON' if value_json.") + key + " else 'OFF' }}";
  if (devClass) doc["dev_cla"] = devClass;
  JsonObject o = doc.as<JsonObject>();
  addDevice(o, devId);
  String topic = String("homeassistant/binary_sensor/") + devId + sfx + "_" + key + "/config";
  String payload;
  serializeJson(doc, payload);
  cli_.publish(topic.c_str(), payload.c_str(), true);
  if (progress_) progress_();  // la discovery e' lunga: non far scattare il failsafe
}

void MqttSvc::pubButton(const String& devId, uint8_t ch, const char* key, const char* label,
                        const char* payload) {
  StaticJsonDocument<640> doc;
  const String sfx = chamberSuffix(ch);
  doc["name"] = ch ? String(label) + " C" + String(ch + 1) : String(label);
  doc["uniq_id"] = devId + sfx + "_btn_" + key;
  doc["cmd_t"] = chamberBase(ch) + "/cmd/profile";
  doc["pl_prs"] = payload;
  doc["avty_t"] = base_ + "/status";
  JsonObject o = doc.as<JsonObject>();
  addDevice(o, devId);
  String topic = String("homeassistant/button/") + devId + sfx + "_" + key + "/config";
  String out;
  serializeJson(doc, out);
  cli_.publish(topic.c_str(), out.c_str(), true);
  if (progress_) progress_();
}

void MqttSvc::publishDiscovery() {
  String devId = devIdOf();
  // Un'entita' per camera. La camera 0 non prende suffisso: le sue entita' Home
  // Assistant restano quelle di sempre e le automazioni documentate non si rompono.
  for (uint8_t ch = 0; ch < AX_N_CHAMBERS; ch++) {
  const String sfx = chamberSuffix(ch);

  // --- Entità climate: il termostato "nativo" in Home Assistant ---
  {
    StaticJsonDocument<1024> doc;
    doc["name"] = ch ? name_ + " C" + String(ch + 1) : name_;
    doc["uniq_id"] = devId + sfx + "_climate";
    doc["avty_t"] = base_ + "/status";
    doc["temp_unit"] = "C";
    doc["min_temp"] = 0;
    doc["max_temp"] = 30;
    doc["temp_step"] = 0.5;
    JsonArray modes = doc.createNestedArray("modes");
    modes.add("off");
    modes.add("auto");
    doc["mode_cmd_t"] = chamberBase(ch) + "/cmd/climate_mode";
    doc["mode_stat_t"] = chamberBase(ch) + "/state";
    doc["mode_stat_tpl"] = "{{ value_json.climate_mode }}";
    doc["act_t"] = chamberBase(ch) + "/state";
    doc["act_tpl"] = "{{ value_json.climate_action }}";
    doc["curr_temp_t"] = chamberBase(ch) + "/state";
    doc["curr_temp_tpl"] = "{{ value_json.tControl | default('') }}";
    doc["temp_cmd_t"] = chamberBase(ch) + "/cmd/climate_temp";
    doc["temp_stat_t"] = chamberBase(ch) + "/state";
    doc["temp_stat_tpl"] = "{{ value_json.setpoint | default('') }}";
    JsonObject o = doc.as<JsonObject>();
    addDevice(o, devId);
    String topic = String("homeassistant/climate/") + devId + sfx + "/config";
    // Mai pubblicare una config di discovery troncata: HA riceverebbe un'entità climate
    // malformata senza segnale del perché. Con base topic al cap il doc potrebbe traboccare.
    if (!doc.overflowed()) {
      String payload;
      serializeJson(doc, payload);
      cli_.publish(topic.c_str(), payload.c_str(), true);
      if (progress_) progress_();
    }
  }

  // --- Sensori ---
  pubSensor(devId, ch, "tControl", "Temperatura mosto", "°C", "temperature", "measurement");
  pubSensor(devId, ch, "tAux1", "Temperatura camera", "°C", "temperature", "measurement");
  pubSensor(devId, ch, "tAux2", "Temperatura ambiente", "°C", "temperature", "measurement");
  pubSensor(devId, ch, "setpoint", "Set point", "°C", "temperature", nullptr);
  pubSensor(devId, ch, "mode_text", "Modalità", nullptr, nullptr, nullptr);
  pubSensor(devId, ch, "step", "Step profilo", nullptr, nullptr, nullptr);
  pubSensor(devId, ch, "gravity", "Densità", nullptr, nullptr, "measurement");
  pubSensor(devId, ch, "og", "OG", nullptr, nullptr, nullptr);
  pubSensor(devId, ch, "attenuation", "Attenuazione", "%", nullptr, nullptr);
  pubSensor(devId, ch, "abv", "ABV stimato", "%", nullptr, nullptr);
  pubSensor(devId, ch, "days", "Giorni fermentazione", "d", nullptr, nullptr);
  pubSensor(devId, ch, "grav_age", "Età densità", "min", nullptr, nullptr);
  pubSensor(devId, ch, "grav_batt", "Batteria iSpindel", "V", "voltage", "measurement");
  pubSensor(devId, ch, "heap", "Memoria libera", "B", nullptr, "measurement");
  pubSensor(devId, ch, "rssi", "Segnale WiFi", "dBm", "signal_strength", "measurement");

  // --- Sensori binari ---
  pubBinary(devId, ch, "heat", "Riscaldamento", "heat");
  pubBinary(devId, ch, "cool", "Raffreddamento", "cold");
  pubBinary(devId, ch, "armed", "Uscite armate", "power");
  pubBinary(devId, ch, "running", "Profilo in marcia", "running");
  pubBinary(devId, ch, "completed", "Profilo completato", nullptr);
  pubBinary(devId, ch, "alarm", "Allarme", "problem");
  pubBinary(devId, ch, "grav_stale", "Densità stantia", "problem");
  pubBinary(devId, ch, "batt_low", "Batteria iSpindel bassa", "battery");

  // --- Pulsanti profilo ---
  pubButton(devId, ch, "start", "Avvia profilo", "start");
  pubButton(devId, ch, "stop", "Ferma profilo", "stop");
  pubButton(devId, ch, "pause", "Pausa", "pause");
  pubButton(devId, ch, "resume", "Riprendi", "resume");
  pubButton(devId, ch, "skip", "Salta step", "skip");

  discoveryDone_ = true;
  }  // fine del ciclo sulle camere
}

void MqttSvc::publishState(const AppStatus& st, uint8_t ch) {
  StaticJsonDocument<1024> doc;
  if (st.tControlValid) doc["tControl"] = serialized(String(st.tControl, 2));
  if (st.tAux1Valid) doc["tAux1"] = serialized(String(st.tAux1, 2));
  if (st.tAux2Valid) doc["tAux2"] = serialized(String(st.tAux2, 2));
  if (st.hasSetpoint) doc["setpoint"] = serialized(String(st.setpoint, 2));

  const char* modes[] = {"Fermo", "Termostato", "Profilo"};
  doc["mode_text"] = modes[st.mode <= 2 ? st.mode : 0];
  if (st.mode == 2)
    doc["step"] = String(st.stepEnIdx + 1) + "/" + String(st.stepEnCount);
  else
    doc["step"] = "-";

  // Climate: mode off/auto, azione heating/cooling/idle/off.
  doc["climate_mode"] = st.mode == 0 ? "off" : "auto";
  doc["climate_action"] = st.heat ? "heating" : st.cool ? "cooling"
                                                        : (st.mode == 0 ? "off" : "idle");

  if (st.gravityHas) {
    doc["gravity"] = serialized(String(st.gravity, 4));
    doc["grav_batt"] = serialized(String(st.gravityBattery, 2));
    doc["grav_age"] = st.gravityAgeSec / 60;
  }
  if (st.gravityOg > 0) doc["og"] = serialized(String(st.gravityOg, 4));
  if (st.gravityHas && st.gravityOg > 0) {
    doc["attenuation"] = serialized(String(st.attenuation, 0));
    doc["abv"] = serialized(String(st.abv, 1));
  }
  // profileStartEpoch deve essere un epoch reale (>~2017), non una timeline fittizia da
  // boot senza RTC: altrimenti i giorni sarebbero un numero assurdo.
  if (st.mode == 2 && st.profileStartEpoch > 1500000000UL && st.nowEpoch > st.profileStartEpoch)
    doc["days"] = serialized(String((st.nowEpoch - st.profileStartEpoch) / 86400.0f, 1));

  doc["heat"] = st.heat;
  doc["cool"] = st.cool;
  doc["armed"] = st.armed;
  doc["running"] = (st.mode == 2 && !st.completed && !st.paused);
  doc["completed"] = (st.mode == 2 && st.completed);
  // alarmFailsafe incluso: e' l'unico che dice "il controllo si e' fermato", e MQTT e'
  // il solo canale che raggiunge il telefono.
  doc["alarm"] = st.alarmFailsafe || st.alarmProbe || st.alarmConfig || st.alarmRtc || st.alarmGravity ||
                 st.alarmRampStall || st.alarmStorage || st.alarmMultiGravity ||
                 st.alarmThermal || st.alarmProbeSuspect || st.alarmOutputs;
  doc["grav_stale"] = st.gravityStale;
  doc["batt_low"] = st.alarmBattery;
  doc["heap"] = st.freeHeap;
  doc["rssi"] = st.rssi;

  if (doc.overflowed()) return;  // stato troncato: meglio saltare questa pubblicazione
  String payload;
  serializeJson(doc, payload);
  cli_.publish((chamberBase(ch) + "/state").c_str(), payload.c_str(), true);
}
