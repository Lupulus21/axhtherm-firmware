#include "svc/WebApi.h"
#include "ax/MinMaxGroup.h"
#include "ax/GravityTrend.h"
#include "svc/BootLog.h"
#include <ArduinoJson.h>
#include <memory>
#include <new>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <Updater.h>
#include <Wire.h>
#include <uri/UriBraces.h>
#include <MD5Builder.h>
#include <coredecls.h>  // crc32()
#include "web_assets.h"
#include "build_id.h"
#include "ax/ProbeOffsets.h"

// Validazione comune degli step (editor libreria e profilo attivo).
// Ritorna nullptr se valido, altrimenti un messaggio IN FLASH (PSTR): usare con FPSTR.
static PGM_P validateSteps(const ax::StepDef* steps, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    if (!steps[i].enabled) continue;
    // Freeze-guard: sotto -1 °C l'overshoot del frigo può portare la birra verso il
    // punto di congelamento (~-3 °C a 5% ABV): il floor del setpoint è -1.
    if (steps[i].targetC < -1 || steps[i].targetC > 40 || steps[i].startC < -1 ||
        steps[i].startC > 40)
      return PSTR("temperatura step fuori range (-1..40 \xC2\xB0""C)");
    if (steps[i].durationSec > 90UL * 86400UL) return PSTR("durata step oltre 90 giorni");
    if (steps[i].degPerHour < 0 || steps[i].degPerHour > 10)
      return PSTR("rampa fuori range (0..10 \xC2\xB0""C/h)");
    // maxDurationSec è ora un timeout di sicurezza anche per RampRate (oltre che per
    // Gravity): va validato dove il motore lo usa.
    if (steps[i].type == ax::StepType::RampRate &&
        steps[i].maxDurationSec > 90UL * 86400UL)
      return PSTR("timeout rampa oltre 90 giorni");
    // I campi densità si validano SOLO dove il motore li usa: uno step fixed con
    // residui strani (import, cambio tipo in editor) non deve essere rifiutato per
    // un campo invisibile.
    if (steps[i].type != ax::StepType::Gravity) continue;
    if (steps[i].maxDurationSec > 90UL * 86400UL) return PSTR("timeout step oltre 90 giorni");
    if (steps[i].gravityMode == ax::GravityMode::Attenuation) {
      if (steps[i].gravityTarget < 1 || steps[i].gravityTarget > 100)
        return PSTR("attenuazione target fuori range (1..100 %)");
    } else if (steps[i].gravityMode == ax::GravityMode::Below &&
               (steps[i].gravityTarget < 0 || steps[i].gravityTarget > 35)) {
      return PSTR("densita' target fuori range");
    }
    if (steps[i].gravityMode == ax::GravityMode::Stable) {
      if (steps[i].stableBand < 0 || steps[i].stableBand > 2)
        return PSTR("banda di stabilita' fuori range");
      if (steps[i].stableHours < 1 || steps[i].stableHours > 168)
        return PSTR("finestra di stabilita' fuori range (1..168 h)");
    }
  }
  return nullptr;
}

// Un profilo con step a densita' e' caricato (in corsa o fermo)?
bool WebApi::profiloDensita() const {
  const ax::ProfileEngine* eng = c_().engine;
  if (!eng) return false;
  const ax::StepDef* ps = eng->steps();
  for (uint8_t i = 0; ps && i < eng->stepCount(); i++)
    if (ps[i].enabled && ps[i].type == ax::StepType::Gravity) return true;
  return false;
}

String WebApi::segAfter(const char* prefix) const {
  const String u = server_.uri();
  const size_t n = strlen(prefix);
  if (!u.startsWith(prefix)) return String();
  int fine = u.indexOf('/', (int)n);
  return fine < 0 ? u.substring((int)n) : u.substring((int)n, fine);
}

void WebApi::begin(const Refs& refs) {
  r_ = refs;

  server_.collectHeaders("If-None-Match", "Cookie", "X-Ax-Password", "Content-Type");
  bootHost_ = r_.cfg->hostname;
  bootNetSig_ = netSig(*r_.cfg);

  server_.on(F("/"), HTTP_GET, [this]() { handleRoot(); });
  server_.on(F("/api/login"), HTTP_POST, [this]() { handleLogin(); });
  server_.on(F("/api/logout"), HTTP_POST, [this]() {
    if (!hostOk()) return;
    server_.sendHeader("Set-Cookie", "axs=; Max-Age=0; Path=/");
    sendOk();
  });
  // Icona per "Aggiungi a Home"/favicon: senza auth (Safari la scarica fuori sessione).
  server_.on(F("/icon.png"), HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "max-age=86400");
    server_.send_P(200, "image/png", (const char*)WEB_ICON_PNG, WEB_ICON_PNG_LEN);
  });
  server_.on(F("/api/status"), HTTP_GET, [this]() { handleStatus(); });
  server_.on(F("/api/config"), HTTP_GET, [this]() { handleGetConfig(); });
  server_.on(F("/api/config"), HTTP_PUT, [this]() { handlePutConfig(); });
  server_.on(F("/api/profile"), HTTP_GET, [this]() { handleGetProfile(); });
  server_.on(F("/api/profile"), HTTP_PUT, [this]() { handlePutProfile(); });
  server_.on(F("/api/profiles"), HTTP_GET, [this]() { handleProfilesList(); });
  server_.on(UriBraces(String(F("/api/profiles/{}"))), HTTP_GET, [this]() { handleProfileById(HTTP_GET); });
  server_.on(UriBraces(String(F("/api/profiles/{}"))), HTTP_PUT, [this]() { handleProfileById(HTTP_PUT); });
  server_.on(UriBraces(String(F("/api/profiles/{}"))), HTTP_DELETE,
             [this]() { handleProfileById(HTTP_DELETE); });
  server_.on(F("/api/profile/start"), HTTP_POST, [this]() { handleProfileCmd("start"); });
  server_.on(F("/api/profile/stop"), HTTP_POST, [this]() { handleProfileCmd("stop"); });
  server_.on(F("/api/profile/pause"), HTTP_POST, [this]() { handleProfileCmd("pause"); });
  server_.on(F("/api/profile/resume"), HTTP_POST, [this]() { handleProfileCmd("resume"); });
  server_.on(F("/api/profile/skip"), HTTP_POST, [this]() { handleProfileCmd("skip"); });
  server_.on(F("/api/profile/confirm"), HTTP_POST, [this]() { handleProfileCmd("confirm"); });
  server_.on(F("/api/profile/coldcrash"), HTTP_POST, [this]() { handleColdCrash(); });
  server_.on(F("/api/setpoint"), HTTP_POST, [this]() { handleSetpoint(); });
  server_.on(F("/api/thermostat"), HTTP_POST, [this]() { handleThermostat(); });
  server_.on(F("/api/gravity/og"), HTTP_POST, [this]() { handleOg(); });
  server_.on(F("/api/outputs/arm"), HTTP_POST, [this]() { handleArm(); });
  server_.on(F("/api/outputs/disarm"), HTTP_POST, [this]() {
    if (!auth()) return;
    // Disarma la camera e spegne TUTTI gli 8 canali (anche quelli di un test manuale).
    r_.disarmAll();
    sendSaved(r_.persistNow());
  });
  server_.on(F("/api/safety/reset"), HTTP_POST, [this]() {
    if (!auth()) return;
    // Sblocco del caldo dopo un guasto rilevato: solo da un gesto esplicito dell'utente.
    sendSaved(r_.clearHeatLock());
  });

  server_.on(F("/ispindel"), HTTP_POST, [this]() { handleIspindel(); });
  server_.on(F("/api/calib"), HTTP_GET, [this]() { handleCalibGet(); });
  server_.on(F("/api/calib"), HTTP_POST, [this]() { handleCalibAdd(); });
  server_.on(F("/api/calib"), HTTP_DELETE, [this]() { handleCalibDelete(); });
  server_.on(F("/api/log.csv"), HTTP_GET, [this]() { handleLogCsv(); });
  server_.on(F("/api/events"), HTTP_GET, [this]() { handleEvents(); });
  server_.on(F("/api/log"), HTTP_GET, [this]() { handleLogJson(); });
  server_.on(F("/api/wifi/scan"), HTTP_GET, [this]() { handleWifiScan(); });
  server_.on(F("/api/diag/i2c"), HTTP_GET, [this]() { handleDiagI2c(); });
  server_.on(F("/api/diag/onewire"), HTTP_GET, [this]() { handleDiagOneWire(); });
  server_.on(F("/api/probes"), HTTP_GET, [this]() { handleProbeDiag(); });
  server_.on(F("/api/probes/calibrate"), HTTP_POST, [this]() { handleProbeCalib(); });
  server_.on(F("/api/probes/reset"), HTTP_POST, [this]() {
    if (!auth()) return;
    r_.probes->resetStats();
    sendOk();
  });
  server_.on(F("/api/diag/relay"), HTTP_POST, [this]() { handleDiagRelay(); });
  server_.on(F("/api/diag/testmode"), HTTP_POST, [this]() {
    if (!auth()) return;
    r_.outputs->exitTestMode();
    sendOk();
  });
  server_.on(F("/api/time"), HTTP_POST, [this]() { handleTime(); });
  server_.on(F("/api/reboot"), HTTP_POST, [this]() {
    if (!auth()) return;
    sendJson(200, "{\"ok\":true,\"reboot\":true}");
    r_.scheduleReboot(1000, (uint8_t)RebootIntent::User);
  });
  server_.on(F("/api/factory_reset"), HTTP_POST, [this]() {
    if (!auth()) return;
    // Il reset rigenera le password: dopo, si rientra solo dal seriale o dal display. Il
    // cookie di sessione da solo non basta: serve la password attuale.
    StaticJsonDocument<160> d;
    deserializeJson(d, server_.arg("plain"));
    const char* attuale = d["currentPassword"] | "";
    if (r_.cfg->uiPassword.length() && pwLocked()) return;
    if (r_.cfg->uiPassword.length() && r_.cfg->uiPassword != attuale) {
      pwFail();
      sendErr(403, F("password attuale errata"));
      return;
    }
    sendJson(200, "{\"ok\":true,\"reboot\":true}");
    r_.factoryReset();
  });
  server_.on(F("/api/update"), HTTP_POST, [this]() { handleUpdateFinal(); },
             [this]() { handleUpdateUpload(); });
  server_.onNotFound([this]() {
    if (server_.method() == HTTP_OPTIONS) {
      server_.send(204);
      return;
    }
    // Splash screen iOS (apple-touch-startup-image), una per risoluzione iPhone. Senza
    // auth: iOS le scarica al momento di "Aggiungi a Home". Servite da qui invece che con
    // una rotta ciascuna: dieci registrazioni costavano heap PERMANENTE (handler, String
    // dell'URI e std::function) per file che si scaricano una volta sola.
    if (server_.method() == HTTP_GET && server_.uri().startsWith(F("/splash_"))) {
      for (unsigned int i = 0; i < WEB_SPLASHES_COUNT; i++) {
        if (server_.uri() != WEB_SPLASHES[i].path) continue;
        server_.sendHeader(F("Cache-Control"), F("max-age=604800"));
        server_.send_P(200, "image/png", (const char*)WEB_SPLASHES[i].data, WEB_SPLASHES[i].len);
        return;
      }
    }
    server_.send(404, "text/plain", "Not found");
  });
  server_.begin();
}

void WebApi::computeToken() {
  MD5Builder md5;
  md5.begin();
  md5.add(r_.cfg->uiPassword + ":" + String(ESP.getChipId(), HEX) + ":axsession");
  md5.calculate();
  sessionToken_ = md5.toString();
  tokenPass_ = r_.cfg->uiPassword;
}

bool WebApi::hostOk(bool reply) {
  String h = server_.hostHeader();
  bool ok = h.length() == 0;  // HTTP/1.0 senza Host: niente da confrontare
  if (!ok) {
    const int dp = h.indexOf(':');
    if (dp >= 0) h.remove(dp);
    if (h.endsWith(".")) h.remove(h.length() - 1);
    h.toLowerCase();
    IPAddress ip;
    // Un indirizzo IP non si puo' riassegnare a un'altra macchina come un nome DNS.
    ok = ip.fromString(h);
    // Solo domini che un estraneo non puo' registrare: .local (mDNS) e quelli dei router
    // di casa. Un "nome.qualsiasi" no: con il nome della scheda davanti a un dominio suo,
    // una pagina malevola passerebbe il controllo.
    static const char* const kDomini[] = {"", ".local", ".lan", ".home", ".home.arpa",
                                          ".fritz.box", ".localdomain", ".internal"};
    for (const String* n : {&r_.cfg->hostname, &bootHost_}) {
      if (ok || !n->length()) continue;
      String nome = *n;
      nome.toLowerCase();
      for (const char* d : kDomini) {
        if (h == nome + d) { ok = true; break; }
      }
    }
  }
  // Il nome della scheda NON va nella risposta: la pagina malevola la legge, e
  // imparerebbe proprio il nome da usare al tentativo successivo.
  if (!ok && reply)
    server_.send(403, "text/plain",
                 F("Nome non riconosciuto: apri la scheda con il suo indirizzo IP o con il suo "
                   "nome seguito da .local"));
  return ok;
}

bool WebApi::pwLocked() {
  if (!pwPen_.locked(ipKey(), millis())) return false;
  sendWait(pwPen_.waitSec(ipKey(), millis()));
  return true;
}

void WebApi::sendWait(uint32_t sec) {
  sendErr(429, (String(F("troppi tentativi sbagliati: riprova fra ")) + sec + F(" s")).c_str());
}

uint32_t WebApi::netSig(const AppConfig& c) {
  const String s = c.ssid + c.wifiPassword + String(c.dhcp) + c.ip + c.mask + c.gw + c.dns +
                   c.hostname + c.softApPassword + String(c.softApAfterMin) + c.oledDriver +
                   String(c.oledAddress) + String(c.oledEnabled) + String(c.oledFlip) +
                   String(c.mqttEnabled) + c.mqttHost + String(c.mqttPort) + c.mqttUser +
                   c.mqttPass + c.mqttBaseTopic + String(c.ntpEnabled) + c.ntpServer + c.tz;
  return crc32(s.c_str(), s.length());
}

bool WebApi::authValid() {
  if (r_.cfg->uiPassword.length() == 0) return true;
  if (tokenPass_ != r_.cfg->uiPassword) computeToken();
  if (server_.hasHeader("Cookie")) {
    String c = server_.header("Cookie");
    int i = c.indexOf("axs=");
    // "axs=" deve iniziare un cookie, non essere la coda di un altro nome.
    while (i > 0 && c[i - 1] != ' ' && c[i - 1] != ';') i = c.indexOf("axs=", i + 1);
    if (i >= 0) {
      String v = c.substring(i + 4);
      int j = v.indexOf(';');
      if (j >= 0) v = v.substring(0, j);
      v.trim();
      if (v.length() && v == sessionToken_) return true;
    }
  }
  // Fallback per script/curl: Basic auth esplicita (nessun popup: siamo noi a NON
  // mandare WWW-Authenticate nella risposta 401). In pausa per troppi errori non si prova.
  if (!server_.hasHeader(F("Authorization")) || pwPen_.locked(ipKey(), millis())) return false;
  if (!server_.authenticate("admin", r_.cfg->uiPassword.c_str())) return false;
  pwPen_.success(ipKey());
  return true;
}

bool WebApi::auth() {
  // Keepalive su ogni richiesta, non solo sull'upload: il framework legge il CORPO con
  // attese senza tetto, quindi un peer che sparisce senza chiudere il TCP terrebbe fermo
  // il loop. Gate su ESTABLISHED perche' ClientContext::keepAlive dereferenzia _pcb senza
  // controllo di nullita' (vedi il commento in handleUpdateUpload).
  if (server_.client().status() == ESTABLISHED) server_.client().keepAlive(5, 2, 3);
  if (!hostOk()) return false;
  if (authValid()) return true;
  if (server_.hasHeader(F("Authorization"))) {
    if (pwLocked()) return false;
    pwFail();
  }
  sendJson(401, "{\"ok\":false,\"error\":\"login richiesto\"}");
  return false;
}

void WebApi::handleLogin() {
  if (!hostOk()) return;
  // Solo JSON dichiarato: un modulo inviato da un altro sito (text/plain, senza
  // preflight) non deve poter consumare i tentativi del proprietario.
  if (!server_.header("Content-Type").startsWith(F("application/json"))) {
    sendErr(415, F("serve Content-Type: application/json"));
    return;
  }
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, F("json non valido"));
    return;
  }
  String pw = doc["password"] | String("");
  if (r_.cfg->uiPassword.length() && pwLocked()) return;
  if (r_.cfg->uiPassword.length() && pw != r_.cfg->uiPassword) {
    pwFail();
    sendErr(401, F("password errata"));
    return;
  }
  pwPen_.success(ipKey());
  if (tokenPass_ != r_.cfg->uiPassword) computeToken();
  server_.sendHeader("Set-Cookie", String("axs=") + sessionToken_ +
                                       "; Max-Age=31536000; Path=/; SameSite=Lax; HttpOnly");
  sendOk();
}

void WebApi::handleRoot() {
  // La pagina (solo codice UI, nessun dato) è pubblica: il login avviene in pagina
  // e ogni /api/* resta protetta. Così niente popup Basic-auth del browser.
  if (!hostOk()) return;
  if (server_.hasHeader("If-None-Match") &&
      server_.header("If-None-Match") == WEB_INDEX_ETAG) {
    server_.send(304);
    return;
  }
  server_.sendHeader("Content-Encoding", "gzip");
  server_.sendHeader("ETag", WEB_INDEX_ETAG);
  // no-store: la UI viaggia col firmware; una cache stantia dopo un aggiornamento
  // mostrerebbe una pagina vecchia con la versione nuova (confusione garantita).
  server_.sendHeader("Cache-Control", "no-store");
  server_.send_P(200, "text/html", (const char*)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

__attribute__((noinline)) void WebApi::statusSistema(JsonDocument& doc, const AppStatus& st) {
  doc["fw"] = AX_FW_VERSION;
  doc["build"] = AX_GIT_REV;  // commit; "-mod" = compilato con modifiche non salvate
  doc["host"] = r_.cfg->hostname;  // nome della scheda: con due schede dice quale si sta guardando
  if (rebootPending_) doc["rebootPending"] = true;  // impostazioni salvate, attive al riavvio
  doc["chip"] = String(ESP.getChipId(), HEX);  // identita' fisica: il backup sa da che scheda viene
  // Diagnostica dell'heap. Il totale libero da solo NON basta a spiegare i guasti di
  // allocazione: ArduinoJson e le String hanno bisogno di un blocco CONTIGUO, e con
  // l'heap frammentato si puo' avere molta memoria libera e nessun blocco abbastanza
  // grande. E' esattamente il meccanismo del troncamento silenzioso osservato in campo.
  doc["heapFrag"] = ESP.getHeapFragmentation();      // %
  doc["heapMaxBlock"] = ESP.getMaxFreeBlockSize();   // piu' grande blocco contiguo
  // Minimi dall'avvio (HeapMon): il margine reale, picchi degli handler compresi.
  doc["heapMin"] = HeapMon::freeMin();
  if (r_.heap) doc["heapMaxBlockMin"] = r_.heap->maxBlockMin;
  doc["stackFreeMin"] = HeapMon::stackFreeMin();
  doc["allocFail"] = HeapMon::oom();
  doc["uptimeSec"] = st.uptimeSec;  // 3.15.0 (UX-39)
  {
    JsonObject mq = doc.createNestedObject("mqtt");  // 3.15.0 (ID-08)
    mq["state"] = st.mqttState;                      // 0 spento, 1 non connesso, 2 connesso
    mq["fails"] = st.mqttFails;
  }
  doc["eventsOn"] = st.eventsOn;
  if (r_.loop) {
    JsonObject lp = doc.createNestedObject("loop");
    lp["late"] = r_.loop->late;
    lp["maxLateMs"] = r_.loop->maxLateMs;
    lp["lastLateMs"] = r_.loop->lastLateMs;
    lp["lastLateAt"] = r_.loop->lastLateEpoch;
    lp["kicks"] = r_.loop->kicks;
    lp["legitLong"] = r_.loop->legitLong;
  }
  if (r_.boots) doc["boots"] = r_.boots->total();
  if (st.slopeValid) {
    JsonObject sl = doc.createNestedObject("slope");
    sl["cph"] = serialized(String(st.slopeCph, 2));  // °C/h, finestra di 20 minuti
    if (st.etaMin >= 0) sl["etaMin"] = st.etaMin;   // minuti al setpoint
  }
  {
    JsonObject li = doc.createNestedObject("limits");
    li["airCool"] = st.airCoolLimited;  // freddo sospeso: aria troppo fredda
    li["airHeat"] = st.airHeatLimited;  // caldo sospeso: aria troppo calda
    li["abs"] = st.absLimited;          // limite assoluto sul mosto
    li["antigelo"] = st.antigelo;       // caldo riacceso come antigelo
  }
  if (st.crashActive) {
    JsonObject cc = doc.createNestedObject("crash");
    cc["targetC"] = serialized(String(st.crashTargetC, 1));
    cc["startEpoch"] = st.crashStartEpoch;
    if (st.crashReachedEpoch) cc["reachedEpoch"] = st.crashReachedEpoch;
    if (st.crashEtaMin >= 0) {
      cc["etaMin"] = st.crashEtaMin;
      cc["etaMeasured"] = st.crashEtaMeasured;  // dalla discesa reale o dal programma
    }
    cc["dropC"] = serialized(String(st.crashDropC, 1));   // discesa nella finestra
    cc["windowH"] = serialized(String(st.crashWindowH, 1));
    cc["minDropC"] = serialized(String(st.crashMinDropC, 2));
    cc["cph"] = serialized(String(st.crashCph, 2));
    cc["slow"] = st.alarmCrashSlow;
  }
  if (r_.reboot) doc["crashStreak"] = r_.reboot->crashStreak;
  doc["heap"] = st.freeHeap;

  JsonObject tm = doc.createNestedObject("time");
  tm["epoch"] = st.nowEpoch;
  tm["local"] = r_.time->localString(st.nowEpoch);
  tm["trusted"] = st.timeTrusted;
  tm["rtcPresent"] = r_.time->rtcPresent();
  tm["ntp"] = r_.time->ntpSynced();

  JsonArray temps = doc.createNestedArray("temps");
  const float vals[3] = {st.tControl, st.tAux1, st.tAux2};
  const bool oks[3] = {st.tControlValid, st.tAux1Valid, st.tAux2Valid};
  // Indici REALI delle sonde di questa camera. Usare 0,1,2 fissi avrebbe etichettato le
  // temperature della camera 2 con i nomi e lo stato di configurazione delle sonde della
  // camera 1: i valori venivano gia' dalla camera servita, le etichette no.
  const uint8_t iSonda[3] = {
      c_().controlProbe ? *c_().controlProbe : (uint8_t)0,
      c_().auxCount > 0 ? c_().auxProbe[0] : (uint8_t)0,
      c_().auxCount > 1 ? c_().auxProbe[1] : (uint8_t)0};
  for (int i = 0; i < 3; i++) {
    const uint8_t p = iSonda[i] < ProbeBus::kProbes ? iSonda[i] : 0;
    JsonObject t = temps.createNestedObject();
    t["label"] = r_.cfg->probeLabel[p];
    t["valid"] = oks[i];
    if (oks[i]) t["c"] = serialized(String(vals[i], 2));
    t["configured"] = r_.cfg->probeId[p].length() == 16;
  }

  doc["mode"] = st.mode;
  if (st.hasSetpoint) doc["setpoint"] = serialized(String(st.setpoint, 2));
  doc["thermostatSetpoint"] = serialized(String(c_().engine->thermostatSetpoint(), 2));
}

__attribute__((noinline)) void WebApi::statusCamera(JsonDocument& doc, const AppStatus& st) {
  JsonObject prof = doc.createNestedObject("profile");
  prof["running"] = c_().engine->running();
  prof["paused"] = st.paused;
  prof["completed"] = st.completed;
  prof["stepIdx"] = st.stepIdx;       // assoluto: per indicizzare RUNPROF.steps
  prof["stepCount"] = st.stepCount;
  prof["stepEnIdx"] = st.stepEnIdx;   // abilitati: per la visualizzazione «step N/M»
  prof["stepEnCount"] = st.stepEnCount;
  prof["waitingDelayedStart"] = st.waitingDelayedStart;
  if (st.waitingTarget) prof["waitingTarget"] = true;
  if (st.waitTimedOut) prof["waitTimedOut"] = true;
  if (st.waitingConfirm) prof["waitingConfirm"] = true;  // 3.16.0
  if (st.stepEndsAt) prof["stepEndsAt"] = st.stepEndsAt;
  // Solo un epoch REALE (>~2017): se il profilo è stato avviato con orologio inaffidabile
  // (timeline di fallback ~100) profileStartEpoch NON viene traslato dal sync NTP e mostrerebbe
  // un'età di fermentazione assurda. Stessa guardia di MqttSvc.
  if (st.profileStartEpoch > 1500000000UL) prof["startEpoch"] = st.profileStartEpoch;
  prof["overrideActive"] = c_().engine->overrideActive();
  prof["name"] = st.profileName;
  prof["batch"] = st.batchName;
  prof["waitsGravity"] = st.profileWaitsGravity;
  prof["noLimit"] = st.profileNoLimit;
  // In pausa i conteggi si fermano qui: senza questo la UI continuerebbe a contare.
  if (st.paused) prof["pausedAtEpoch"] = c_().engine->snapshot().pausedAtEpoch;
  prof["hasGravity"] = st.gravityHas;
  prof["stepStartEpoch"] = st.stepStartEpoch;
  prof["stepStartTemp"] = serialized(String(st.stepStartTemp, 2));

  JsonObject out = doc.createNestedObject("outputs");
  out["heat"] = st.heat;
  out["fan"] = st.fan;            // 3.17.0
  out["fanRel"] = st.fanRel;      // rele' effettivo (0 = nessuno)
  if (st.fanConflict) out["fanConflict"] = true;
  out["cool"] = st.cool;
  out["armed"] = st.armed;
  out["coolRel"] = r_.outputs->coolRel(c_().chamber);
  out["heatRel"] = r_.outputs->heatRel(c_().chamber);
  out["coolBlockedSec"] = st.coolBlockedSec;
  out["pidDuty"] = serialized(String(st.pidDuty, 0));
  out["predCoolCph"] = serialized(String(st.predCoolCph, 2));
  out["predHeatCph"] = serialized(String(st.predHeatCph, 2));
  out["testMode"] = r_.outputs->testMode();

  JsonObject gr = doc.createNestedObject("gravity");
  gr["has"] = st.gravityHas;
  gr["unit"] = r_.cfg->gravityUnit;
  if (st.gravityOg > 0) gr["og"] = serialized(String(st.gravityOg, 4));
  if (st.gravityHas) {
    gr["value"] = serialized(String(st.gravity, 4));
    gr["ageSec"] = st.gravityAgeSec;
    gr["stale"] = st.gravityStale;
    gr["battery"] = serialized(String(st.gravityBattery, 2));
    gr["temp"] = serialized(String(st.gravityTemp, 1));
    gr["angle"] = serialized(String(st.gravityAngle, 2));
    gr["rssi"] = st.gravityRssi;
  }
  {
    // 3.16.0: andamento (0 ?, 1 attiva, 2 rallenta, 3 ferma), pendenza su 24 ore nell'unita'
    // della densita' al giorno, avvisi di fine e di fermentazione ferma.
    JsonObject fe = gr.createNestedObject("trend");
    fe["state"] = st.fermTrend;
    if (st.fermSlopeValid) fe["perDay"] = serialized(String(st.fermSlopeDay, st.gravityPlato ? 2 : 4));
    fe["done"] = st.fermDone;
    fe["stuck"] = st.fermStuck;
  }
}

__attribute__((noinline)) void WebApi::statusAllarmi(JsonDocument& doc, const AppStatus& st) {
  JsonArray alarms = doc.createNestedArray("alarms");
  if (st.fermStuck)
    alarms.add(F("FERMENTAZIONE FERMA? La densita' non cambia da 48 ore ma e' sopra l'atteso "
                 "(attenuazione sotto il 50% o soglia dello step non raggiunta). Lievito, "
                 "temperatura, densimetro incrostato? Si disattiva in Impostazioni."));
  if (st.absLimited)
    alarms.add(F("MOSTO FUORI DAI LIMITI ASSOLUTI (oltre 42 o sotto -3 \xC2\xB0""C): il verso che "
                 "lo peggiora e' sospeso. Qualcosa nella regolazione o nel cablaggio non va."));
  if (st.alarmTooCold)
    alarms.add(F("MOSTO TROPPO FREDDO: sotto -2 \xC2\xB0""C, o 2 \xC2\xB0""C sotto il setpoint "
                 "con l'antigelo acceso (ambiente gelato, riscaldatore debole o bloccato?)"));
  if (st.waitTimedOut)
    alarms.add(F("STEP PARTITO SENZA IL TARGET: in 24 ore il mosto non e' arrivato alla "
                 "temperatura dello step, e il suo tempo conta comunque (frigo o riscaldatore che "
                 "non ce la fanno?)"));
  if (st.alarmCrashSlow)
    alarms.add(F("COLD CRASH LENTO: nelle ultime 6 ore il mosto e' sceso meno della soglia "
                 "impostata (frigo che non ce la fa, porta aperta, sonda fuori dal mosto?)"));
  if (st.alarmCoolUncommanded)
    alarms.add(F("FREDDO NON COMANDATO: l'aria della camera resta fredda col frigo spento (rele' "
                 "del frigo incollato?). Il firmware non puo' aprirlo: controlla il rele'."));
  if (st.alarmAirProbe)
    alarms.add(F("sonda della camera non valida: i limiti sull'aria (gelo vicino alle pareti, "
                 "aria troppo calda) sono disattivati"));
  if (st.alarmCrashLoop)
    alarms.add(F("RIAVVII A CATENA: il firmware si e' bloccato piu' volte poco dopo l'avvio. "
                 "Modalita' ridotta, MQTT spento: si riaccende da solo dopo 30 minuti stabili. "
                 "Controlla il registro dei riavvii in Diagnostica."));
  if (st.heatLock) {
    alarms.add(heatLockText(st.heatLock));
    JsonObject hl = doc.createNestedObject("heatLock");
    hl["code"] = st.heatLock;
    hl["since"] = st.heatLockEpoch;
  }
  // Per primo: e' l'evento piu' grave e piu' raro, e va visto subito.
  if (st.alarmFailsafe)
    alarms.add(F("FAILSAFE: il loop di controllo si e' bloccato oltre il limite e il timer "
               "hardware ha spento i rele'. Il riposo del compressore e' stato riassunto. "
               "Riarma per prendere in carico l'evento"));
  if (st.alarmFailsafeInerte)
    alarms.add(F("il timer del failsafe non sta girando: la protezione contro il blocco "
               "del loop NON e' attiva"));
  if (st.alarmProbe) alarms.add(F("sonda di controllo guasta o assente"));
  if (st.alarmOutputs) alarms.add(F("driver uscite (MCP23008) in errore: uscite disabilitate"));
  if (st.alarmConfig) alarms.add(F("configurazione non valida: uscite disabilitate"));
  if (st.alarmStorage) alarms.add(F("scrittura su flash fallita: stato e registro NON salvati (controlla lo spazio)"));
  if (st.alarmThermal) alarms.add(F("scostamento termico: la temperatura non raggiunge il setpoint da ore (attuatore o impianto?)"));
  if (st.alarmProbeSuspect) alarms.add(F("sonda di controllo SOSPETTA: diverge da tutte le ausiliarie (contatto termico perso?) — controlla la sonda del mosto"));
  if (st.alarmRtc) alarms.add(F("orologio inaffidabile: impostare data/ora"));
  if (st.alarmGravity) alarms.add(F("densita' stantia: nessun avanzamento step"));
  if (st.alarmRampStall) alarms.add(F("rampa gradi/ora ferma: la temperatura non raggiunge il target, lo step non avanza"));
  if (st.alarmBattery) alarms.add(F("batteria iSpindel bassa"));
  if (st.alarmMultiGravity)
    alarms.add(F("piu' densimetri attivi senza selezione: scegli quale pilota la cotta in Calibrazione"));
  if (!st.armed) alarms.add(F("uscite DISARMATE: confermare il mapping rele'"));
  if (r_.cfg->usingDefaultUiPassword())
    alarms.add(F("PASSWORD DI DEFAULT attiva: cambiala in Impostazioni (OTA disattivato finche' non lo fai)"));
  else if (r_.cfg->uiPassword.length() == 0)
    alarms.add(F("NESSUNA PASSWORD impostata: chiunque sulla LAN ha accesso completo (OTA disattivato)"));
  else if (r_.cfg->uiPassword.length() < 8)
    alarms.add(F("PASSWORD interfaccia troppo corta (meno di 8 caratteri): cambiala in Impostazioni"));
  if (r_.cfg->softApPassword.length() < 8)
    alarms.add(F("PASSWORD della rete di configurazione non valida (meno di 8 caratteri): e' in uso "
                 "quella di fabbrica, cambiala in Impostazioni"));
  // Un profilo a densita' si fida delle letture di /ispindel, che non chiede login.
  if (!r_.cfg->ispindelToken.length() && profiloDensita())
    alarms.add(F("CODICE DEI DENSIMETRI vuoto con un profilo a densita': un dispositivo della rete "
                 "puo' inviare letture finte. Impostalo in Impostazioni -> iSpindel e nel densimetro"));
}

__attribute__((noinline)) void WebApi::statusRete(JsonDocument& doc, const AppStatus& st) {
  JsonObject net = doc.createNestedObject("net");
  net["ip"] = st.ip;
  net["rssi"] = st.rssi;
  net["ap"] = st.apUp;
  net["sta"] = st.staUp;
}

void WebApi::handleStatus() {
  if (!auth()) return;
  // 3.17.2: lo stato sull'heap e la risposta costruita a pezzi (statusSistema & C., ognuna
  // in una funzione propria). In un'unica funzione il frame era di 1.248 B, sotto c'era tutto
  // l'invio TCP, e la pagina chiede lo stato ogni 3 s: stack libero minimo misurato sulla
  // scheda 1 dopo 24 minuti di uso, 1.108 B.
  std::unique_ptr<AppStatus> stp(new (std::nothrow) AppStatus(c_().status()));
  if (!stp) {
    sendErr(503, F("memoria esaurita: riprova fra poco"));
    return;
  }
  const AppStatus& st = *stp;
  // 5 KB: il caso peggiore realistico (tutti gli allarmi + nomi lunghi) supera i 2 KB,
  // e i testi degli allarmi stanno in flash (F()), quindi ArduinoJson li COPIA nel pool:
  // con tutti gli allarmi attivi sono ~1,9 KB in piu', piu' il blocco del cold crash.
  // Transitorio, solo per la richiesta. Blocco contiguo libero misurato ~15 KB a riposo
  // (28/10/2026): da ricontrollare sul campo con heapMaxBlockMin dopo giorni d'uso.
  DynamicJsonDocument doc(5120);
  statusSistema(doc, st);
  statusCamera(doc, st);
  statusAllarmi(doc, st);
  statusRete(doc, st);
  // Guardia contro il troncamento SILENZIOSO. /api/status e' l'endpoint che la dashboard
  // interroga di continuo, e il commento sul dimensionamento qui sopra dichiara che il
  // caso peggiore realistico di UNA camera (tutti gli allarmi + nomi lunghi) supera gia'
  // i 2 KB. ArduinoJson tronca senza dire nulla: il frontend riceverebbe un JSON monco e
  // fallirebbe il parse, con un guasto molto piu' difficile da diagnosticare di un 500.
  // Stessa scelta gia' fatta in handleProbeDiag ("meglio un errore esplicito").
  if (doc.overflowed()) {
    sendErr(500, F("stato troppo grande (nomi profilo/cotta molto lunghi?)"));
    return;
  }
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebApi::handleGetConfig() {
  if (!auth()) return;
  DynamicJsonDocument doc(ConfigStore::kConfigDoc);  // derivata: vedi ConfigStore.h
  r_.cfgStore->toJson(*r_.cfg, doc, true);
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebApi::handlePutConfig() {
  if (!auth()) return;
  // Cambi che richiedono riavvio: rete, hostname, display, layout MQTT.
  AppConfig& c = *r_.cfg;
  // Si parte da una COPIA, si salva quella, e si applica al vivo SOLO se il salvataggio
  // riesce. Prima si mutava *r_.cfg in place: un save fallito (flash piena) lasciava RAM e
  // flash divergenti — password nuova già attiva (utente escluso con una password che
  // crede non salvata), campi letti live su una config mai persistita, e la successiva
  // taratura sonde committava per sempre la modifica rifiutata. static: fuori dallo stack
  // profondo del web (single-thread, nessuna rientranza), come startProfileCmd.
  static AppConfig next;
  {
    // Il doc di parsing (ConfigStore::kConfigDoc, 4 KB) vive SOLO in questo blocco: liberarlo
    // prima di cfgStore->save() — che in ConfigStore::save alloca un ALTRO doc della stessa
    // misura — evita di tenere due grossi buffer JSON vivi insieme sull'heap ESP8266
    // (frammentato). Stesso schema, e stessa cura, del salvataggio profili: la seconda alloc
    // poteva fallire e far
    // fallire in modo intermittente il salvataggio della configurazione.
    DynamicJsonDocument doc(ConfigStore::kConfigDoc);  // derivata: vedi ConfigStore.h
    if (deserializeJson(doc, server_.arg("plain"))) {
      sendErr(400, F("json non valido"));
      return;
    }
    // Locking ottimistico: se il client dichiara una revisione diversa da quella corrente,
    // un altro accesso ha scritto nel frattempo (tipicamente una taratura sonde) e questo PUT
    // — costruito su dati vecchi — la cancellerebbe in silenzio (last-write-wins). Si rifiuta.
    // Un client che non manda configRev (script) non è soggetto al controllo.
    if (!doc["configRev"].isNull() && (doc["configRev"] | (uint32_t)0) != c.configRev) {
      sendErr(409, F("configurazione cambiata da un altro accesso: ricarica la pagina e riprova"));
      return;
    }
    // L'unità di densità NON è solo un'etichetta: governa i confronti e le formule degli
    // step a densità (main.cpp/ProfileEngine) e il clamp di /ispindel. Cambiarla a profilo
    // in corso farebbe avanzare all'istante o bloccare per sempre uno step, su una scala
    // diversa da quella con cui la cotta è stata definita. Si rifiuta finché è attiva.
    {
      JsonVariantConst inUnit = doc["ispindel"]["unit"];
      if (!inUnit.isNull() && inUnit.as<String>() != c.gravityUnit) {
        // Non basta bloccare a profilo IN CORSA: anche con un profilo a densità fermo ma
        // CARICATO, cambiare unità e riavviarlo valuterebbe gli step su una scala diversa
        // da quella con cui gravityTarget è stato scritto (i profili non portano la loro
        // unità). Si blocca finché un profilo a densità è caricato; l'unità si cambia fra
        // una cotta e l'altra. (Il fix completo sarebbe marcare ogni profilo con la sua unità.)
        const ax::ProfileEngine* eng = c_().engine;
        if ((eng && eng->mode() == ax::RunMode::Profile) || profiloDensita()) {
          sendErr(409, F("unita' densita' non modificabile con un profilo a densita' caricato: cambiala fra una cotta e l'altra"));
          return;
        }
      }
    }
    // Le password NON vanno troncate in silenzio (il cap generale di ConfigStore lo farebbe):
    // una uiPassword troncata escluderebbe l'utente dall'interfaccia (lockout). Si rifiuta con
    // un errore esplicito. "***" = invariata (setSecret), non va controllata.
    // Minimo 8 caratteri: per la rete di configurazione e' il minimo del WPA2 (sotto, la rete
    // partirebbe con la password di fabbrica); per l'interfaccia, la stessa soglia.
    // Solo ASCII stampabile: il WPA2 lo impone alla rete, e la password dell'interfaccia
    // viaggia nell'intestazione X-Ax-Password dell'OTA, che non porta caratteri accentati.
    // Niente spazi in testa o in coda: l'intestazione HTTP li toglie, e la password
    // dell'OTA non coinciderebbe piu' con quella salvata.
    auto asciiStampabile = [](const String& s) {
      for (size_t i = 0; i < s.length(); i++)
        if ((uint8_t)s[i] < 0x20 || (uint8_t)s[i] > 0x7E) return false;
      return !s.length() || (s[0] != ' ' && s[s.length() - 1] != ' ');
    };
    // Nome della scheda: diventa l'SSID della rete di configurazione (nome + "-setup",
    // max 32 caratteri WiFi) e il nome .local. Fuori regola, la rete di configurazione non
    // partirebbe: l'unica via di rientro senza la rete di casa.
    {
      JsonVariantConst hn = doc["system"]["hostname"];
      if (!hn.isNull()) {
        const String h = hn.as<String>();
        bool ok = h.length() >= 1 && h.length() <= 26 && h[0] != '-' && h[h.length() - 1] != '-';
        for (size_t i = 0; ok && i < h.length(); i++)
          ok = (h[i] >= 'a' && h[i] <= 'z') || (h[i] >= '0' && h[i] <= '9') || h[i] == '-';
        if (!ok) {
          sendErr(400, F("nome della scheda: da 1 a 26 caratteri, solo lettere minuscole, cifre e "
                         "trattini (non in testa o in coda)"));
          return;
        }
      }
    }
    {
      JsonVariantConst pw = doc["system"]["uiPassword"];
      // Qualunque valore diverso da "***" e' una richiesta di cambio, ANCHE se coincide con
      // la password attuale: prima un valore uguale passava in silenzio (200) e uno diverso
      // no (403), e con un cookie rubato si poteva provare password su password.
      const bool cambiaPw = !pw.isNull() && pw.as<String>() != "***";
      if (cambiaPw) {
        // Per cambiarla serve quella ATTUALE: un cookie di sessione rubato o lasciato aperto
        // non deve bastare a prendersi la scheda. Senza password attuale (vuota) non si chiede.
        // `| ""`, non as<String>(): su un campo assente as<String>() darebbe "null".
        // Prima di ogni altro controllo: nessuna risposta deve dipendere dal valore inviato
        // finche' la password attuale non e' verificata.
        const char* attuale = doc["system"]["currentPassword"] | "";
        if (c.uiPassword.length() && pwLocked()) return;
        if (c.uiPassword.length() && c.uiPassword != attuale) {
          pwFail();
          sendErr(403, F("password attuale errata: serve per impostarne una nuova"));
          return;
        }
        const size_t n = pw.as<String>().length();
        if (pw.as<String>() != c.uiPassword &&
            (n < 8 || n > 64 || !asciiStampabile(pw.as<String>()))) {
          sendErr(400, F("password interfaccia: da 8 a 64 caratteri, senza lettere accentate e "
                         "senza spazi in testa o in coda"));
          return;
        }
      }
      JsonVariantConst ap = doc["network"]["softApPassword"];
      if (!ap.isNull() && ap.as<String>() != "***") {
        const size_t n = ap.as<String>().length();
        if (n < 8 || n > 63 || !asciiStampabile(ap.as<String>())) {
          sendErr(400, F("password della rete di configurazione: da 8 a 63 caratteri, senza "
                         "lettere accentate e senza spazi in testa o in coda"));
          return;
        }
      }
    }
    next = c;
    r_.cfgStore->fromJson(&next, doc);
    // pwdAuto non si imposta da fuori: resta com'era, e cade quando l'utente sceglie la
    // sua password dell'interfaccia (da li' in poi l'OLED non la mostra piu').
    next.pwdAuto = (next.uiPassword == c.uiPassword) ? c.pwdAuto : false;
    // Campi numerici riportati nei limiti da fromJson: la risposta li elenca (la UI li
    // mostra e rilegge la configurazione).
    corretti_ = ConfigStore::correctedFields(doc, next);
    // La taratura appartiene alla sonda FISICA, non alla posizione: se in una posizione
    // cambia la sonda e la correzione non e' stata toccata in questo salvataggio, segue la
    // sonda (spostata da un'altra posizione) o si azzera (sonda nuova). Prima una sonda
    // sostituita si portava dietro in silenzio la correzione della vecchia, fino a 5 °C.
    offsetMossi_ = "";
    bool mossi[ax::kMaxProbes];
    ax::followProbeOffsets(
        c.probeId, c.probeOffset, next.probeId, next.probeOffset, ax::kMaxProbes,
        [](const String& a, const String& b) { return a.equalsIgnoreCase(b); },
        [](const String& a) { return a.length() == 0; }, mossi);
    for (uint8_t i = 0; i < ax::kMaxProbes; i++) {
      if (!mossi[i]) continue;
      if (offsetMossi_.length()) offsetMossi_ += ',';
      offsetMossi_ += i + 1;
    }
  }  // doc liberato: da qui in poi nessun grosso buffer JSON del gestore è vivo
  // Unicita' degli ID ROM delle sonde. Un duplicato degrada il cross-check di
  // plausibilita' (l'ausiliaria legge identica al mosto, quindi minDiff ~ 0 e mai
  // "sospetta"): quella configurazione non ha alcun uso legittimo, si rifiuta.
  for (uint8_t i = 0; i < ax::kMaxProbes; i++) {
    if (!next.probeId[i].length()) continue;  // slot vuoto: piu' slot vuoti sono leciti
    for (uint8_t j = (uint8_t)(i + 1); j < ax::kMaxProbes; j++) {
      if (next.probeId[i].equalsIgnoreCase(next.probeId[j])) {
        sendErr(400, F("la stessa sonda e' assegnata a due slot: ogni DS18B20 va usata una "
                     "volta sola"));
        return;
      }
    }
  }
  next.valid = true;
  next.configRev = c.configRev + 1;  // nuova revisione (fromJson non tocca configRev)
  if (!r_.cfgStore->save(next)) {
    sendErr(500, F("salvataggio fallito"));
    return;
  }
  // Cambi attivi solo al riavvio: il riavvio NON parte da solo (a fermentazione in corso
  // spegnerebbe i relè e farebbe ripartire la pausa del compressore). La UI lo propone.
  // In sospeso finche' le impostazioni differiscono da quelle dell'avvio: rimettere i
  // valori di prima chiude il sospeso.
  const bool rebootNeeded = netSig(c) != netSig(next);
  rebootPending_ = netSig(next) != bootNetSig_;
  c = next;  // applica al vivo SOLO ora che la scrittura è riuscita
  r_.applyConfigLive();
  if (r_.logEvent) r_.logEvent((uint8_t)Ev::Config, 0, 0, 0);
  // La nuova revisione va restituita: senza, il client resterebbe sulla vecchia e il
  // salvataggio successivo fallirebbe con un 409 falso.
  sendJson(200, String("{\"ok\":true,\"configRev\":") + c.configRev +
                    ",\"reboot\":false,\"rebootNeeded\":" +
                    (rebootNeeded && rebootPending_ ? "true" : "false") +
                    ",\"rebootPending\":" + (rebootPending_ ? "true" : "false") +
                    ",\"corrected\":[" + corretti_ + "]" +
                    ",\"probeOffsetsMoved\":[" + offsetMossi_ + "]}");
  corretti_ = "";
  offsetMossi_ = "";
}

void WebApi::handleGetProfile() {
  if (!auth()) return;
  DynamicJsonDocument doc(ProfileStore::kProfileDoc);  // derivata: vedi ProfileStore.h
  doc["name"] = c_().activeProfileName ? *c_().activeProfileName : String("");
  ProfileStore::stepsToJson(c_().engine->steps(), c_().engine->stepCount(), doc);
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebApi::handleProfilesList() {
  if (!auth()) return;
  DynamicJsonDocument doc(3072);
  JsonArray arr = doc.createNestedArray("profiles");
  if (!r_.profStore->list(arr)) {
    sendErr(503, F("memoria esaurita: riprova fra poco"));
    return;
  }
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebApi::handleProfileById(int method) {
  if (!auth()) return;
  // Id letto dall'URI, non da pathArg(): vedi segAfter in WebApi.h (panic
  // pre-autenticazione sul percorso letterale "/api/profiles/{}").
  const String idTxt = segAfter("/api/profiles/");
  if (!idTxt.length()) {
    sendErr(404, F("profilo inesistente"));
    return;
  }
  int id = idTxt.toInt();
  if (id < 1 || id > ProfileStore::kMaxProfiles) {
    sendErr(400, F("id profilo non valido (1..20)"));
    return;
  }
  ProfileStore::StepBuf buf;  // sull'heap (3.17.3): vedi ProfileStore::StepBuf
  if (!buf) {
    sendErr(503, F("memoria esaurita: riprova fra poco"));
    return;
  }
  ax::StepDef* steps = buf.get();
  if (method == HTTP_GET) {
    uint8_t n = 0;
    String name;
    if (!r_.profStore->load((uint8_t)id, &name, steps, &n)) {
      // Distinzione (come il fix C30 su list()): un profilo che ESISTE ma non si è letto
      // (OOM/corrotto) non deve fingersi assente, o il client lo crederebbe libero.
      if (r_.profStore->exists((uint8_t)id))
        sendErr(500, F("profilo illeggibile (memoria insufficiente o file corrotto)"));
      else
        sendErr(404, F("profilo inesistente"));
      return;
    }
    DynamicJsonDocument doc(ProfileStore::kProfileDoc);  // derivata: vedi ProfileStore.h
    doc["id"] = id;
    doc["name"] = name;
    ProfileStore::stepsToJson(steps, n, doc);
    String body;
    serializeJson(doc, body);
    sendJson(200, body);
    return;
  }
  if (method == HTTP_DELETE) {
    if (!r_.profStore->remove((uint8_t)id)) {
      sendErr(404, F("profilo inesistente"));
      return;
    }
    sendOk();
    return;
  }
  // PUT: crea o aggiorna (stesso buffer sull'heap del GET).
  String name;
  uint8_t n = 0;
  {
    // Il doc di parsing (ProfileStore::kProfileDoc) vive SOLO in questo blocco: liberarlo prima di save() evita
    // di tenere due grossi buffer JSON vivi insieme sull'heap ESP8266 (causa accertata dei
    // profili "{}" e dei "salvataggio fallito" intermittenti su heap frammentato).
    DynamicJsonDocument doc(ProfileStore::kProfileDoc);  // derivata: vedi ProfileStore.h
    if (deserializeJson(doc, server_.arg("plain"))) {
      sendErr(400, F("json non valido"));
      return;
    }
    name = doc["name"] | String("");
    if (name.length() == 0) {
      sendErr(400, F("nome profilo mancante"));
      return;
    }
    // Troncare in silenzio faceva credere salvato un nome diverso da quello scritto,
    // e due profili potevano finire con lo stesso nome accorciato. String misura BYTE.
    if (name.length() > 32) {
      sendErr(400, F("nome troppo lungo (max 32 byte)"));
      return;
    }
    n = ProfileStore::stepsFromJson(doc, steps);
  }
  PGM_P err = validateSteps(steps, n);
  if (err) {
    sendErr(400, FPSTR(err));
    return;
  }
  if (!r_.profStore->save((uint8_t)id, name, steps, n)) {
    sendErr(500, F("salvataggio fallito"));
    return;
  }
  sendOk();
}

void WebApi::handlePutProfile() {
  if (!auth()) return;
  // Anche a profilo COMPLETED il setpoint mantenuto deriva dagli step: si modifica
  // solo a modalità profilo disattivata.
  if (c_().engine->mode() == ax::RunMode::Profile) {
    sendErr(409, F("profilo in corso o completato: fermarlo prima di modificarlo"));
    return;
  }
  ProfileStore::StepBuf buf;  // sull'heap (3.17.3): vedi ProfileStore::StepBuf
  if (!buf) {
    sendErr(503, F("memoria esaurita: riprova fra poco"));
    return;
  }
  ax::StepDef* steps = buf.get();
  uint8_t n = 0;
  String name;
  {
    // doc di parsing liberato prima di saveActive(): un solo grosso buffer JSON alla
    // volta sull'heap (vedi handleProfileById).
    DynamicJsonDocument doc(ProfileStore::kProfileDoc);  // derivata: vedi ProfileStore.h
    if (deserializeJson(doc, server_.arg("plain"))) {
      sendErr(400, F("json non valido"));
      return;
    }
    n = ProfileStore::stepsFromJson(doc, steps);
    name = doc["name"] | (c_().activeProfileName ? *c_().activeProfileName : String(""));
  }
  PGM_P err = validateSteps(steps, n);
  if (err) {
    sendErr(400, FPSTR(err));
    return;
  }
  if (name.length() > 32) {
    sendErr(400, F("nome troppo lungo (max 32 byte)"));
    return;
  }
  // Il motore si aggiorna SOLO dopo che la scrittura su flash è riuscita: applicarlo
  // prima significherebbe che una richiesta rifiutata lascia in RAM un piano diverso
  // da quello su /profile.json, e al riavvio la fermentazione cambierebbe da sola.
  if (!r_.profStore->saveActive(name, steps, n)) {
    sendErr(500, F("salvataggio fallito"));
    return;
  }
  c_().engine->setSteps(steps, n);
  if (c_().activeProfileName) *c_().activeProfileName = name;
  sendOk();
}

// Cold crash su richiesta (dalla 3.11.0, scelte dell'utente): una discesa a 3 o 1,5 °C ogni
// 6 ore fino al target, poi il target mantenuto finche' l'utente non ferma il profilo. La
// velocita' non cambia il risultato in cieco (Brulosophy), ma una discesa graduale limita
// il risucchio dal gorgogliatore in un fermentatore non in pressione. Proposta di default
// 2 °C: sotto i 2-4 °C il lievito ancora in sospensione si danneggia (Escarpment, fonti in
// docs/PROFILI.md).
void WebApi::handleColdCrash() {
  if (!auth()) return;
  float target = 2.0f;
  float cph = kColdCrashCph;
  float minDrop = -1.0f;  // assente: meta' di quanto il programma scende in 6 ore
  bool anteprima = false;
  if (server_.arg("plain").length()) {
    StaticJsonDocument<192> doc;
    if (deserializeJson(doc, server_.arg("plain"))) {
      sendErr(400, F("json non valido"));
      return;
    }
    if (!doc["targetC"].isNull()) {
      if (!doc["targetC"].is<float>() && !doc["targetC"].is<int>()) {
        sendErr(400, F("temperatura finale non numerica"));
        return;
      }
      target = doc["targetC"];
    }
    if (!doc["minDropC"].isNull()) {
      if (!doc["minDropC"].is<float>() && !doc["minDropC"].is<int>()) {
        sendErr(400, F("soglia di discesa non numerica"));
        return;
      }
      minDrop = doc["minDropC"];
    }
    if (!doc["cph"].isNull()) {
      if (!doc["cph"].is<float>() && !doc["cph"].is<int>()) {
        sendErr(400, F("velocita' di discesa non numerica"));
        return;
      }
      cph = doc["cph"];
    }
    anteprima = doc["preview"] | false;
  }
  // Le due velocita' previste: 3 °C ogni 6 ore o 1,5 °C ogni 6 ore.
  if (fabsf(cph - kColdCrashCph) < 0.01f) cph = kColdCrashCph;
  else if (fabsf(cph - kColdCrashSlowCph) < 0.01f) cph = kColdCrashSlowCph;
  else {
    sendErr(400, F("velocita' di discesa: 0,5 o 0,25 \xC2\xB0""C/h"));
    return;
  }
  const float programma6h = cph * 6.0f;  // quanto scende il setpoint in 6 ore
  if (minDrop < 0.0f) minDrop = programma6h / 2.0f;
  if (isnan(target) || target < -1.0f || target > 12.0f) {
    sendErr(400, F("temperatura finale fuori range (-1..12 \xC2\xB0""C)"));
    return;
  }
  // Soglia fino a 2/3 di quanto il programma scende in 6 ore (2 °C a 0,5 °C/h, 1 °C a
  // 0,25 °C/h): vicino al programma l'allarme si accenderebbe e spegnerebbe anche con il
  // frigo in regola, perche' il mosto segue la rampa a gradini di tolleranza.
  if (isnan(minDrop) || minDrop < 0.25f || minDrop > programma6h * 2.0f / 3.0f + 0.001f) {
    sendErr(400, cph < kColdCrashCph
                     ? F("soglia di discesa fuori range (0,25..1 \xC2\xB0""C ogni 6 ore a 0,25 \xC2\xB0""C/h)")
                     : F("soglia di discesa fuori range (0,25..2 \xC2\xB0""C ogni 6 ore a 0,5 \xC2\xB0""C/h)"));
    return;
  }
  if (c_().engine->snapshot().delayedStartEpoch != 0) {
    sendErr(409, F("partenza ritardata in attesa: annullala prima del cold crash"));
    return;
  }
  ProbeBus::Reading ctl = r_.probes->reading(*c_().controlProbe);
  if (!ctl.valid) {
    // Senza sonda il frigo lavorerebbe alla cieca: una discesa non controllata può
    // congelare la birra.
    sendErr(409, F("sonda di controllo non valida: impossibile avviare il cold crash"));
    return;
  }
  const float ora = ctl.c;
  // Il cold crash scende: con il caldo disattivato una "salita" terrebbe solo il frigo
  // fermo. Un mosto gia' sotto il target si tiene con il termostato.
  if (ora < target - r_.cfg->toleranceC) {
    sendErr(409, F("il mosto e' gia' sotto la temperatura di cold crash: scegline una piu' bassa "
                   "o usa il termostato"));
    return;
  }

  // Un solo step: rampa a 0,5 o 0,25 °C/h dalla temperatura misurata al target. La rampa
  // la fa il SETPOINT, calcolato sul tempo: non serve che la sonda risolva 0,25 °C/h
  // istante per istante (a 0,25 °C/h il setpoint scende di 0,0625 °C, un passo della DS18B20,
  // ogni 15 minuti), e la massa del mosto media i cicli del frigo. La rampa °C/h finisce solo quando il mosto entra nella tolleranza del target,
  // quindi la fine dello step e' il target raggiunto; poi il profilo completato mantiene il
  // target finche' l'utente non interviene. Nessun timeout: se il frigo non ce la fa lo
  // dice l'allarme di discesa lenta, e la temperatura resta comunque al minimo possibile.
  ax::StepDef steps[1];
  steps[0] = ax::StepDef{};
  steps[0].type = ax::StepType::RampRate;
  steps[0].enabled = true;
  steps[0].targetC = target;
  steps[0].degPerHour = cph;
  steps[0].fromCurrent = true;
  steps[0].allowHeat = false;  // il riscaldatore non contrasta la discesa (salvo antigelo)
  const uint8_t n = 1;
  const float gradi = ora > target ? ora - target : 0.0f;
  const uint32_t sec = (uint32_t)(gradi / cph * 3600.0f);

  // Il piano viene restituito sempre: la conferma nella pagina si costruisce da qui,
  // così non può descrivere una discesa diversa da quella che il firmware esegue.
  DynamicJsonDocument out(384);
  out["ok"] = true;
  out["preview"] = anteprima;
  out["fromC"] = serialized(String(ora, 1));
  out["targetC"] = serialized(String(target, 1));
  out["cph"] = serialized(String(cph, 2));
  out["minDropC"] = serialized(String(minDrop, 1));
  out["totalSec"] = sec;  // durata nominale della discesa, se il frigo tiene il passo

  if (!anteprima) {
    // Prima la flash, poi il motore: fermare il profilo e scoprire solo dopo che la
    // scrittura è fallita lascerebbe la cotta senza controllo di temperatura, in
    // silenzio. È la stessa invariante di handlePutProfile e di startProfileCmd.
    const String nome = kColdCrashName;
    if (!r_.profStore->saveActive(nome, steps, n)) {
      sendErr(500, F("salvataggio fallito: il profilo in corso non è stato toccato"));
      return;
    }
    c_().engine->stop();
    c_().engine->setSteps(steps, n);
    if (c_().activeProfileName) *c_().activeProfileName = nome;
    // Si avvia il motore direttamente, senza passare dal comando di avvio profilo: quello
    // azzererebbe l'OG della cotta, e con essa attenuazione e ABV finali.
    if (!c_().engine->startProfile(r_.time->nowEpoch(), ctl.c, ctl.valid, 0)) {
      sendErr(400, F("avvio del cold crash fallito"));
      return;
    }
    if (c_().beginCrash) c_().beginCrash(target, minDrop, cph);
    // Il piano e' gia' salvato come profilo attivo: se lo stato del motore non si salva, al
    // riavvio ripartirebbe con l'indice dello step di prima applicato al piano nuovo.
    if (!r_.persistNow()) {
      sendErr(500, F("cold crash avviato ma lo stato NON e' salvato su flash: un riavvio lo "
                     "lascerebbe incoerente. Controlla lo spazio e ripeti il comando"));
      return;
    }
  }
  String body;
  serializeJson(out, body);
  sendJson(200, body);
}

void WebApi::handleProfileCmd(const String& cmd) {
  if (!auth()) return;
  uint32_t now = r_.time->nowEpoch();
  ProbeBus::Reading ctl = r_.probes->reading(*c_().controlProbe);
  bool ok = true;
  if (cmd == "start") {
    // Un profilo GIÀ in corso non va riavviato in silenzio: azzererebbe OG/giorni/step
    // della cotta. La stessa protezione del ramo MQTT (C07), qui mancante sul ramo HTTP.
    if (c_().engine->running()) {
      sendErr(409, F("profilo gia' in corso: fermalo prima di riavviarlo"));
      return;
    }
    uint32_t delayedAt = 0;
    int profileId = 0;
    String batch = "";
    if (server_.arg("plain").length()) {
      DynamicJsonDocument doc(512);  // heap: il percorso di start è profondo di stack
      if (deserializeJson(doc, server_.arg("plain"))) {
        sendErr(400, F("json non valido"));  // mai avviare col profilo sbagliato in silenzio
        return;
      }
      delayedAt = doc["delayedStartAt"] | 0;
      profileId = doc["profileId"] | 0;
      batch = doc["batchName"] | String("");
      if (batch.length() > 40) {
        unsigned cut = 40;  // non spezzare una sequenza UTF-8: arretra all'inizio del carattere
        while (cut > 0 && ((uint8_t)batch[cut] & 0xC0) == 0x80) cut--;
        batch = batch.substring(0, cut);
      }
    }
    int rc = c_().startProfile(profileId, batch, delayedAt);
    if (rc == 404) {
      sendErr(404, F("profilo inesistente"));
      return;
    }
    if (rc == 500) {
      sendErr(500, F("scrittura del profilo attivo fallita"));
      return;
    }
    if (rc != 0) {
      sendErr(400, F("nessuno step abilitato"));
      return;
    }
  } else if (cmd == "stop") {
    c_().engine->stop();
  } else if (cmd == "pause") {
    c_().engine->pause(now);
  } else if (cmd == "resume") {
    c_().engine->resume(now);
  } else if (cmd == "skip") {
    ok = c_().engine->skip(now, ctl.c, ctl.valid);
    if (!ok) {
      sendErr(400, F("nessun profilo in corso"));
      return;
    }
  } else if (cmd == "confirm") {
    ok = c_().engine->confirm(now, ctl.c, ctl.valid);
    if (!ok) {
      sendErr(409, F("nessuno step in attesa di conferma"));
      return;
    }
  }
  sendSaved(r_.persistNow());
}

void WebApi::handleSetpoint() {
  if (!auth()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server_.arg("plain")) || doc["value"].isNull()) {
    sendErr(400, F("atteso {\"value\": <float>}"));
    return;
  }
  float v = doc["value"];
  // Floor a -1 (antigelo), coerente con validateSteps e cold crash: sotto -1 l'overshoot
  // del frigo porta la birra verso il congelamento. Prima accettava fino a -10.
  // isfinite: un NaN passerebbe il solo controllo di range (ogni confronto con NaN è falso)
  // e finirebbe in /api/status come "nan", rendendo illeggibile l'intera risposta.
  if (!isfinite(v) || v < -1 || v > 40) {
    sendErr(400, F("setpoint fuori range (-1..40)"));
    return;
  }
  // overrideSetpoint è un no-op a modalità Off o a profilo completato: rispondere 200 lì
  // farebbe credere applicato un setpoint che resta invariato. Meglio un 409 esplicito.
  ax::RunMode m = c_().engine->mode();
  bool delayedWait = c_().engine->snapshot().delayedStartEpoch != 0;
  if (m == ax::RunMode::Off ||
      (m == ax::RunMode::Profile && (c_().engine->completed() || delayedWait))) {
    sendErr(409, F("nessun setpoint attivo da modificare: avvia un profilo o il termostato"));
    return;
  }
  c_().engine->overrideSetpoint(v);
  sendSaved(r_.persistNow());
}

void WebApi::handleThermostat() {
  if (!auth()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, F("json non valido"));
    return;
  }
  bool on = doc["on"] | false;
  float sp = doc["setpoint"] | c_().engine->thermostatSetpoint();
  // Accendendo si valida il setpoint per dare un 400 esplicito. (Spegnendo non si rifiuta: il
  // clamp in setThermostat sotto porta comunque a range QUALSIASI valore — anche un +inf da
  // "1e40" del client → 40 — quindi niente valore pericoloso né JSON di /status rotto.)
  if (on && (!isfinite(sp) || sp < -1 || sp > 40)) {
    sendErr(400, F("setpoint fuori range (-1..40)"));
    return;
  }
  // Passare al termostato mentre un profilo è in CORSA azzera lo snapshot del motore (setThermostat
  // fa s_ = EngineSnapshot{}): step/giorni/OG/batch persi e cotta abbandonata in SILENZIO — e questo
  // vale ANCHE spegnendo (on=false), non solo accendendo. Per fermare una cotta si usa lo stop del
  // profilo. Il guard copre entrambi i versi. (Il gemello MQTT climate_mode=auto agisce solo da Off.)
  if (c_().engine->running()) {
    sendErr(409, F("profilo in corso: fermalo (stop profilo) prima di usare il termostato"));
    return;
  }
  // setThermostat clampa il setpoint a [-1,40] da qualunque sorgente: un client non può memorizzare
  // un valore fuori range o non-finito (che romperebbe /status via serialized(String(sp,2))).
  c_().engine->setThermostat(on, sp);
  sendSaved(r_.persistNow());
}

void WebApi::handleArm() {
  if (!auth()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, F("json non valido"));
    return;
  }
  uint8_t cool = doc["cool"] | 0;
  uint8_t heat = doc["heat"] | 0;
  if (cool == heat || cool > 8 || heat > 8) {
    sendErr(400, F("mapping non valido: cool e heat devono essere rele' distinti (1..8, 0=nessuno)"));
    return;
  }
  if (!r_.cfg->valid) {
    sendErr(409, F("configurazione non valida: correggila prima di armare"));
    return;
  }
  if (!c_().arm(cool, heat)) {
    sendErr(409, F("impossibile armare: esci prima dalla modalita' test"));
    return;
  }
  sendSaved(r_.persistNow());
}

void WebApi::handleIspindel() {
  // Nessuna auth di sessione: e' lo iSpindel a pubblicare (formato Generic HTTP JSON).
  // Difese contro iniezioni da chiunque sulla LAN: token opzionale + clamp di plausibilita'.
  if (!hostOk()) return;
  // Senza codice l'unica difesa contro una pagina web che invia letture finte dalla LAN e'
  // il formato: un modulo da un altro sito non puo' dichiararsi JSON senza preflight, a cui
  // la scheda non risponde. iSpindel, GravityMon e la ricetta di Home Assistant inviano JSON.
  if (!r_.cfg->ispindelToken.length()) {
    String ct = server_.header("Content-Type");
    ct.toLowerCase();
    if (!ct.startsWith(F("application/json"))) {
      sendErr(415, F("serve Content-Type: application/json (oppure imposta il codice dei densimetri)"));
      return;
    }
  }
  DynamicJsonDocument doc(768);
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, F("json non valido"));
    return;
  }
  if (r_.cfg->ispindelToken.length()) {
    if (tokPen_.locked(ipKey(), millis())) {
      sendWait(tokPen_.waitSec(ipKey(), millis()));
      return;
    }
    String tok = doc["token"] | String("");
    if (tok != r_.cfg->ispindelToken) {
      tokPen_.fail(ipKey(), millis());
      sendErr(401, F("token non valido"));
      return;
    }
    tokPen_.success(ipKey());
  }
  if (doc["gravity"].isNull()) {
    sendErr(400, F("campo gravity mancante"));
    return;
  }
  float gravity = doc["gravity"];
  float temp = doc["temperature"] | 0.0f;
  // Unita' dichiarata (iSpindel: "C", "F" o "K"): un densimetro o un ponte in °F a 65 °F
  // veniva rifiutato per intero, e sotto i 50 °F registrato come °C sbagliati.
  if (!doc["temperature"].isNull()) {
    const char* tu = doc["temp_units"] | "C";
    if ((uint8_t)tu[0] == 0xC2 && (uint8_t)tu[1] == 0xB0) tu += 2;  // "°F": si salta il simbolo
    if (tu[0] == 'F' || tu[0] == 'f') temp = (temp - 32.0f) * 5.0f / 9.0f;
    else if (tu[0] == 'K' || tu[0] == 'k') temp -= 273.15f;
  }
  float batt = doc["battery"] | 0.0f;
  float angle = doc["angle"] | 0.0f;
  int rssi = doc["RSSI"] | (int)(doc["rssi"] | 0);
  // Range plausibili (SG 0.98..1.20; Plato -1..40; temperatura -10..50): fuori = scartato,
  // cosi' una lettura falsa non sposta l'avanzamento di uno step a densita'.
  bool plato = r_.cfg->gravityUnit == "plato";
  float gmin = plato ? -1.0f : 0.90f, gmax = plato ? 45.0f : 1.30f;
  if (gravity < gmin || gravity > gmax || temp < -10.0f || temp > 50.0f) {
    sendErr(400, F("valori fuori range plausibile"));
    return;
  }
  // Anche angolo e batteria vanno filtrati: un valore fuori scala (o NaN/infinito da
  // un JSON malformato) finirebbe in /api/status come numero non rappresentabile,
  // rendendo illeggibile l'intera risposta e congelando la UI.
  if (!isfinite(angle) || angle < 0.0f || angle > 90.0f) angle = 0.0f;
  if (!isfinite(batt) || batt < 0.0f || batt > 6.0f) batt = 0.0f;
  if (rssi > 0 || rssi < -120) rssi = 0;
  // ID e nome identificano il densimetro: con due strumenti accesi servono a non
  // attribuire a uno la lettura dell'altro (fondamentale in calibrazione).
  uint32_t devId = doc["ID"] | (uint32_t)0;
  String devName = doc["name"] | String("");
  if (devName.length() > 32) {
    unsigned cut = 32;  // non spezzare una sequenza UTF-8: arretra all'inizio del carattere
    while (cut > 0 && ((uint8_t)devName[cut] & 0xC0) == 0x80) cut--;
    devName = devName.substring(0, cut);
  }
  r_.ispindelData(gravity, temp, batt, angle, rssi, devId, devName);
  sendJson(200, "{\"result\":\"ok\"}");
}

void WebApi::handleOg() {
  if (!auth()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, F("json non valido"));
    return;
  }
  float og = doc["og"] | 0.0f;  // 0 (o assente) = azzera -> ricattura automatica
  if (og > 0) {
    // Clamp di plausibilita' (come /ispindel): evita che un refuso tipo 1010 al posto
    // di 1.010 avveleni le metriche di fermentazione.
    bool plato = r_.cfg->gravityUnit == "plato";
    float gmin = plato ? 1.0f : 1.0f, gmax = plato ? 40.0f : 1.20f;
    if (og < gmin || og > gmax) {
      sendErr(400, plato ? F("OG fuori range (1..40 P)") : F("OG fuori range (1.000..1.200 SG)"));
      return;
    }
  }
  c_().setOg(og);
  sendOk();
}

// --- Assistente di calibrazione del densimetro -------------------------------
// L'iSpindel misura un ANGOLO e lo converte in densità con un polinomio che vive
// a bordo suo. Qui raccogliamo le coppie (angolo osservato, densità misurata dal
// densimetro): la regressione la fa la UI in JavaScript e restituisce il polinomio
// da incollare nel portale dell'iSpindel.
//
// Vincolo centrale: OGNI PUNTO DEVE VENIRE DA UNA LETTURA DIVERSA. L'iSpindel
// trasmette ogni ~15 minuti; registrando più punti fra due trasmissioni si
// otterrebbero densità diverse associate allo STESSO angolo, e la regressione
// sarebbe priva di senso. Per questo il punto porta con sé l'epoch della lettura
// da cui proviene, e il client deve dichiarare quale lettura considera "vecchia".

// Oltre questa età il dato non descrive più la soluzione in cui si trova ADESSO
// lo strumento: con l'intervallo tipico di 15 min, 25 concede un ciclo di margine.
static constexpr int64_t kCalibMaxAgeSec = 25 * 60;

void WebApi::handleCalibGet() {
  if (!auth()) return;
  DynamicJsonDocument doc(3072);  // 12 punti con nomi densimetro fino a 32 byte + elenco device
  uint32_t now = r_.time->nowEpoch();
  bool has = r_.gravityAny->hasValue();
  doc["hasReading"] = has;
  if (has) {
    int64_t age = r_.gravityAny->signedAgeSec(now);
    doc["angle"] = serialized(String(r_.gravityAny->angle(), 2));
    // Densità GREZZA: quella corretta da gravityCorrection non serve a calibrare.
    doc["gravity"] = serialized(String(r_.gravityAny->rawGravity(), 4));
    doc["temp"] = serialized(String(r_.gravityAny->temperature(), 1));
    doc["ageSec"] = (double)age;
    doc["readingEpoch"] = r_.gravityAny->updatedEpoch();
    // Un'età negativa (lettura "nel futuro" dopo un salto d'orologio) non è usabile.
    doc["usable"] = age >= 0 && age <= kCalibMaxAgeSec;
    doc["correction"] = serialized(String(r_.cfg->gravityCorrection[0], 4));
    doc["deviceId"] = r_.gravityAny->deviceId();
    doc["deviceName"] = r_.calibDeviceName ? *r_.calibDeviceName : String("");
  }
  doc["unit"] = r_.cfg->gravityUnit;
  doc["maxPoints"] = CalibStore::kMaxPoints;
  doc["activeDevice"] = r_.cfg->gravityDevice[0];
  // Elenco dei densimetri già visti: alimenta la tendina di scelta, così l'utente
  // non deve ricordare (o andare a rileggere) il nome configurato nello strumento.
  JsonArray devs = doc.createNestedArray("devices");
  if (r_.devices) {
    for (uint8_t i = 0; i < r_.devices->count(); i++) {
      const KnownDevice* d = r_.devices->device(i);
      if (!d) continue;
      JsonObject o = devs.createNestedObject();
      o["name"] = d->name;
      o["id"] = d->id;
      o["lastSeen"] = d->lastSeen;
    }
  }
  JsonArray arr = doc.createNestedArray("points");
  if (r_.calib) {
    for (uint8_t i = 0; i < r_.calib->count(); i++) {
      const CalibPoint* p = r_.calib->point(i);
      if (!p) continue;
      JsonObject o = arr.createNestedObject();
      o["angle"] = serialized(String(p->angle, 2));
      o["gravity"] = serialized(String(p->gravity, 4));
      o["temp"] = serialized(String(p->temp, 1));
      o["epoch"] = p->epoch;
      o["plato"] = p->plato;
      o["deviceId"] = p->deviceId;
      o["deviceName"] = p->deviceName;
    }
  }
  if (doc.overflowed()) { sendErr(500, F("calibrazione troppo grande")); return; }  // no punti persi in silenzio
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebApi::handleCalibAdd() {
  if (!auth()) return;
  if (!r_.calib) {
    sendErr(500, F("archivio calibrazione non disponibile"));
    return;
  }
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, F("json non valido"));
    return;
  }
  if (doc["gravity"].isNull()) {
    sendErr(400, F("densita' misurata mancante"));
    return;
  }
  float g = doc["gravity"];
  // Stessi limiti di plausibilità dell'ingest iSpindel: un refuso di battitura
  // (1.40 invece di 1.040) deformerebbe il polinomio su tutta la curva.
  bool plato = r_.cfg->gravityUnit == "plato";
  if (plato ? (g < -1 || g > 45) : (g < 0.90f || g > 1.30f)) {
    sendErr(400, F("densita' fuori range plausibile"));
    return;
  }
  // Mescolare SG e °Plato nello stesso dataset produce una regressione insensata.
  if (r_.calib->count() > 0 && r_.calib->storedPlato() != plato) {
    sendErr(409, F("i punti esistenti usano un'altra unita': azzera la calibrazione"));
    return;
  }
  if (!r_.gravityAny->hasValue()) {
    sendErr(409, F("nessuna lettura dall'iSpindel: attendi il primo invio"));
    return;
  }
  int64_t age = r_.gravityAny->signedAgeSec(r_.time->nowEpoch());
  if (age < 0) {
    sendErr(409, F("orologio incoerente con la lettura: sistema data e ora"));
    return;
  }
  if (age > kCalibMaxAgeSec) {
    sendErr(409, F("lettura troppo vecchia: attendi il prossimo invio dell'iSpindel"));
    return;
  }
  uint32_t readEpoch = r_.gravityAny->updatedEpoch();
  // La lettura deve essere ARRIVATA DOPO l'immersione dichiarata dal client:
  // altrimenti si registrerebbe l'angolo della soluzione precedente.
  if (!doc["afterEpoch"].isNull()) {
    uint32_t after = doc["afterEpoch"];
    if (readEpoch <= after) {
      sendErr(409, F("lettura precedente all'immersione: attendi il prossimo invio"));
      return;
    }
  }
  // Due punti dalla stessa trasmissione avrebbero lo stesso angolo.
  if (r_.calib->hasReadEpoch(readEpoch)) {
    sendErr(409, F("punto gia' registrato per questa lettura: attendi la prossima"));
    return;
  }
  CalibPoint p;
  p.angle = r_.gravityAny->angle();
  p.gravity = g;
  p.temp = r_.gravityAny->temperature();
  p.epoch = r_.time->nowEpoch();
  p.readEpoch = readEpoch;
  p.plato = plato;
  p.deviceId = r_.gravityAny->deviceId();
  if (r_.calibDeviceName)
    strlcpy(p.deviceName, r_.calibDeviceName->c_str(), sizeof(p.deviceName));
  if (p.angle <= 0.01f) {
    sendErr(409, F("angolo non disponibile: aggiorna il firmware dell'iSpindel"));
    return;
  }
  if (!r_.calib->add(p)) {
    sendErr(409, F("numero massimo di punti raggiunto"));
    return;
  }
  if (!r_.calib->save()) {
    r_.calib->remove(r_.calib->count() - 1);  // RAM e disco non devono divergere
    sendErr(500, F("salvataggio fallito"));
    return;
  }
  sendOk();
}

void WebApi::handleCalibDelete() {
  if (!auth()) return;
  if (!r_.calib) {
    sendErr(500, F("archivio calibrazione non disponibile"));
    return;
  }
  if (server_.hasArg("idx")) {
    String raw = server_.arg("idx");
    // "abc".toInt() vale 0: senza questo controllo cancellerebbe il primo punto.
    bool numerico = raw.length() > 0;
    for (size_t i = 0; i < raw.length(); i++)
      if (!isDigit(raw[i])) numerico = false;
    long idx = numerico ? raw.toInt() : -1;
    if (idx < 0 || idx >= (long)r_.calib->count()) {
      sendErr(404, F("punto inesistente"));
      return;
    }
    r_.calib->remove((uint8_t)idx);
  } else {
    r_.calib->clear();
  }
  if (!r_.calib->save()) {
    r_.calib->load();  // ripristina lo stato reale del file
    sendErr(500, F("salvataggio fallito"));
    return;
  }
  sendOk();
}

// --- Diagnostica e taratura delle sonde ------------------------------------
// Espone per ogni sonda il valore GREZZO (senza offset) e le statistiche di
// affidabilità: servono a rispondere a due domande concrete — "questa sonda
// funziona?" e "di quanto sbaglia?" — senza esportare e analizzare il registro.

void WebApi::handleProbeDiag() {
  if (!auth()) return;
  // Dimensione proporzionale al numero di sonde: a kProbes=3 vale esattamente 3072,
  // cioe' il valore storico. Con piu' camere l'array "probes" raddoppia e un doc
  // fisso costringerebbe la guardia overflowed() a rifiutare la diagnostica.
  // +1 KB per lo storico dei riavvii, che dalla 3.5.0 porta intento, durata ed eccezione.
  // +512 B dalla 3.8.0 per il tipo dei guasti di ogni sonda.
  DynamicJsonDocument doc((size_t)ProbeBus::kProbes * 1024u + 1536u);
  uint32_t up = millis();
  doc["uptimeSec"] = up / 1000;
  // Endpoint di DISPOSITIVO: interessa se sta fermentando una camera QUALSIASI,
  // non solo la prima (c_() qui varrebbe sempre la camera 0).
  doc["armed"] = r_.anyArmed ? r_.anyArmed() : c_().isArmed();
  // Storico degli avvii: un riavvio non sorvegliato lascia altrimenti solo un buco
  // nel registro, indistinguibile da un blackout.
  if (r_.boots) {
    doc["bootsTotal"] = r_.boots->total();
    JsonArray bs = doc.createNestedArray("boots");
    for (uint8_t i = 0; i < r_.boots->count(); i++) {
      const BootEntry* b = r_.boots->entry(i);
      JsonObject o = bs.createNestedObject();
      o["t"] = b->epoch;
      o["r"] = b->reason;
      if (b->upSec) o["u"] = b->upSec;
      if (b->intent) o["i"] = b->intent;
      if (b->exc || b->epc) {
        o["x"] = b->exc;
        o["e"] = b->epc;
      }
    }
  }
  doc["profileRunning"] = c_().engine->running() && !c_().engine->paused();
  // Soglie della taratura pubblicate dal firmware: la UI non le duplica a mano.
  doc["avgWindow"] = ProbeBus::kAvgWindow;
  doc["spanMax"] = ProbeBus::kCalibMaxSpanC;
  JsonArray arr = doc.createNestedArray("probes");
  for (uint8_t i = 0; i < ProbeBus::kProbes; i++) {
    ProbeBus::Reading r = r_.probes->reading(i);
    ProbeBus::Stats st = r_.probes->stats(i);
    JsonObject o = arr.createNestedObject();
    o["idx"] = i;
    o["label"] = r_.cfg->probeLabel[i];
    o["id"] = r_.cfg->probeId[i];
    o["configured"] = r.configured;
    o["valid"] = r.valid;
    if (r.valid) {
      o["raw"] = serialized(String(r.raw, 3));
      o["value"] = serialized(String(r.c, 3));
    }
    // serialized() inietta testo grezzo: un offset non finito scriverebbe "nan"
    // nel JSON e romperebbe il pannello in modo permanente.
    if (isfinite(r_.cfg->probeOffset[i])) o["offset"] = serialized(String(r_.cfg->probeOffset[i], 3));
    else o["offset"] = 0;
    o["reads"] = st.reads;
    o["badReads"] = st.badReads;
    o["faults"] = st.faults;
    // Percentuale di letture perse: è il numero che dice se una sonda è affidabile.
    // Finestra per sonda: riconfigurarne una non deve far leggere male le altre.
    o["windowSec"] = r_.probes->statsWindowSec(i);
    // Marginalità: primi tentativi falliti, anche quando il ritentativo li ha salvati.
    // Senza questo numero una sonda che sbaglia sempre al primo colpo sembrerebbe sana.
    o["firstFail"] = st.firstFail;
    o["recovered"] = st.firstFail - st.badReads;
    // Tipo dei tentativi falliti (indice = ax::ProbeFault): dice dove guardare. I nomi li
    // mette la UI, per non tenerli in flash.
    if (st.firstFail) {
      JsonArray k = o.createNestedArray("kinds");
      for (uint8_t j = 0; j < ax::kProbeFaultKinds; j++) k.add(st.kind[j]);
      o["lastKind"] = st.lastKind;
    }
    if (st.reads) o["marginPct"] = serialized(String(100.0f * st.firstFail / st.reads, 2));
    // Senza letture non esiste una percentuale: 0,00% si leggerebbe come "perfetta".
    if (st.reads) o["badPct"] = serialized(String(100.0f * st.badReads / st.reads, 2));
    o["avgSamples"] = r_.probes->rawSamples(i);
    if (r_.probes->rawSamples(i)) {
      o["avgRaw"] = serialized(String(r_.probes->rawAverage(i), 3));
      o["span"] = serialized(String(r_.probes->rawSpan(i), 3));
    }
    if (st.lastFaultMs) o["lastFaultSecAgo"] = (up - st.lastFaultMs) / 1000;
  }
  if (doc.overflowed()) {  // meglio un errore esplicito che un JSON troncato
    sendErr(500, F("diagnostica troppo grande"));
    return;
  }
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

// Taratura: l'utente mette le sonde in un riferimento noto (ghiaccio fondente = 0 °C,
// oppure acqua con un termometro affidabile) e il firmware calcola gli offset.
void WebApi::handleProbeCalib() {
  if (!auth()) return;
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, F("json non valido"));
    return;
  }
  // Il riferimento deve essere un numero: un "0" testuale o una stringa vuota
  // verrebbero letti come 0 °C — il punto di ghiaccio — falsando ogni offset.
  if (!doc["reference"].is<float>() && !doc["reference"].is<int>()) {
    sendErr(400, F("temperatura di riferimento mancante o non numerica"));
    return;
  }
  float ref = doc["reference"];
  if (isnan(ref) || isinf(ref) || ref < -20.0f || ref > 60.0f) {
    sendErr(400, F("riferimento fuori range (-20..60 \xC2\xB0""C)"));
    return;
  }
  // La selezione è obbligatoria: "campo assente = tara tutte" è il default
  // sbagliato da cui difendersi, perché applicherebbe il bagno di riferimento
  // anche a sonde rimaste nel fermentatore.
  if (!doc["probes"].is<JsonArray>()) {
    sendErr(400, F("elenco sonde mancante"));
    return;
  }
  JsonArray sel = doc["probes"];
  bool wanted[ProbeBus::kProbes] = {false};
  uint8_t nSel = 0;
  for (JsonVariant v : sel) {
    if (!v.is<int>()) {
      sendErr(400, F("indice sonda non valido"));
      return;
    }
    int idx = v.as<int>();
    if (idx < 0 || idx >= (int)ProbeBus::kProbes) {
      sendErr(400, F("indice sonda fuori range"));
      return;
    }
    if (!wanted[idx]) { wanted[idx] = true; nSel++; }
  }
  if (!nSel) {
    sendErr(400, F("nessuna sonda selezionata"));
    return;
  }
  // Con l'impianto armato la sonda di controllo sta pilotando i relè: tirarla fuori
  // e immergerla nel riferimento farebbe inseguire al termostato una temperatura
  // che non ha nulla a che vedere col fermentatore.
  // Il motore di profilo consuma la sonda di controllo anche a uscite disarmate:
  // tarare mentre un profilo è in marcia gli farebbe vedere la temperatura del
  // bagno di riferimento e potrebbe fargli avanzare uno step.
  // La sonda di CONTROLLO non e' piu' per definizione l'indice 0: il ruolo appartiene
  // alla camera (ChamberCtx::probeControl). Usare il letterale 0 qui proteggerebbe la
  // sonda sbagliata non appena la camera servita non fosse la prima.
  // Su TUTTE le camere: la sonda che si sta per tarare non deve essere la sonda di
  // controllo di una camera armata o con un profilo in marcia. Interrogare la sola
  // camera servita lascerebbe tarare la sonda che pilota i relè dell'altra.
  for (uint8_t i = 0; i < ProbeBus::kProbes; i++) {
    if (!wanted[i] || !r_.probeInUse || !r_.probeInUse(i)) continue;
    sendErr(409, F("sonda di controllo di una camera armata o con profilo in marcia: "
                 "disarma quella camera (o metti in pausa il profilo) prima di tararla"));
    return;
  }

  // Prima si valida TUTTO, poi si scrive: un rifiuto a metà lascerebbe in
  // configurazione offset applicati solo a una parte delle sonde.
  float nuovo[ProbeBus::kProbes];
  String motivo[ProbeBus::kProbes];
  bool ok[ProbeBus::kProbes] = {false};
  for (uint8_t i = 0; i < ProbeBus::kProbes; i++) {
    if (!wanted[i]) continue;
    ProbeBus::Reading r = r_.probes->reading(i);
    if (!r.configured) { motivo[i] = "non configurata"; continue; }
    if (!r.valid)      { motivo[i] = "lettura non valida"; continue; }
    // Si tara sulla media di un minuto, non su un istante: un singolo campione
    // porta con sé il rumore del convertitore e l'eventuale deriva del bagno.
    if (r_.probes->rawSamples(i) < ProbeBus::kAvgWindow) {
      motivo[i] = "attendi un minuto di letture";
      continue;
    }
    if (r_.probes->rawSpan(i) > ProbeBus::kCalibMaxSpanC) {
      motivo[i] = "bagno non in equilibrio";
      continue;
    }
    float off = ref - r_.probes->rawAverage(i);
    // Un offset enorme non è una taratura: è la sonda sbagliata o il bagno non in
    // equilibrio. Meglio rifiutare che scrivere un errore permanente in configurazione.
    if (off < -5.0f || off > 5.0f) {
      String msg = String("scarto di ") + String(off, 1) + " \xC2\xB0""C su " +
                   r_.cfg->probeLabel[i] + ": verifica il riferimento";
      sendErr(409, msg.c_str());
      return;  // nulla è stato scritto
    }
    nuovo[i] = off;
    ok[i] = true;
  }
  // Una sonda scelta esplicitamente e non tarabile è un errore, non un dettaglio:
  // saltarla in silenzio farebbe credere all'utente di averla tarata.
  for (uint8_t i = 0; i < ProbeBus::kProbes; i++) {
    if (wanted[i] && !ok[i]) {
      String msg = String(r_.cfg->probeLabel[i]) + ": " + motivo[i];
      sendErr(409, msg.c_str());
      return;
    }
  }

  float prima[ProbeBus::kProbes];
  for (uint8_t i = 0; i < ProbeBus::kProbes; i++) {
    prima[i] = r_.cfg->probeOffset[i];
    if (ok[i]) r_.cfg->probeOffset[i] = nuovo[i];
  }
  r_.cfg->configRev++;  // la taratura è un cambio di config: fa avanzare la revisione
  if (!r_.cfgStore->save(*r_.cfg)) {
    for (uint8_t i = 0; i < ProbeBus::kProbes; i++) r_.cfg->probeOffset[i] = prima[i];
    r_.cfg->configRev--;  // salvataggio fallito: nulla è cambiato su flash
    sendErr(500, F("salvataggio fallito"));
    return;
  }
  // setOffset e non applyConfigLive: quest'ultima riconfigura tutte le sonde e,
  // finché non arriva la lettura successiva, il controllo resterebbe cieco.
  // 128 B per sonda (a kProbes=3 fa 384, il valore storico): l'array "applied"
  // cresce con le sonde.
  DynamicJsonDocument out((size_t)ProbeBus::kProbes * 128u);
  out["ok"] = true;
  // La revisione è avanzata (configRev++ sopra): va restituita così il client aggiorna la sua e il
  // salvataggio successivo delle Impostazioni non prende un 409 falso ("cambiata da un altro accesso").
  out["configRev"] = r_.cfg->configRev;
  JsonArray res = out.createNestedArray("applied");
  for (uint8_t i = 0; i < ProbeBus::kProbes; i++) {
    if (!ok[i]) continue;
    r_.probes->setOffset(i, nuovo[i]);
    JsonObject o = res.createNestedObject();
    o["idx"] = i;
    o["label"] = r_.cfg->probeLabel[i];
    o["offset"] = serialized(String(nuovo[i], 3));
  }
  // Guardia uniforme agli altri 10 punti del firmware: mai una risposta troncata in
  // silenzio. La taratura e' gia' stata applicata, ma una risposta monca romperebbe
  // il pannello senza dire perche'.
  if (out.overflowed()) { sendErr(500, F("risposta taratura troppo grande")); return; }
  String body;
  serializeJson(out, body);
  sendJson(200, body);
}

// Densita' di un record del registro nell'unita' attuale (3.16.0): i record nuovi portano la
// loro unita' (bit6 = registrata, bit5 = °Plato), quelli di prima valgono nell'unita'
// configurata. Cosi' cambiare SG <-> °P non stravolge lo storico.
static float densitaRecord(const DataLog::Sample& s, bool plato) {
  const bool recP = (s.flags & 0x40) ? (s.flags & 0x20) != 0 : plato;
  float v = s.gravity / (recP ? 100.0f : 10000.0f);
  if (recP != plato) v = plato ? ax::sgToPlato(v) : ax::platoToSg(v);
  return v;
}

// Registro eventi (3.15.0): i piu' recenti per primi, a pagine (?before=<seq>&n=<1..40>).
// I testi li mette la UI: la scheda manda solo codici e numeri, 16 B per evento in EEPROM.
void WebApi::handleEvents() {
  if (!auth()) return;
  EventLog* ev = r_.events;
  const bool on = ev && ev->on();
  const uint32_t last = on ? ev->lastSeq() : 0;
  uint32_t before = server_.hasArg("before") ? strtoul(server_.arg("before").c_str(), nullptr, 10)
                                             : last + 1;
  if (before > last + 1) before = last + 1;
  long n = server_.hasArg("n") ? server_.arg("n").toInt() : 20;
  if (n < 1) n = 1;
  if (n > 40) n = 40;
  String out;
  out.reserve(48 + n * 64);
  out = F("{\"on\":");
  out += on ? F("true") : F("false");
  out += F(",\"last\":");
  out += String(last);
  out += F(",\"ev\":[");
  uint32_t seq = before ? before - 1 : 0;
  for (long k = 0; on && seq > 0 && k < n; k++, seq--) {
    ax::EventRec r;
    if (!ev->read(seq, r)) break;  // oltre i 256 conservati, o record illeggibile
    if (k) out += ',';
    out += F("{\"s\":");
    out += String(r.seq);
    out += F(",\"e\":");
    out += String(r.epoch);
    out += F(",\"c\":");
    out += String(r.code);
    out += F(",\"a\":");
    out += String(r.a);
    out += F(",\"b\":");
    out += String(r.b);
    out += F(",\"v\":");
    out += String(r.v);
    out += '}';
  }
  out += F("]}");
  sendJson(200, out);
}

void WebApi::handleLogCsv() {
  if (!auth()) return;
  if (csv_.active) {
    // Uno alla volta: due export insieme raddoppierebbero heap e scritture senza motivo.
    sendErrT(503, F("un altro export CSV e' in corso: riprova fra poco"));
    return;
  }
  uint32_t hours = server_.hasArg("hours") ? server_.arg("hours").toInt() : 0;
  uint32_t now = r_.time->nowEpoch();
  uint32_t from = hours > 0 && now > hours * 3600 ? now - hours * 3600 : 0;
  // Mai iterare oltre la retention (senza 'hours' un epoch reale farebbe girare
  // il loop su ~20 000 giorni inesistenti).
  uint32_t retSec = (uint32_t)r_.cfg->logRetentionDays * 86400UL;
  if (now > retSec && from < now - retSec) from = now - retSec;
  csv_.plato = r_.cfg->gravityUnit == "plato";
  csv_.rows = 0;
  csv_.lastMs = millis();
  DataLog::cursorBegin(csv_.cur, from, now);
  // Intestazioni scritte a mano, senza la modalita' chunked del server: quella chiuderebbe
  // la risposta (chunk vuoto finale) appena l'handler ritorna, mentre qui il corpo arriva
  // dopo, da loop(). "Connection: close": il browser non riusa la connessione per altro.
  csv_.client = server_.client();
  csv_.client.print(F("HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/csv\r\n"
                      "Content-Disposition: attachment; filename=\"axhtherm.csv\"\r\n"
                      "Cache-Control: no-store\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n\r\n"));
  static const char kHead[] PROGMEM =
      "epoch;data_locale;t_controllo;t_aux1;t_aux2;setpoint;caldo;freddo;"
      "armato;allarme;densita;batteria\r\n";
  csv_.client.printf("%x\r\n", (unsigned)strlen_P(kHead));
  csv_.client.print(FPSTR(kHead));
  csv_.client.print(F("\r\n"));
  csv_.active = true;
}

// Un pezzo di CSV per giro di loop: tante righe quante ne stanno nel buffer TCP libero, cosi'
// la scrittura non si ferma mai ad aspettare il client. Ogni riga e' al massimo ~110 B (si
// contano 128 con il margine), e il pezzo porta 8 B di intestazione chunked.
// 3.17.2: la 3.14.0 aspettava 1.460 B liberi, ma con lwIP «low memory» (MSS 536) il buffer di
// invio e' al massimo 2 x 536 = 1.072 B: non scriveva mai, il download restava appeso e dopo
// 30 s la scheda chiudeva. Trovato sulla scheda 1 il 15/11/2026.
void WebApi::csvPump() {
  if (!csv_.client.connected()) {  // il browser ha annullato il download
    csv_.client.stop();
    csv_.active = false;
    Serial.println(F("[csv] export interrotto dal client"));
    return;
  }
  const int libero = csv_.client.availableForWrite();
  int righe = (libero - 16) / 128;
  if (righe > 12) righe = 12;
  if (righe < 1) {
    if (millis() - csv_.lastMs > 30000) {  // client fermo da 30 s: si lascia perdere
      csv_.client.stop();
      csv_.active = false;
      Serial.println(F("[csv] export chiuso: client fermo da 30 s"));
    }
    return;
  }
  // Tetto di righe: oltre si tronca in modo VISIBILE, non in silenzio.
  static constexpr uint32_t kMaxRows = 50000;
  String chunk;
  chunk.reserve(righe * 128);
  DataLog& log = *r_.log;
  log.cursorRead(csv_.cur, (uint16_t)righe, 400, [&](const DataLog::Sample& s) {
    csv_.rows++;
    chunk += String(s.epoch);
    chunk += ';';
    chunk += r_.time->localString(s.epoch);
    chunk += ';';
    if (s.tControl != DataLog::kMissing) chunk += String(s.tControl / 100.0f, 2);
    chunk += ';';
    if (s.tAux1 != DataLog::kMissing) chunk += String(s.tAux1 / 100.0f, 2);
    chunk += ';';
    if (s.tAux2 != DataLog::kMissing) chunk += String(s.tAux2 / 100.0f, 2);
    chunk += ';';
    if ((s.flags & 0x10) && s.setpoint != DataLog::kMissing) chunk += String(s.setpoint / 100.0f, 2);
    chunk += ';';
    chunk += (s.flags & 0x01) ? F("1;") : F("0;");
    chunk += (s.flags & 0x02) ? F("1;") : F("0;");
    chunk += (s.flags & 0x04) ? F("1;") : F("0;");
    chunk += (s.flags & 0x08) ? F("1;") : F("0;");
    if (s.gravity) chunk += String(densitaRecord(s, csv_.plato), csv_.plato ? 2 : 4);
    chunk += ';';
    if (s.battery) chunk += String(s.battery / 20.0f, 2);
    chunk += F("\r\n");
  });
  if (chunk.length()) {
    csv_.client.printf("%x\r\n", (unsigned)chunk.length());
    csv_.client.write((const uint8_t*)chunk.c_str(), chunk.length());
    csv_.client.print(F("\r\n"));
    csv_.lastMs = millis();
  }
  if (csv_.rows >= kMaxRows)
    csvEnd(F("# export troncato a 50000 righe: restringi con ?hours=<n>\r\n"));
  else if (csv_.cur.done)
    csvEnd(nullptr);
}

void WebApi::csvEnd(const __FlashStringHelper* trailer) {
  if (trailer) {
    csv_.client.printf("%x\r\n", (unsigned)strlen_P((PGM_P)trailer));
    csv_.client.print(trailer);
    csv_.client.print(F("\r\n"));
  }
  csv_.client.print(F("0\r\n\r\n"));  // fine del corpo chunked
  csv_.client.stop(200);  // attesa breve dell'ultimo ACK; il resto lo chiude lwIP
  csv_.active = false;
  Serial.printf_P(PSTR("[csv] export completato: %lu righe\n"), (unsigned long)csv_.rows);
}

void WebApi::handleLogJson() {
  if (!auth()) return;
  uint32_t hours = server_.hasArg("hours") ? server_.arg("hours").toInt() : 48;
  if (hours == 0 || hours > 24 * 60) hours = 48;
  uint32_t now = r_.time->nowEpoch();
  uint32_t from = now > hours * 3600 ? now - hours * 3600 : 0;
  uint32_t retSec = (uint32_t)r_.cfg->logRetentionDays * 86400UL;
  if (now > retSec && from < now - retSec) from = now - retSec;
  const bool plato = r_.cfg->gravityUnit == "plato";
  const float gdiv = plato ? 100.0f : 10000.0f;
  // Mai piu' di ~600 punti al grafico: fino a 300 gruppi, ognuno ridotto a due punti che
  // portano minimo e massimo di ogni serie (ax::MinMaxGroup), cosi' i picchi restano.
  uint32_t expected = hours * 3600 / ((uint32_t)r_.cfg->logSampleMin * 60);
  uint32_t stride = expected > 600 ? (expected + 299) / 300 : 1;

  server_.chunkedResponseModeStart(200, "application/json");
  // Pezzi da ~1 KB invece di un invio TCP per punto: 10 volte meno scritture.
  String buf;
  buf.reserve(1200);
  buf = '[';
  bool first = true;
  enum : uint8_t { T = 0, A, B, S, G, NSERIE };
  ax::MinMaxGroup<NSERIE> grp;
  grp.reset();
  auto emit = [&](uint32_t epoch, bool second) {
    if (!first) buf += ',';
    first = false;
    buf += F("{\"e\":");
    buf += String(epoch);
    int32_t v;
    if ((v = grp.value(T, second)) != grp.kMissing) { buf += F(",\"t\":"); buf += String(v / 100.0f, 2); }
    if ((v = grp.value(A, second)) != grp.kMissing) { buf += F(",\"a\":"); buf += String(v / 100.0f, 2); }
    if ((v = grp.value(B, second)) != grp.kMissing) { buf += F(",\"b\":"); buf += String(v / 100.0f, 2); }
    if ((v = grp.value(S, second)) != grp.kMissing) { buf += F(",\"s\":"); buf += String(v / 100.0f, 2); }
    if ((v = grp.value(G, second)) != grp.kMissing) { buf += F(",\"g\":"); buf += String(v / gdiv, plato ? 2 : 4); }
    buf += F(",\"h\":");
    buf += (grp.flags() & 0x01) ? '1' : '0';
    buf += F(",\"c\":");
    buf += (grp.flags() & 0x02) ? '1' : '0';
    buf += '}';
    if (buf.length() > 1000) {
      server_.sendContent(buf);
      buf = "";
    }
  };
  uint32_t letti = 0;
  auto flush = [&]() {
    if (!grp.count()) return;
    emit(grp.firstEpoch(), false);
    if (grp.count() > 1) emit(grp.lastEpoch(), true);
    grp.reset();
  };
  r_.log->forEach(from, now, [&](const DataLog::Sample& s) {
    // Sincrono dentro il loop: si rinvia il failsafe solo dove c'e' progresso dimostrato
    // (campioni letti), che e' la differenza con il blocco che deve intercettare. Ogni 64
    // campioni (3.17.3): uno per campione gonfiava il contatore (28.902 rinvii in 12 minuti).
    if ((++letti & 63) == 0 && r_.progress) r_.progress();
    const int32_t M = grp.kMissing;
    const int32_t v[NSERIE] = {
        s.tControl == DataLog::kMissing ? M : s.tControl,
        s.tAux1 == DataLog::kMissing ? M : s.tAux1,
        s.tAux2 == DataLog::kMissing ? M : s.tAux2,
        (s.flags & 0x10) && s.setpoint != DataLog::kMissing ? s.setpoint : M,
        s.gravity ? (int32_t)lroundf(densitaRecord(s, plato) * (plato ? 100.0f : 10000.0f)) : M};
    grp.add(s.epoch, v, s.flags & 0x03);
    if (grp.count() >= stride) flush();
    return true;
  });
  flush();
  buf += ']';
  server_.sendContent(buf);
  server_.chunkedResponseFinalize();
}

void WebApi::handleWifiScan() {
  if (!auth()) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    // Nessuna scansione in corso: avviala in modo ASINCRONO (il loop di controllo
    // non si ferma). In modalità solo-AP serve abilitare la STA per scandire.
    WiFi.enableSTA(true);
    WiFi.scanNetworks(true, false);
    sendJson(200, "{\"scanning\":true}");
    return;
  }
  if (n == WIFI_SCAN_RUNNING) {
    sendJson(200, "{\"scanning\":true}");
    return;
  }
  // Risultati pronti: dedup per SSID (tiene il segnale migliore), ordina per RSSI.
  const int kMax = 20;
  int idx[kMax];
  int cnt = 0;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    int found = -1;
    for (int j = 0; j < cnt; j++) {
      if (WiFi.SSID(idx[j]) == ssid) { found = j; break; }
    }
    if (found >= 0) {
      if (WiFi.RSSI(i) > WiFi.RSSI(idx[found])) idx[found] = i;
    } else if (cnt < kMax) {
      idx[cnt++] = i;
    }
  }
  for (int a = 0; a < cnt - 1; a++) {
    for (int b = a + 1; b < cnt; b++) {
      if (WiFi.RSSI(idx[b]) > WiFi.RSSI(idx[a])) {
        int t = idx[a];
        idx[a] = idx[b];
        idx[b] = t;
      }
    }
  }
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("networks");
  for (int j = 0; j < cnt; j++) {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = WiFi.SSID(idx[j]);
    o["rssi"] = WiFi.RSSI(idx[j]);
    o["enc"] = WiFi.encryptionType(idx[j]) != ENC_TYPE_NONE;
  }
  WiFi.scanDelete();  // libera la memoria e riabilita una scansione futura
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebApi::handleDiagI2c() {
  if (!auth()) return;
  String body = "{\"devices\":[";
  bool first = true;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (!first) body += ",";
      first = false;
      char buf[8];
      sprintf(buf, "\"0x%02X\"", a);
      body += buf;
    }
    yield();
  }
  body += "]}";
  sendJson(200, body);
}

void WebApi::handleDiagOneWire() {
  if (!auth()) return;
  // scan()/tempById() resettano il bus e annullano la conversione in corso: un polling serrato di
  // questo endpoint (pannello diagnostico in auto-refresh) affamerebbe le letture periodiche e
  // congelerebbe la sonda di CONTROLLO. Si ri-scansiona al più ogni 2 s e si riusa l'ultimo elenco;
  // le LETTURE live (reading(i), che NON toccano il bus) si servono sempre aggiornate.
  uint32_t nowMs = millis();
  if (owScanMs_ == 0 || (uint32_t)(nowMs - owScanMs_) >= 2000) {
    String ids[8];
    uint8_t n = r_.probes->scan(ids, 8);
    DynamicJsonDocument sd(768);
    JsonArray arr = sd.createNestedArray("p");
    for (uint8_t i = 0; i < n; i++) {
      JsonObject o = arr.createNestedObject();
      o["id"] = ids[i];
      float t;
      if (r_.probes->tempById(ids[i], &t)) o["c"] = serialized(String(t, 1));
    }
    owProbesCache_ = "";
    serializeJson(arr, owProbesCache_);
    owScanMs_ = nowMs;
  }
  DynamicJsonDocument doc(512);
  JsonArray rd = doc.createNestedArray("readings");
  for (uint8_t i = 0; i < ProbeBus::kProbes; i++) {
    ProbeBus::Reading rr = r_.probes->reading(i);
    JsonObject o = rd.createNestedObject();
    o["configured"] = rr.configured;
    o["valid"] = rr.valid;
    if (rr.valid) o["c"] = serialized(String(rr.c, 2));
  }
  String rdStr;
  serializeJson(rd, rdStr);
  sendJson(200, String("{\"probes\":") + owProbesCache_ + ",\"readings\":" + rdStr + "}");
}

void WebApi::handleDiagRelay() {
  if (!auth()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, F("json non valido"));
    return;
  }
  uint8_t n = doc["n"] | 0;
  bool on = doc["on"] | false;
  if (n < 1 || n > 8) {
    sendErr(400, F("n deve essere 1..8"));
    return;
  }
  // Stato live (non la cache di status, vecchia fino a 1 s) e su TUTTE le camere: gli 8
  // canali sono del dispositivo, e un test puo' eccitare un carico mentre un'altra
  // camera sta fermentando. Stessa regola della CLI seriale (anyChamberArmed).
  bool armed = r_.anyArmed ? r_.anyArmed() : c_().isArmed();
  if (armed) {
    sendErr(409, F("almeno una camera e' ARMATA: disarmare prima del test rele'"));
    return;
  }
  if (!r_.outputs->testMode() && !r_.outputs->enterTestMode(armed)) {
    sendErr(409, F("impossibile entrare in modalita' test"));
    return;
  }
  if (!r_.outputs->testRelay(n, on)) {
    sendErr(500, F("comando rele' fallito (MCP23008 assente?)"));
    return;
  }
  sendOk();
}

void WebApi::handleTime() {
  if (!auth()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server_.arg("plain")) || doc["epoch"].isNull()) {
    sendErr(400, F("atteso {\"epoch\": <utc>}"));
    return;
  }
  if (!r_.time->setManual(doc["epoch"])) {
    sendErr(400, F("epoch non plausibile"));
    return;
  }
  sendSaved(r_.persistNow());
}

void WebApi::handleUpdateFinal() {
  // Doppio controllo: sia il flag dell'upload sia una verifica auth fresca sulla
  // richiesta finale (una POST senza multipart non attiva l'upload handler e non deve
  // poter cavalcare un flag rimasto da un upload precedente annullato).
  if (!otaAuthorized_ || !authValid()) {
    otaAuthorized_ = false;
    if (!authValid()) sendErr(401, F("non autorizzato"));
    else if (otaDeny_ == 1) sendErr(403, F("aggiornamento via rete disattivato in Impostazioni"));
    else if (otaDeny_ == 2) sendErr(403, F("prima cambia la password di fabbrica dell'interfaccia"));
    else if (otaDeny_ == 5) sendWait(otaWait_);
    else if (otaDeny_ == 6) sendErr(403, F("nome della scheda non riconosciuto: usa l'IP o il nome .local"));
    else sendErr(403, F("password attuale errata"));
    return;
  }
  otaAuthorized_ = false;
  // Immagine non valida: l'aggiornamento non e' stato confermato, resta il firmware attuale.
  using R = ax::FwImageCheck::Result;
  if (otaImg_ == R::NotImage) {
    sendErr(400, F("non e' un firmware.bin per questa scheda (un .bin.gz o un altro file?): "
                   "niente installato"));
    return;
  }
  if (otaImg_ == R::Short || otaImg_ == R::Long) {
    sendErr(400, F("file incompleto o con dati in piu' (lunghezza diversa da quella dichiarata "
                   "nel firmware): niente installato, resta il firmware attuale"));
    return;
  }
  if (otaImg_ == R::Crc) {
    sendErr(400, F("file danneggiato (CRC diverso): niente installato, resta il firmware attuale"));
    return;
  }
  if (Update.hasError()) {
    sendErr(500, (String(F("aggiornamento fallito: ")) + Update.getErrorString()).c_str());
    return;
  }
  sendJson(200, "{\"ok\":true,\"reboot\":true}");
  if (r_.logEvent) r_.logEvent((uint8_t)Ev::Ota, 1, 0, 0);
  r_.scheduleReboot(1500, (uint8_t)RebootIntent::Ota);
}

void WebApi::handleUpdateUpload() {
  HTTPUpload& up = server_.upload();
  if (up.status == UPLOAD_FILE_START) {
    // Keepalive TCP sulla connessione di upload. Il framework attende i byte del corpo
    // multipart con `while(!client.available() && client.connected()) yield();`
    // (Parsing-impl.h): NESSUN timeout, e yield() nutre entrambi i watchdog. Se il peer
    // sparisce senza chiudere il TCP — il telefono che esce dal WiFi a meta' OTA, cioe'
    // lo scenario NON avversariale e piu' probabile — connected() resterebbe vero a
    // tempo indefinito, e il loop di controllo resterebbe fermo con i rele' latchati.
    // Col keepalive un peer morto viene rilevato in ~11 s.
    // Il gate NON e' cosmetico: ClientContext::keepAlive dereferenzia _pcb SENZA
    // controllo di nullita' (ClientContext.h, entrambi i rami), e _error() azzera _pcb su
    // RST. Il parser puo' arrivare qui con _pcb gia' nullo, perche' le letture del
    // preambolo sono servite da _rx_buf e connected() resta vero grazie ad available():
    // sarebbe una store su ~0x14, eccezione 29, RIAVVIO — provocabile da chiunque sulla
    // LAN senza credenziali. status() passa invece da state(), che e' l'unico accessore
    // null-safe (`if (!_pcb ...) return CLOSED`).
    if (server_.client().status() == ESTABLISHED) server_.client().keepAlive(5, 2, 3);
    // OTA vietato finché la password UI è quella di default: altrimenti chiunque
    // conosca il default potrebbe flashare firmware arbitrario. Va cambiata prima.
    // Password vuota => authValid() è true per tutti (WebApi.cpp:152): l'OTA si aprirebbe
    // a chiunque sulla LAN. Va richiesta una password reale, non default E non vuota.
    // Oltre alla sessione serve la password ATTUALE nell'intestazione X-Ax-Password: un
    // cookie valido un anno non deve bastare a cambiare il firmware.
    const bool nomeOk = hostOk(false);
    const bool sessione = nomeOk && authValid();
    if (nomeOk && !sessione) countBasicFail();
    const bool bloccato = pwPen_.locked(ipKey(), millis());
    otaWait_ = pwPen_.waitSec(ipKey(), millis());
    const bool pwOk = !bloccato && server_.header("X-Ax-Password") == r_.cfg->uiPassword;
    const bool pwReale = !r_.cfg->usingDefaultUiPassword() && r_.cfg->uiPassword.length() > 0;
    otaAuthorized_ = nomeOk && r_.cfg->otaEnabled && sessione && pwOk && pwReale;
    otaDeny_ = !nomeOk ? 6 : !r_.cfg->otaEnabled ? 1 : !pwReale ? 2 : bloccato ? 5 : !pwOk ? 3 : 0;
    otaImg_ = ax::FwImageCheck::Result::Ok;
    if (otaDeny_ == 2)
      Serial.println(F("[ota] RIFIUTATO: impostare una password UI (non default, non vuota) prima di aggiornare"));
    else if (otaDeny_ == 3) {
      Serial.println(F("[ota] RIFIUTATO: password attuale mancante o errata"));
      if (sessione) pwFail();
    }
    if (!otaAuthorized_) {
      // Si distingue il NON AUTENTICATO da chi e' autenticato ma ha l'OTA sbarrata
      // (password di default o vuota, o ota disabilitata). Al primo si chiude la
      // connessione: il parser continuerebbe altrimenti a consumare l'intero corpo, e una
      // richiesta senza credenziali comprerebbe tempo di loop. Al secondo NO: merita di
      // ricevere l'errore da handleUpdateFinal, invece di vedersi cadere la connessione
      // senza sapere perche'.
      if (!sessione) server_.client().stop();
      return;
    }
    // NB: durante il flash il loop è congelato per la durata di upload+reboot. Dalla 3.14.0 il
    // CALDO si spegne qui (SF-14): un riscaldatore acceso e senza controllo per tutto l'upload
    // e' il caso peggiore, e non ha una guardia da tenere coerente. Il FREDDO resta latchato:
    // spegnere il compressore senza sincronizzare la guardia (assumeJustStopped) provocherebbe,
    // su OTA ABORTITA (nessun reboot), un corto ciclo alla ripresa del loop (guardia con on_
    // stantio → mustStayOn rieccita subito). Massa termica di ore → frazione di grado.
    if (r_.outputs) {
      r_.outputs->heatOff(0);
      Serial.println(F("[ota] caldo spento per la durata dell'aggiornamento"));
    }
    uint32_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    Serial.printf_P(PSTR("[ota] upload %s (max %u B)\n"), up.filename.c_str(), maxSpace);
    if (!Update.begin(maxSpace)) Update.printError(Serial);
    fwCheck_.begin();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    // Progresso dimostrato: un blocco e' stato ricevuto e scritto. Rinviando qui, la
    // durata dell'OTA smette di contare contro il failsafe — e cade il margine sottile
    // che i 60 s lasciavano su un firmware da ~730 KB con WiFi lento. Restano coperte le
    // attese SENZA progresso, che sono esattamente quelle da intercettare.
    // Solo un upload AUTORIZZATO rinvia il failsafe: chi ha solo il cookie e una password
    // sbagliata non deve poter tenere fermo il loop mandando dati.
    if (otaAuthorized_ && r_.progress) r_.progress();
    if (otaAuthorized_) fwCheck_.feed(up.buf, up.currentSize);
    if (otaAuthorized_ && Update.write(up.buf, up.currentSize) != up.currentSize)
      Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaAuthorized_) {
      // Si conferma (end(true) scrive il comando per il bootloader) SOLO se il file e' un
      // firmware intero e integro: il bootloader copia senza alcun controllo, e un'immagine
      // troncata lascerebbe la scheda in riavvio continuo, recuperabile solo via USB.
      otaImg_ = fwCheck_.result();
      if (otaImg_ != ax::FwImageCheck::Result::Ok) {
        Serial.printf_P(PSTR("[ota] RIFIUTATO: immagine non valida (esito %u, %u B ricevuti, %u "
                             "dichiarati)\n"),
                        (unsigned)otaImg_, fwCheck_.received(), fwCheck_.declared());
        // Annulla senza comando al bootloader. end(false) conferma comunque se il file ha
        // riempito esattamente lo spazio: un MD5 impossibile garantisce l'errore (che azzera).
        Update.setMD5("00000000000000000000000000000000");
        Update.end(false);
      } else if (Update.end(true)) {
        Serial.printf_P(PSTR("[ota] ok, %u B\n"), up.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    otaAuthorized_ = false;  // non lasciare l'autorizzazione appesa a un upload interrotto
    Update.end(false);
  }
}
