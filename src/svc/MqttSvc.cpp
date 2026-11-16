#include "svc/MqttSvc.h"
#include <ArduinoJson.h>
#include "build_id.h"
#include "svc/Alarms.h"

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
  // 3.14.0: 1,5 s -> 0,5 s per la connessione TCP e 2 s -> 1 s per il CONNACK. In rete di
  // casa un broker raggiungibile risponde in pochi ms; uno spento (ARP senza risposta)
  // costava ~1,5-2 s di loop fermo a ogni tentativo.
  net_.setTimeout(kConnectTimeoutMs);
  cli_.setBufferSize(1536);  // margine per discovery climate con base topic al cap
  cli_.setSocketTimeout(1);
  // Se il broker è indicato come IP (consigliato), niente risoluzione DNS: azzera il rischio
  // di blocco. Se è un hostname, lo si risolve al primo tentativo e di nuovo dopo
  // kReresolveAfter fallimenti di fila (il router puo' avergli dato un altro indirizzo).
  ipResolved_ = false;
  hostIsIp_ = serverIp_.fromString(host_);
  if (hostIsIp_) {
    ipResolved_ = true;
    cli_.setServer(serverIp_, port_);
  }
  fails_ = 0;
  retryMs_ = kRetryMinMs;
  triedOnce_ = false;
  cli_.setCallback([this](char* topic, uint8_t* payload, unsigned int len) {
    String t(topic);  // copia su heap: non aliasa il buffer interno di PubSubClient
    String prefix = base_ + "/cmd/";
    if (!t.startsWith(prefix) || !onCmd_) return;
    // Messaggio vuoto: è (anche) la nostra pulizia del retained qui sotto. Ignorarlo
    // non riesegue nulla e spezza l'eco della publish di pulizia (niente loop).
    if (len == 0) return;
    String cmd = t.substring(prefix.length());
    char buf[48];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, payload, len);
    buf[len] = 0;
    onCmd_(cmd, String(buf));
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

void MqttSvc::loop(uint32_t nowMs, bool staUp, const AppStatus& st) {
  if (!enabled_) return;
  if (!staUp) {
    // Al ritorno del WiFi si riprova subito e dal passo piu' corto: l'attesa accumulata
    // riguardava il broker, non la rete.
    triedOnce_ = false;
    retryMs_ = kRetryMinMs;
    return;
  }
  if (!cli_.connected()) {
    if (triedOnce_ && nowMs - lastAttemptMs_ < retryMs_) return;  // primo tentativo subito
    lastAttemptMs_ = nowMs;
    triedOnce_ = true;
    // Risoluzione DNS solo se il broker è un hostname, non un IP.
    if (!ipResolved_) {
      IPAddress ip;
      if (WiFi.hostByName(host_.c_str(), ip, 1000) == 1) {
        serverIp_ = ip;
        ipResolved_ = true;
        cli_.setServer(serverIp_, port_);
      } else {
        noteFailure();
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
    if (!ok) {
      noteFailure();
      return;
    }
    fails_ = 0;
    retryMs_ = kRetryMinMs;
    cli_.publish(willTopic.c_str(), "online", true);
    cli_.subscribe((base_ + "/cmd/+").c_str());
    if (haDiscovery_) publishDiscovery();  // ripubblica a ogni riconnessione (retained)
    lastPublishMs_ = 0;
  }
  cli_.loop();
  if (nowMs - lastPublishMs_ >= 15000) {
    lastPublishMs_ = nowMs;
    publishState(st);
  }
}

// Broker giu': attese crescenti fra i tentativi (30 s, 60, 120, 240, poi 5 min), cosi' un
// broker spento per giorni non costa un blocco del loop ogni 30 s. Dopo kReresolveAfter
// fallimenti di fila un hostname viene risolto di nuovo (ID-06).
void MqttSvc::noteFailure() {
  if (fails_ < 0xFFFF) fails_++;
  retryMs_ = retryMs_ >= kRetryMaxMs / 2 ? kRetryMaxMs : retryMs_ * 2;
  if (!hostIsIp_ && fails_ % kReresolveAfter == 0) ipResolved_ = false;
}

void MqttSvc::publishResult(const String& cmd, bool ok, uint8_t reason) {
  if (!enabled_ || !cli_.connected()) return;
  String p = String(F("{\"cmd\":\"")) + cmd + F("\",\"ok\":") + (ok ? F("true") : F("false"));
  if (!ok) p += String(F(",\"reason\":")) + reason;
  p += '}';
  cli_.publish((base_ + "/cmd_result").c_str(), p.c_str(), false);
}

void MqttSvc::addDevice(JsonObject& doc, const String& devId) {
  JsonObject dev = doc.createNestedObject("dev");
  JsonArray ids = dev.createNestedArray("ids");
  ids.add(devId);
  dev["name"] = name_;
  dev["mf"] = "AxHTherm (firmware locale)";
  dev["mdl"] = "AxHTherm 2.5";
  dev["sw"] = AX_FW_VERSION " (" AX_GIT_REV ")";
}

void MqttSvc::pubSensor(const String& devId, const char* key, const char* label,
                        const char* unit, const char* devClass, const char* stateClass) {
  StaticJsonDocument<640> doc;
  doc["name"] = label;
  doc["uniq_id"] = devId + "_" + key;
  doc["stat_t"] = base_ + "/state";
  doc["avty_t"] = base_ + "/status";
  // Campo assente = 'None' (HA lo mostra sconosciuto): con '' HA scarta lo stato e
  // tiene l'ultimo valore, e una stima d'arrivo vecchia resterebbe a schermo.
  doc["val_tpl"] = String("{{ value_json.") + key + " | default('None') }}";
  if (unit) doc["unit_of_meas"] = unit;
  if (devClass) doc["dev_cla"] = devClass;
  if (stateClass) doc["stat_cla"] = stateClass;
  JsonObject o = doc.as<JsonObject>();
  addDevice(o, devId);
  String topic = String("homeassistant/sensor/") + devId + "_" + key + "/config";
  String payload;
  serializeJson(doc, payload);
  cli_.publish(topic.c_str(), payload.c_str(), true);
  if (progress_) progress_();  // la discovery e' lunga: non far scattare il failsafe
}

void MqttSvc::pubBinary(const String& devId, const char* key, const char* label,
                        const char* devClass) {
  StaticJsonDocument<640> doc;
  doc["name"] = label;
  doc["uniq_id"] = devId + "_" + key;
  doc["stat_t"] = base_ + "/state";
  doc["avty_t"] = base_ + "/status";
  doc["val_tpl"] = String("{{ 'ON' if value_json.") + key + " else 'OFF' }}";
  if (devClass) doc["dev_cla"] = devClass;
  JsonObject o = doc.as<JsonObject>();
  addDevice(o, devId);
  String topic = String("homeassistant/binary_sensor/") + devId + "_" + key + "/config";
  String payload;
  serializeJson(doc, payload);
  cli_.publish(topic.c_str(), payload.c_str(), true);
  if (progress_) progress_();  // la discovery e' lunga: non far scattare il failsafe
}

void MqttSvc::pubButton(const String& devId, const char* key, const char* label,
                        const char* payload) {
  StaticJsonDocument<640> doc;
  doc["name"] = label;
  doc["uniq_id"] = devId + "_btn_" + key;
  doc["cmd_t"] = base_ + "/cmd/profile";
  doc["pl_prs"] = payload;
  doc["avty_t"] = base_ + "/status";
  JsonObject o = doc.as<JsonObject>();
  addDevice(o, devId);
  String topic = String("homeassistant/button/") + devId + "_" + key + "/config";
  String out;
  serializeJson(doc, out);
  cli_.publish(topic.c_str(), out.c_str(), true);
  if (progress_) progress_();
}

// Funzione propria e non inline (3.14.0): il documento da 1 KB del climate restava sullo stack
// anche mentre giravano le ~30 pubSensor/pubBinary (frame 1.248 B + 832 B), la catena
// piu' profonda del firmware a ogni riconnessione (stack libero minimo misurato: 880 B).
__attribute__((noinline)) void MqttSvc::publishClimate(const String& devId) {
  StaticJsonDocument<1024> doc;
  doc["name"] = name_;
  doc["uniq_id"] = devId + "_climate";
  doc["avty_t"] = base_ + "/status";
  doc["temp_unit"] = "C";
  // Lo stesso campo che la scheda accetta: lagering a -1 °C, kveik fino a 40 °C.
  doc["min_temp"] = -1;
  doc["max_temp"] = 40;
  doc["temp_step"] = 0.5;
  JsonArray modes = doc.createNestedArray("modes");
  modes.add("off");
  modes.add("auto");
  doc["mode_cmd_t"] = base_ + "/cmd/climate_mode";
  doc["mode_stat_t"] = base_ + "/state";
  doc["mode_stat_tpl"] = "{{ value_json.climate_mode }}";
  doc["act_t"] = base_ + "/state";
  doc["act_tpl"] = "{{ value_json.climate_action }}";
  doc["curr_temp_t"] = base_ + "/state";
  doc["curr_temp_tpl"] = "{{ value_json.tControl | default('') }}";
  doc["temp_cmd_t"] = base_ + "/cmd/climate_temp";
  doc["temp_stat_t"] = base_ + "/state";
  doc["temp_stat_tpl"] = "{{ value_json.setpoint | default('') }}";
  JsonObject o = doc.as<JsonObject>();
  addDevice(o, devId);
  String topic = String("homeassistant/climate/") + devId + "/config";
  // Mai pubblicare una config di discovery troncata: HA riceverebbe un'entità climate
  // malformata senza segnale del perché. Con base topic al cap il doc potrebbe traboccare.
  if (!doc.overflowed()) {
    String payload;
    serializeJson(doc, payload);
    cli_.publish(topic.c_str(), payload.c_str(), true);
    if (progress_) progress_();
  }
}

void MqttSvc::publishDiscovery() {
  String devId = devIdOf();

  // --- Entità climate: il termostato "nativo" in Home Assistant ---
  publishClimate(devId);

  // --- Sensori ---
  pubSensor(devId, "tControl", "Temperatura mosto", "°C", "temperature", "measurement");
  pubSensor(devId, "tAux1", "Temperatura camera", "°C", "temperature", "measurement");
  pubSensor(devId, "tAux2", "Temperatura ambiente", "°C", "temperature", "measurement");
  pubSensor(devId, "setpoint", "Set point", "°C", "temperature", nullptr);
  pubSensor(devId, "mode_text", "Modalità", nullptr, nullptr, nullptr);
  pubSensor(devId, "step", "Step profilo", nullptr, nullptr, nullptr);
  pubSensor(devId, "gravity", "Densità", plato_ ? "°P" : "SG", nullptr, "measurement");
  pubSensor(devId, "og", "OG", nullptr, nullptr, nullptr);
  pubSensor(devId, "attenuation", "Attenuazione", "%", nullptr, nullptr);
  pubSensor(devId, "abv", "ABV stimato", "%", nullptr, nullptr);
  pubSensor(devId, "days", "Giorni fermentazione", "d", nullptr, nullptr);
  pubSensor(devId, "grav_age", "Età densità", "min", nullptr, nullptr);
  pubSensor(devId, "grav_batt", "Batteria iSpindel", "V", "voltage", "measurement");
  pubSensor(devId, "heap", "Memoria libera", "B", nullptr, "measurement");
  pubSensor(devId, "rssi", "Segnale WiFi", "dBm", "signal_strength", "measurement");
  pubSensor(devId, "loop_max_ms", "Ritardo massimo del ciclo", "ms", "duration", nullptr);
  pubSensor(devId, "slope", "Pendenza temperatura", "°C/h", nullptr, "measurement");
  pubSensor(devId, "crash", "Cold crash", nullptr, nullptr, nullptr);
  pubSensor(devId, "crash_eta", "Cold crash: arrivo stimato", "min", "duration", nullptr);
  // 3.16.0: andamento della fermentazione e allarmi per nome.
  pubSensor(devId, "ferm_trend", "Fermentazione", nullptr, nullptr, nullptr);
  pubSensor(devId, "ferm_slope", "Calo della densità", plato_ ? "°P/d" : "SG/d", nullptr, "measurement");
  pubSensor(devId, "alarms", "Allarmi attivi", nullptr, nullptr, nullptr);

  // --- Sensori binari ---
  pubBinary(devId, "heat", "Riscaldamento", "heat");
  pubBinary(devId, "cool", "Raffreddamento", "cold");
  pubBinary(devId, "armed", "Uscite armate", "power");
  pubBinary(devId, "running", "Profilo in marcia", "running");
  pubBinary(devId, "completed", "Profilo completato", nullptr);
  pubBinary(devId, "alarm", "Allarme", "problem");
  pubBinary(devId, "grav_stale", "Densità stantia", "problem");
  pubBinary(devId, "batt_low", "Batteria iSpindel bassa", "battery");
  pubBinary(devId, "heat_lock", "Caldo bloccato", "problem");
  pubBinary(devId, "failsafe_off", "Failsafe non attivo", "problem");
  // Si accende quando il cold crash arriva al target: e' il gancio per la notifica.
  pubBinary(devId, "crash_reached", "Cold crash raggiunto", nullptr);
  pubBinary(devId, "crash_slow", "Cold crash lento", "problem");
  pubBinary(devId, "antigelo", "Antigelo attivo", "heat");
  pubBinary(devId, "too_cold", "Mosto troppo freddo", "cold");
  pubBinary(devId, "ferm_done", "Fermentazione finita", nullptr);
  pubBinary(devId, "ferm_stuck", "Fermentazione ferma?", "problem");
  pubBinary(devId, "wait_confirm", "Step in attesa di conferma", nullptr);
  pubBinary(devId, "fan", "Ventola", "running");

  // --- Pulsanti profilo ---
  pubButton(devId, "start", "Avvia profilo", "start");
  pubButton(devId, "stop", "Ferma profilo", "stop");
  pubButton(devId, "pause", "Pausa", "pause");
  pubButton(devId, "resume", "Riprendi", "resume");
  pubButton(devId, "skip", "Salta step", "skip");
  pubButton(devId, "confirm", "Conferma step", "confirm");

  discoveryDone_ = true;
}

void MqttSvc::publishState(const AppStatus& st) {
  // Sull'heap dalla 3.16.0: con l'elenco degli allarmi il documento cresce, e da 1 KB sullo
  // stack del loop sarebbe diventato il frame piu' grande del firmware.
  DynamicJsonDocument doc(1536);
  if (doc.capacity() == 0) return;  // heap esaurito: si salta questa pubblicazione
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
  // Il cold crash ha il suo «raggiunto»: «profilo completato» resta per le fermentazioni,
  // o l'automazione «fermentazione completata» scatterebbe anche alla fine del crash.
  doc["completed"] = (st.mode == 2 && st.completed && !st.crashActive);
  // alarmFailsafe incluso: e' l'unico che dice "il controllo si e' fermato", e MQTT e'
  // il solo canale che raggiunge il telefono. Cosi' il failsafe non attivo e il blocco del
  // caldo, che hanno anche un'entita' propria.
  doc["alarm"] = st.heatLock || st.absLimited || st.alarmAirProbe || st.alarmCoolUncommanded ||
                 st.alarmCrashSlow || st.alarmTooCold ||
                 st.alarmFailsafeInerte || st.alarmFailsafe || st.alarmProbe || st.alarmConfig || st.alarmRtc || st.alarmGravity ||
                 st.alarmRampStall || st.alarmStorage || st.alarmMultiGravity ||
                 st.alarmThermal || st.alarmProbeSuspect || st.alarmOutputs;
  doc["grav_stale"] = st.gravityStale;
  doc["batt_low"] = st.alarmBattery;
  doc["heat_lock"] = st.heatLock != 0;
  if (st.heatLock) doc["heat_lock_code"] = st.heatLock;  // ax::HeatLock: il motivo
  doc["failsafe_off"] = st.alarmFailsafeInerte;
  doc["heap"] = st.freeHeap;
  doc["rssi"] = st.rssi;
  doc["loop_max_ms"] = st.loopMaxLateMs;
  if (st.slopeValid) doc["slope"] = serialized(String(st.slopeCph, 2));
  doc["crash"] = !st.crashActive ? "-" : st.crashReachedEpoch ? "raggiunto" : "in discesa";
  if (st.crashActive && st.crashEtaMin >= 0) doc["crash_eta"] = st.crashEtaMin;
  doc["crash_reached"] = st.crashActive && st.crashReachedEpoch != 0;
  doc["crash_slow"] = st.alarmCrashSlow;
  doc["antigelo"] = st.antigelo;
  doc["too_cold"] = st.alarmTooCold;
  {
    static const char* const kTrend[] = {"-", "attiva", "rallenta", "ferma"};
    doc["ferm_trend"] = kTrend[st.fermTrend <= 3 ? st.fermTrend : 0];
    if (st.fermSlopeValid) doc["ferm_slope"] = serialized(String(st.fermSlopeDay, st.gravityPlato ? 2 : 4));
    doc["ferm_done"] = st.fermDone;
    doc["ferm_stuck"] = st.fermStuck;
    doc["wait_confirm"] = st.waitingConfirm;
    doc["fan"] = st.fan;
    // Codici di docs/ALLARMI.md separati da virgola; HA tiene al massimo 255 caratteri.
    const uint32_t mask = alarmMask(st);
    String al;
    uint8_t altri = 0;
    for (uint8_t i = 0; i < AL_COUNT; i++) {
      const uint8_t id = alarmByPriority(i);
      if (!(mask & (1UL << id))) continue;
      String c(alarmCode(id));
      if (al.length() + c.length() + 1 > 230) { altri++; continue; }
      if (al.length()) al += ',';
      al += c;
    }
    if (altri) al += ",+" + String(altri);
    doc["alarms"] = al.length() ? al : String("nessuno");
  }

  if (doc.overflowed()) return;  // stato troncato: meglio saltare questa pubblicazione
  String payload;
  serializeJson(doc, payload);
  cli_.publish((base_ + "/state").c_str(), payload.c_str(), true);
}
