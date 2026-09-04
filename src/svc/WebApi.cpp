#include "svc/WebApi.h"
#include "svc/BootLog.h"
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <Updater.h>
#include <Wire.h>
#include <uri/UriBraces.h>
#include <MD5Builder.h>
#include "web_assets.h"

// Validazione comune degli step (editor libreria e profilo attivo).
static const char* validateSteps(const ax::StepDef* steps, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    if (!steps[i].enabled) continue;
    // Freeze-guard: sotto -1 °C l'overshoot del frigo può portare la birra verso il
    // punto di congelamento (~-3 °C a 5% ABV): il floor del setpoint è -1.
    if (steps[i].targetC < -1 || steps[i].targetC > 40 || steps[i].startC < -1 ||
        steps[i].startC > 40)
      return "temperatura step fuori range (-1..40 \xC2\xB0""C)";
    if (steps[i].durationSec > 90UL * 86400UL) return "durata step oltre 90 giorni";
    if (steps[i].degPerHour < 0 || steps[i].degPerHour > 10)
      return "rampa fuori range (0..10 \xC2\xB0""C/h)";
    // maxDurationSec è ora un timeout di sicurezza anche per RampRate (oltre che per
    // Gravity): va validato dove il motore lo usa.
    if (steps[i].type == ax::StepType::RampRate &&
        steps[i].maxDurationSec > 90UL * 86400UL)
      return "timeout rampa oltre 90 giorni";
    // I campi densità si validano SOLO dove il motore li usa: uno step fixed con
    // residui strani (import, cambio tipo in editor) non deve essere rifiutato per
    // un campo invisibile.
    if (steps[i].type != ax::StepType::Gravity) continue;
    if (steps[i].maxDurationSec > 90UL * 86400UL) return "timeout step oltre 90 giorni";
    if (steps[i].gravityMode == ax::GravityMode::Attenuation) {
      if (steps[i].gravityTarget < 1 || steps[i].gravityTarget > 100)
        return "attenuazione target fuori range (1..100 %)";
    } else if (steps[i].gravityMode == ax::GravityMode::Below &&
               (steps[i].gravityTarget < 0 || steps[i].gravityTarget > 35)) {
      return "densita' target fuori range";
    }
    if (steps[i].gravityMode == ax::GravityMode::Stable) {
      if (steps[i].stableBand < 0 || steps[i].stableBand > 2)
        return "banda di stabilita' fuori range";
      if (steps[i].stableHours < 1 || steps[i].stableHours > 168)
        return "finestra di stabilita' fuori range (1..168 h)";
    }
  }
  return nullptr;
}

String WebApi::segAfter(const char* prefix) const {
  const String u = server_.uri();
  const size_t n = strlen(prefix);
  if (!u.startsWith(prefix)) return String();
  int fine = u.indexOf('/', (int)n);
  return fine < 0 ? u.substring((int)n) : u.substring((int)n, fine);
}

// Ricava la camera dall'URI, NON da server_.pathArg(0).
//
// pathArg() sarebbe una trappola: UriBraces::canHandle chiama PRIMA Uri::canHandle, che
// fa un confronto di stringhe esatto (`_uri == requestUri`), e in caso di match ritorna
// true SENZA popolare pathArgs. Una richiesta al percorso LETTERALE "/api/ch/{}/status"
// combacia quindi esattamente, pathArgs resta vuoto, e pathArg(0) esegue
// `assert(0 < 0)` (RequestHandler.h) — NDEBUG non e' definito in questo build, quindi
// e' un panic con riavvio del controllore, raggiungibile senza autenticazione.
// Leggendo l'URI direttamente il comportamento non dipende da come il framework popola
// gli argomenti.
bool WebApi::selectChamberFromUri() {
  const String n = segAfter("/api/ch/");
  if (!n.length()) {  // percorso letterale con le graffe, o prefisso inatteso
    sendErr(404, "camera inesistente");
    sel_ = 0;
    return false;
  }
  return selectChamber(n);
}

bool WebApi::selectChamber(const String& n) {
  const long v = n.toInt();
  if (v < 1 || v > AX_N_CHAMBERS) {
    sendErr(404, "camera inesistente");
    sel_ = 0;
    return false;
  }
  sel_ = (uint8_t)(v - 1);
  return true;
}

// Tabella delle rotte per camera. Vive in .rodata (~240 B) e sostituisce ~2,9 KB di heap
// PERMANENTE: e' il singolo recupero piu' grande disponibile, e per di piu' elimina il
// costo dinamico che la SECONDA CAMERA aggiungeva. Gli URL esposti non cambiano.
const WebApi::RottaCamera WebApi::kRotteCamera[] = {
    {"/api/status", HTTP_GET, &WebApi::handleStatus},
    {"/api/profile", HTTP_GET, &WebApi::handleGetProfile},
    {"/api/profile", HTTP_PUT, &WebApi::handlePutProfile},
    {"/api/profile/start", HTTP_POST, &WebApi::cmdStart},
    {"/api/profile/stop", HTTP_POST, &WebApi::cmdStop},
    {"/api/profile/pause", HTTP_POST, &WebApi::cmdPause},
    {"/api/profile/resume", HTTP_POST, &WebApi::cmdResume},
    {"/api/profile/skip", HTTP_POST, &WebApi::cmdSkip},
    {"/api/profile/coldcrash", HTTP_POST, &WebApi::handleColdCrash},
    {"/api/setpoint", HTTP_POST, &WebApi::handleSetpoint},
    {"/api/thermostat", HTTP_POST, &WebApi::handleThermostat},
    {"/api/gravity/og", HTTP_POST, &WebApi::handleOg},
    {"/api/outputs/arm", HTTP_POST, &WebApi::handleArm},
    {"/api/outputs/disarm", HTTP_POST, &WebApi::cmdDisarmCamera},
    {"/api/log.csv", HTTP_GET, &WebApi::handleLogCsv},
    {"/api/log", HTTP_GET, &WebApi::handleLogJson},
};
const uint8_t WebApi::kNRotteCamera = sizeof(kRotteCamera) / sizeof(kRotteCamera[0]);

// Serve una richiesta /api/ch/<n>/<resto>. Ritorna false se non e' di quella forma.
bool WebApi::dispatchCamera() {
  const String u = server_.uri();
  if (!u.startsWith("/api/ch/")) return false;
  // auth PRIMA di tutto: senza, un non autenticato ricaverebbe dal 404 quante camere
  // esistono, e raggiungerebbe comunque il parsing dell'indice.
  if (!auth()) return true;
  if (!selectChamberFromUri()) return true;   // 404 gia' inviato
  // "/api/ch/<n>/xxx" -> "/api/xxx". Si cerca la barra che chiude l'indice.
  const int barra = u.indexOf('/', 8);
  if (barra < 0) { sendErr(404, "endpoint non valido"); return true; }
  const String base = String("/api") + u.substring(barra);
  const HTTPMethod m = server_.method();
  for (uint8_t i = 0; i < kNRotteCamera; i++) {
    if (m != kRotteCamera[i].method) continue;
    if (base != kRotteCamera[i].path) continue;
    (this->*kRotteCamera[i].fn)();
    return true;
  }
  sendErr(404, "endpoint non valido per una camera");
  return true;
}

void WebApi::onChamber(const char* apiPath, HTTPMethod method, std::function<void()> h) {
  // Forma storica: serve la camera 1. A una sola camera e' l'unica che esiste, ed e'
  // quella che l'interfaccia attuale e le automazioni Home Assistant gia' chiamano.
  server_.on(apiPath, method, [this, h]() { sel_ = 0; h(); });
  // La forma per camera NON si registra piu' come rotta: la serve dispatchCamera() da
  // onNotFound, leggendo l'indice dall'URI. Vedi il commento su kRotteCamera.
}

void WebApi::begin(const Refs& refs) {
  r_ = refs;

  server_.collectHeaders("If-None-Match", "Cookie");

  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/api/login", HTTP_POST, [this]() { handleLogin(); });
  server_.on("/api/logout", HTTP_POST, [this]() {
    server_.sendHeader("Set-Cookie", "axs=; Max-Age=0; Path=/");
    sendOk();
  });
  // Icona per "Aggiungi a Home"/favicon: senza auth (Safari la scarica fuori sessione).
  server_.on("/icon.png", HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "max-age=86400");
    server_.send_P(200, "image/png", (const char*)WEB_ICON_PNG, WEB_ICON_PNG_LEN);
  });
  // Splash screen iOS (apple-touch-startup-image), una per risoluzione iPhone.
  // Senza auth: iOS le scarica al momento di "Aggiungi a Home".
  for (unsigned int i = 0; i < WEB_SPLASHES_COUNT; i++) {
    const WebSplash* sp = &WEB_SPLASHES[i];
    server_.on(sp->path, HTTP_GET, [this, sp]() {
      server_.sendHeader("Cache-Control", "max-age=604800");
      server_.send_P(200, "image/png", (const char*)sp->data, sp->len);
    });
  }
  onChamber("/api/status", HTTP_GET, [this]() { handleStatus(); });
  server_.on("/api/config", HTTP_GET, [this]() { handleGetConfig(); });
  server_.on("/api/config", HTTP_PUT, [this]() { handlePutConfig(); });
  onChamber("/api/profile", HTTP_GET, [this]() { handleGetProfile(); });
  onChamber("/api/profile", HTTP_PUT, [this]() { handlePutProfile(); });
  server_.on("/api/profiles", HTTP_GET, [this]() { handleProfilesList(); });
  server_.on(UriBraces("/api/profiles/{}"), HTTP_GET, [this]() { handleProfileById(HTTP_GET); });
  server_.on(UriBraces("/api/profiles/{}"), HTTP_PUT, [this]() { handleProfileById(HTTP_PUT); });
  server_.on(UriBraces("/api/profiles/{}"), HTTP_DELETE,
             [this]() { handleProfileById(HTTP_DELETE); });
  onChamber("/api/profile/start", HTTP_POST, [this]() { handleProfileCmd("start"); });
  onChamber("/api/profile/stop", HTTP_POST, [this]() { handleProfileCmd("stop"); });
  onChamber("/api/profile/pause", HTTP_POST, [this]() { handleProfileCmd("pause"); });
  onChamber("/api/profile/resume", HTTP_POST, [this]() { handleProfileCmd("resume"); });
  onChamber("/api/profile/skip", HTTP_POST, [this]() { handleProfileCmd("skip"); });
  onChamber("/api/profile/coldcrash", HTTP_POST, [this]() { handleColdCrash(); });
  onChamber("/api/setpoint", HTTP_POST, [this]() { handleSetpoint(); });
  onChamber("/api/thermostat", HTTP_POST, [this]() { handleThermostat(); });
  onChamber("/api/gravity/og", HTTP_POST, [this]() { handleOg(); });
  onChamber("/api/outputs/arm", HTTP_POST, [this]() { handleArm(); });
  server_.on("/api/outputs/disarm", HTTP_POST, [this]() {
    if (!auth()) return;
    sel_ = 0;
    // Forma storica, senza indice di camera: disarma TUTTO (ogni camera + ogni canale).
    r_.disarmAll();
    r_.persistNow();
    sendOk();
  });

  server_.on("/ispindel", HTTP_POST, [this]() { handleIspindel(); });
  server_.on("/api/calib", HTTP_GET, [this]() { handleCalibGet(); });
  server_.on("/api/calib", HTTP_POST, [this]() { handleCalibAdd(); });
  server_.on("/api/calib", HTTP_DELETE, [this]() { handleCalibDelete(); });
  onChamber("/api/log.csv", HTTP_GET, [this]() { handleLogCsv(); });
  onChamber("/api/log", HTTP_GET, [this]() { handleLogJson(); });
  server_.on("/api/wifi/scan", HTTP_GET, [this]() { handleWifiScan(); });
  server_.on("/api/diag/i2c", HTTP_GET, [this]() { handleDiagI2c(); });
  server_.on("/api/diag/onewire", HTTP_GET, [this]() { handleDiagOneWire(); });
  server_.on("/api/probes", HTTP_GET, [this]() { handleProbeDiag(); });
  server_.on("/api/probes/calibrate", HTTP_POST, [this]() { handleProbeCalib(); });
  server_.on("/api/probes/reset", HTTP_POST, [this]() {
    if (!auth()) return;
    r_.probes->resetStats();
    sendOk();
  });
  server_.on("/api/diag/relay", HTTP_POST, [this]() { handleDiagRelay(); });
  server_.on("/api/diag/testmode", HTTP_POST, [this]() {
    if (!auth()) return;
    r_.outputs->exitTestMode();
    sendOk();
  });
  server_.on("/api/time", HTTP_POST, [this]() { handleTime(); });
  server_.on("/api/reboot", HTTP_POST, [this]() {
    if (!auth()) return;
    sendJson(200, "{\"ok\":true,\"reboot\":true}");
    r_.scheduleReboot(1000);
  });
  server_.on("/api/factory_reset", HTTP_POST, [this]() {
    if (!auth()) return;
    sendJson(200, "{\"ok\":true,\"reboot\":true}");
    r_.factoryReset();
  });
  server_.on("/api/update", HTTP_POST, [this]() { handleUpdateFinal(); },
             [this]() { handleUpdateUpload(); });
  server_.onNotFound([this]() {
    if (server_.method() == HTTP_OPTIONS) {
      server_.send(204);
      return;
    }
    // Le rotte per camera non sono registrate: si servono qui, senza costare heap.
    if (AX_N_CHAMBERS > 1 && dispatchCamera()) return;
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
  // mandare WWW-Authenticate nella risposta 401).
  return server_.authenticate("admin", r_.cfg->uiPassword.c_str());
}

bool WebApi::auth() {
  // Keepalive su ogni richiesta, non solo sull'upload: il framework legge il CORPO con
  // attese senza tetto, quindi un peer che sparisce senza chiudere il TCP terrebbe fermo
  // il loop. Gate su ESTABLISHED perche' ClientContext::keepAlive dereferenzia _pcb senza
  // controllo di nullita' (vedi il commento in handleUpdateUpload).
  if (server_.client().status() == ESTABLISHED) server_.client().keepAlive(5, 2, 3);
  if (authValid()) return true;
  sendJson(401, "{\"ok\":false,\"error\":\"login richiesto\"}");
  return false;
}

void WebApi::handleLogin() {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, "json non valido");
    return;
  }
  String pw = doc["password"] | String("");
  if (r_.cfg->uiPassword.length() && pw != r_.cfg->uiPassword) {
    delay(300);  // rallenta la forza bruta senza bloccare a lungo il loop
    sendErr(401, "password errata");
    return;
  }
  if (tokenPass_ != r_.cfg->uiPassword) computeToken();
  server_.sendHeader("Set-Cookie", String("axs=") + sessionToken_ +
                                       "; Max-Age=31536000; Path=/; SameSite=Lax; HttpOnly");
  sendOk();
}

void WebApi::handleRoot() {
  // La pagina (solo codice UI, nessun dato) è pubblica: il login avviene in pagina
  // e ogni /api/* resta protetta. Così niente popup Basic-auth del browser.
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

void WebApi::handleStatus() {
  if (!auth()) return;
  AppStatus st = c_().status();
  // 3 KB: il caso peggiore realistico (tutti gli allarmi + nomi lunghi) supera i 2 KB
  // e ArduinoJson tronca in silenzio i campi che non entrano.
  DynamicJsonDocument doc(3072);
  doc["fw"] = AX_FW_VERSION;
  // Identita' della camera servita, piu' quante ne esistono: e' cio' che permette al
  // frontend di scoprire le camere invece di doverlo sapere a priori. A una camera sola
  // vale sempre {chamber:1, chambers:1}, quindi l'interfaccia attuale puo' ignorarli.
  doc["chamber"] = (uint8_t)(c_().chamber + 1);
  doc["chambers"] = (uint8_t)AX_N_CHAMBERS;
  doc["chamberLabel"] = r_.cfg->chamberLabel[c_().chamber];
  // Diagnostica dell'heap. Il totale libero da solo NON basta a spiegare i guasti di
  // allocazione: ArduinoJson e le String hanno bisogno di un blocco CONTIGUO, e con
  // l'heap frammentato si puo' avere molta memoria libera e nessun blocco abbastanza
  // grande. E' esattamente il meccanismo del troncamento silenzioso osservato in campo.
  // Letti qui e non da AppStatus: sono fatti di DISPOSITIVO, e AppStatus e' per camera
  // (duplicarli costerebbe byte per ogni camera senza aggiungere informazione).
  doc["heapFrag"] = ESP.getHeapFragmentation();      // %
  doc["heapMaxBlock"] = ESP.getMaxFreeBlockSize();   // piu' grande blocco contiguo
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

  JsonObject prof = doc.createNestedObject("profile");
  prof["running"] = c_().engine->running();
  prof["paused"] = st.paused;
  prof["completed"] = st.completed;
  prof["stepIdx"] = st.stepIdx;       // assoluto: per indicizzare RUNPROF.steps
  prof["stepCount"] = st.stepCount;
  prof["stepEnIdx"] = st.stepEnIdx;   // abilitati: per la visualizzazione «step N/M»
  prof["stepEnCount"] = st.stepEnCount;
  prof["waitingDelayedStart"] = st.waitingDelayedStart;
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

  JsonArray alarms = doc.createNestedArray("alarms");
  // Per primo: e' l'evento piu' grave e piu' raro, e va visto subito.
  if (st.alarmFailsafe)
    alarms.add("FAILSAFE: il loop di controllo si e' bloccato oltre il limite e il timer "
               "hardware ha spento i rele'. Il riposo del compressore e' stato riassunto. "
               "Riarma per prendere in carico l'evento");
  if (st.alarmFailsafeInerte)
    alarms.add("il timer del failsafe non sta girando: la protezione contro il blocco "
               "del loop NON e' attiva");
  if (st.alarmProbe) alarms.add("sonda di controllo guasta o assente");
  if (st.alarmOutputs) alarms.add("driver uscite (MCP23008) in errore: uscite disabilitate");
  if (st.alarmConfig) alarms.add("configurazione non valida: uscite disabilitate");
  if (st.alarmStorage) alarms.add("scrittura su flash fallita: stato e registro NON salvati (controlla lo spazio)");
  if (st.alarmThermal) alarms.add("scostamento termico: la temperatura non raggiunge il setpoint da ore (attuatore o impianto?)");
  if (st.alarmProbeSuspect) alarms.add("sonda di controllo SOSPETTA: diverge da tutte le ausiliarie (contatto termico perso?) — controlla la sonda del mosto");
  if (st.alarmRtc) alarms.add("orologio inaffidabile: impostare data/ora");
  if (st.alarmGravity) alarms.add("densita' stantia: nessun avanzamento step");
  if (st.alarmRampStall) alarms.add("rampa gradi/ora ferma: la temperatura non raggiunge il target, lo step non avanza");
  if (st.alarmBattery) alarms.add("batteria iSpindel bassa");
  if (st.alarmMultiGravity)
    alarms.add("piu' densimetri attivi senza selezione: scegli quale pilota la cotta in Calibrazione");
  if (!st.armed) alarms.add("uscite DISARMATE: confermare il mapping rele'");
  if (r_.cfg->usingDefaultUiPassword())
    alarms.add("PASSWORD DI DEFAULT attiva: cambiala in Impostazioni (OTA disattivato finche' non lo fai)");
  else if (r_.cfg->uiPassword.length() == 0)
    alarms.add("NESSUNA PASSWORD impostata: chiunque sulla LAN ha accesso completo (OTA disattivato)");

  JsonObject net = doc.createNestedObject("net");
  net["ip"] = st.ip;
  net["rssi"] = st.rssi;
  net["ap"] = st.apUp;
  net["sta"] = st.staUp;

  // Guardia contro il troncamento SILENZIOSO. /api/status e' l'endpoint che la dashboard
  // interroga di continuo, e il commento sul dimensionamento qui sopra dichiara che il
  // caso peggiore realistico di UNA camera (tutti gli allarmi + nomi lunghi) supera gia'
  // i 2 KB. ArduinoJson tronca senza dire nulla: il frontend riceverebbe un JSON monco e
  // fallirebbe il parse, con un guasto molto piu' difficile da diagnosticare di un 500.
  // Stessa scelta gia' fatta in handleProbeDiag ("meglio un errore esplicito").
  if (doc.overflowed()) {
    sendErr(500, "stato troppo grande (nomi profilo/cotta molto lunghi?)");
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
    // Il doc di parsing (6 KB) vive SOLO in questo blocco: liberarlo prima di cfgStore->save()
    // — che in ConfigStore::save alloca un ALTRO doc da 6 KB — evita di tenere due grossi
    // buffer JSON vivi insieme sull'heap ESP8266 (~17,6 KB, frammentato). Stesso schema, e
    // stessa cura, del salvataggio profili: la seconda alloc da 6 KB poteva fallire e far
    // fallire in modo intermittente il salvataggio della configurazione.
    DynamicJsonDocument doc(ConfigStore::kConfigDoc);  // derivata: vedi ConfigStore.h
    if (deserializeJson(doc, server_.arg("plain"))) {
      sendErr(400, "json non valido");
      return;
    }
    // Locking ottimistico: se il client dichiara una revisione diversa da quella corrente,
    // un altro accesso ha scritto nel frattempo (tipicamente una taratura sonde) e questo PUT
    // — costruito su dati vecchi — la cancellerebbe in silenzio (last-write-wins). Si rifiuta.
    // Un client che non manda configRev (script) non è soggetto al controllo.
    if (!doc["configRev"].isNull() && (doc["configRev"] | (uint32_t)0) != c.configRev) {
      sendErr(409, "configurazione cambiata da un altro accesso: ricarica la pagina e riprova");
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
        // /api/config e' un endpoint di DISPOSITIVO (l'unita' di densita' e' una sola per
        // tutto il firmware), quindi la guardia deve interrogare TUTTE le camere: con c_()
        // avrebbe guardato solo la prima, e si sarebbe potuta cambiare l'unita' mentre la
        // camera 2 esegue un profilo a densita'.
        bool profileAttivo = false, haStepDensita = false;
        for (uint8_t k = 0; k < AX_N_CHAMBERS; k++) {
          const ax::ProfileEngine* eng = r_.ch[k].engine;
          if (!eng) continue;
          if (eng->mode() == ax::RunMode::Profile) profileAttivo = true;
          const ax::StepDef* ps = eng->steps();
          for (uint8_t i = 0; ps && i < eng->stepCount() && !haStepDensita; i++)
            if (ps[i].enabled && ps[i].type == ax::StepType::Gravity) haStepDensita = true;
        }
        if (profileAttivo || haStepDensita) {
          sendErr(409, "unita' densita' non modificabile con un profilo a densita' caricato: cambiala fra una cotta e l'altra");
          return;
        }
      }
    }
    // Le password NON vanno troncate in silenzio (il cap generale di ConfigStore lo farebbe):
    // una uiPassword troncata escluderebbe l'utente dall'interfaccia (lockout). Si rifiuta con
    // un errore esplicito. "***" = invariata (setSecret), non va controllata.
    {
      JsonVariantConst pw = doc["system"]["uiPassword"];
      if (!pw.isNull() && pw.as<String>() != "***" && pw.as<String>().length() > 64) {
        sendErr(400, "password interfaccia troppo lunga (max 64 caratteri)");
        return;
      }
      JsonVariantConst ap = doc["network"]["softApPassword"];
      if (!ap.isNull() && ap.as<String>() != "***" && ap.as<String>().length() > 63) {
        sendErr(400, "password SoftAP troppo lunga (max 63 caratteri)");
        return;
      }
    }
    next = c;
    r_.cfgStore->fromJson(&next, doc);
  }  // doc liberato: da qui in poi nessun grosso buffer JSON del gestore è vivo
  // Unicita' degli ID ROM delle sonde. E' lo specchio esatto del conflitto relè, che
  // armOutputs rifiuta gia': due CAMERE che condividono la stessa DS18B20 come sonda di
  // controllo regolerebbero due frigoriferi diversi sulla stessa temperatura, in
  // silenzio. Nessun percorso lo impediva — ne' fromJson ne' configureProbe.
  // A camera singola un duplicato degradava solo il cross-check di plausibilita'
  // (l'ausiliaria leggeva identica al mosto, quindi minDiff ~ 0 e mai "sospetta"):
  // rifiutarlo e' comunque corretto, quella configurazione non ha alcun uso legittimo.
  for (uint8_t i = 0; i < ax::kMaxProbes; i++) {
    if (!next.probeId[i].length()) continue;  // slot vuoto: piu' slot vuoti sono leciti
    for (uint8_t j = (uint8_t)(i + 1); j < ax::kMaxProbes; j++) {
      if (next.probeId[i].equalsIgnoreCase(next.probeId[j])) {
        sendErr(400, "la stessa sonda e' assegnata a due slot: ogni DS18B20 va usata una "
                     "volta sola");
        return;
      }
    }
  }
  next.valid = true;
  next.configRev = c.configRev + 1;  // nuova revisione (fromJson non tocca configRev)
  String beforeNet = c.ssid + c.wifiPassword + String(c.dhcp) + c.ip + c.mask + c.gw + c.dns +
                     c.hostname + c.softApPassword + String(c.softApAfterMin) +
                     c.oledDriver + String(c.oledAddress) + String(c.oledEnabled) +
                     String(c.oledFlip) + String(c.mqttEnabled) + c.mqttHost +
                     String(c.mqttPort) + c.mqttUser + c.mqttPass + c.mqttBaseTopic +
                     String(c.ntpEnabled) + c.ntpServer + c.tz;
  if (!r_.cfgStore->save(next)) {
    sendErr(500, "salvataggio fallito");
    return;
  }
  String afterNet = next.ssid + next.wifiPassword + String(next.dhcp) + next.ip + next.mask +
                    next.gw + next.dns + next.hostname + next.softApPassword +
                    String(next.softApAfterMin) + next.oledDriver + String(next.oledAddress) +
                    String(next.oledEnabled) + String(next.oledFlip) + String(next.mqttEnabled) +
                    next.mqttHost + String(next.mqttPort) + next.mqttUser + next.mqttPass +
                    next.mqttBaseTopic + String(next.ntpEnabled) + next.ntpServer + next.tz;
  bool reboot = beforeNet != afterNet;
  c = next;  // applica al vivo SOLO ora che la scrittura è riuscita
  r_.applyConfigLive();
  // La nuova revisione va restituita: senza, il client resterebbe sulla vecchia e il
  // salvataggio successivo fallirebbe con un 409 falso.
  sendJson(200, String("{\"ok\":true,\"configRev\":") + c.configRev +
                    ",\"reboot\":" + (reboot ? "true" : "false") + "}");
  if (reboot) r_.scheduleReboot(1500);
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
  r_.profStore->list(arr);
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebApi::handleProfileById(int method) {
  if (!auth()) return;
  // Stessa trappola di pathArg() descritta in selectChamberFromUri: una richiesta al
  // percorso letterale "/api/profiles/{}" combacia con Uri::canHandle e lascia pathArgs
  // vuoto. Difetto PREESISTENTE (non introdotto dal passaggio a piu' camere), corretto
  // qui perche' e' esattamente lo stesso panic pre-autenticazione.
  const String idTxt = segAfter("/api/profiles/");
  if (!idTxt.length()) {
    sendErr(404, "profilo inesistente");
    return;
  }
  int id = idTxt.toInt();
  if (id < 1 || id > ProfileStore::kMaxProfiles) {
    sendErr(400, "id profilo non valido (1..20)");
    return;
  }
  if (method == HTTP_GET) {
    ax::StepDef steps[ax::kMaxSteps];
    uint8_t n = 0;
    String name;
    if (!r_.profStore->load((uint8_t)id, &name, steps, &n)) {
      // Distinzione (come il fix C30 su list()): un profilo che ESISTE ma non si è letto
      // (OOM/corrotto) non deve fingersi assente, o il client lo crederebbe libero.
      if (r_.profStore->exists((uint8_t)id))
        sendErr(500, "profilo illeggibile (memoria insufficiente o file corrotto)");
      else
        sendErr(404, "profilo inesistente");
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
      sendErr(404, "profilo inesistente");
      return;
    }
    sendOk();
    return;
  }
  // PUT: crea o aggiorna.
  String name;
  ax::StepDef steps[ax::kMaxSteps];
  uint8_t n = 0;
  {
    // Il doc di parsing (6 KB) vive SOLO in questo blocco: liberarlo prima di save() evita
    // di tenere due grossi buffer JSON vivi insieme sull'heap ESP8266 (causa accertata dei
    // profili "{}" e dei "salvataggio fallito" intermittenti su heap frammentato).
    DynamicJsonDocument doc(ProfileStore::kProfileDoc);  // derivata: vedi ProfileStore.h
    if (deserializeJson(doc, server_.arg("plain"))) {
      sendErr(400, "json non valido");
      return;
    }
    name = doc["name"] | String("");
    if (name.length() == 0) {
      sendErr(400, "nome profilo mancante");
      return;
    }
    // Troncare in silenzio faceva credere salvato un nome diverso da quello scritto,
    // e due profili potevano finire con lo stesso nome accorciato. String misura BYTE.
    if (name.length() > 32) {
      sendErr(400, "nome troppo lungo (max 32 byte)");
      return;
    }
    n = ProfileStore::stepsFromJson(doc, steps);
  }
  const char* err = validateSteps(steps, n);
  if (err) {
    sendErr(400, err);
    return;
  }
  if (!r_.profStore->save((uint8_t)id, name, steps, n)) {
    sendErr(500, "salvataggio fallito");
    return;
  }
  sendOk();
}

void WebApi::handlePutProfile() {
  if (!auth()) return;
  // Anche a profilo COMPLETED il setpoint mantenuto deriva dagli step: si modifica
  // solo a modalità profilo disattivata.
  if (c_().engine->mode() == ax::RunMode::Profile) {
    sendErr(409, "profilo in corso o completato: fermarlo prima di modificarlo");
    return;
  }
  ax::StepDef steps[ax::kMaxSteps];
  uint8_t n = 0;
  String name;
  {
    // doc di parsing liberato prima di saveActive(): un solo grosso buffer JSON alla
    // volta sull'heap (vedi handleProfileById).
    DynamicJsonDocument doc(ProfileStore::kProfileDoc);  // derivata: vedi ProfileStore.h
    if (deserializeJson(doc, server_.arg("plain"))) {
      sendErr(400, "json non valido");
      return;
    }
    n = ProfileStore::stepsFromJson(doc, steps);
    name = doc["name"] | (c_().activeProfileName ? *c_().activeProfileName : String(""));
  }
  const char* err = validateSteps(steps, n);
  if (err) {
    sendErr(400, err);
    return;
  }
  if (name.length() > 32) {
    sendErr(400, "nome troppo lungo (max 32 byte)");
    return;
  }
  // Il motore si aggiorna SOLO dopo che la scrittura su flash è riuscita: applicarlo
  // prima significherebbe che una richiesta rifiutata lascia in RAM un piano diverso
  // da quello su /profile.json, e al riavvio la fermentazione cambierebbe da sola.
  if (!r_.profStore->saveActive(c_().chamber, name, steps, n)) {
    sendErr(500, "salvataggio fallito");
    return;
  }
  c_().engine->setSteps(steps, n);
  if (c_().activeProfileName) *c_().activeProfileName = name;
  sendOk();
}

// Cold crash su richiesta. Non è un cambio di setpoint: è una discesa a gradini con
// due soste, perché è la sosta che fa cadere il lievito, non il numero sul termostato.
// Numeri e ragioni (fonti in docs/PROFILI.md): 5 °C o meno bastano per la limpidezza e
// già 24 h danno un miglioramento netto (BYO); sotto i 2-4 °C il lievito ancora in
// sospensione si danneggia e si rischia autolisi (Escarpment), e in un fermentatore
// casalingo non lo si può spurgare prima — per questo ci si ferma a 2 °C e non a 0.
// La velocità di discesa non cambia il risultato in cieco (Brulosophy), ma la discesa
// lenta limita il risucchio dal gorgogliatore in un fermentatore non in pressione.
void WebApi::handleColdCrash() {
  if (!auth()) return;
  float target = 2.0f;
  float giorni = 3.0f;
  bool anteprima = false;
  if (server_.arg("plain").length()) {
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, server_.arg("plain"))) {
      sendErr(400, "json non valido");
      return;
    }
    if (!doc["targetC"].isNull()) {
      if (!doc["targetC"].is<float>() && !doc["targetC"].is<int>()) {
        sendErr(400, "temperatura finale non numerica");
        return;
      }
      target = doc["targetC"];
    }
    if (!doc["holdDays"].isNull()) {
      if (!doc["holdDays"].is<float>() && !doc["holdDays"].is<int>()) {
        sendErr(400, "durata non numerica");
        return;
      }
      giorni = doc["holdDays"];
    }
    anteprima = doc["preview"] | false;
  }
  if (isnan(target) || target < -1.0f || target > 12.0f) {
    sendErr(400, "temperatura finale fuori range (-1..12 \xC2\xB0""C)");
    return;
  }
  if (isnan(giorni) || giorni < 0.5f || giorni > 30.0f) {
    sendErr(400, "durata fuori range (0,5..30 giorni)");
    return;
  }
  if (c_().engine->snapshot().delayedStartEpoch != 0) {
    sendErr(409, "partenza ritardata in attesa: annullala prima del cold crash");
    return;
  }
  ProbeBus::Reading ctl = r_.probes->reading(*c_().controlProbe);
  if (!ctl.valid) {
    // Senza sonda il frigo lavorerebbe alla cieca: una discesa non controllata può
    // congelare la birra.
    sendErr(409, "sonda di controllo non valida: impossibile avviare il cold crash");
    return;
  }

  // Il piano si costruisce sulla temperatura MISURATA e scende sempre: una gamba che
  // "sale" con il riscaldamento disabilitato tiene solo il frigo fermo, e su una birra
  // già fredda — il caso più comune, visto che quasi tutti i profili chiudono al freddo —
  // significherebbe lasciarla risalire verso l'ambiente per un giorno intero.
  const float ora = ctl.c;
  float sosta = target + 3.0f > 5.0f ? 5.0f : target + 3.0f;
  if (sosta < target) sosta = target;
  if (sosta > ora) sosta = ora;

  auto oreDiscesa = [](float da, float a, float gradiOra) {
    float d = da > a ? da - a : 0.0f;
    float h = d / gradiOra;
    return (uint32_t)((h < 1.0f ? 1.0f : h) * 3600.0f);
  };

  ax::StepDef steps[4];
  uint8_t n = 0;
  auto aggiungi = [&](ax::StepType tipo, float t, uint32_t dur) {
    steps[n] = ax::StepDef{};
    steps[n].type = tipo;
    steps[n].enabled = true;
    steps[n].targetC = t;
    steps[n].durationSec = dur;
    steps[n].allowHeat = false;  // il riscaldatore non deve contrastare la discesa
    n++;
  };
  // Sosta intermedia solo se c'è davvero da scendere prima e da scendere ancora dopo:
  // è lì che cade il grosso del lievito, sopra la soglia in cui inizia a soffrire.
  if (ora - sosta >= 0.5f && sosta - target >= 0.5f) {
    aggiungi(ax::StepType::RampTime, sosta, oreDiscesa(ora, sosta, 0.5f));
    aggiungi(ax::StepType::Fixed, sosta, 24 * 3600);
  }
  const float da = n ? sosta : ora;
  if (da - target >= 0.5f)
    aggiungi(ax::StepType::RampTime, target, oreDiscesa(da, target, n ? 0.25f : 0.5f));
  aggiungi(ax::StepType::Fixed, target, (uint32_t)(giorni * 86400.0f));

  // Il piano viene restituito sempre: la conferma nella pagina si costruisce da qui,
  // così non può descrivere una discesa diversa da quella che il firmware esegue.
  DynamicJsonDocument out(512);
  out["ok"] = true;
  out["preview"] = anteprima;
  out["fromC"] = serialized(String(ora, 1));
  uint32_t totale = 0;
  JsonArray piano = out.createNestedArray("plan");
  for (uint8_t i = 0; i < n; i++) {
    JsonObject o = piano.createNestedObject();
    o["type"] = steps[i].type == ax::StepType::RampTime ? "ramp" : "hold";
    o["targetC"] = serialized(String(steps[i].targetC, 1));
    o["sec"] = steps[i].durationSec;
    totale += steps[i].durationSec;
  }
  out["totalSec"] = totale;

  if (!anteprima) {
    // Prima la flash, poi il motore: fermare il profilo e scoprire solo dopo che la
    // scrittura è fallita lascerebbe la cotta senza controllo di temperatura, in
    // silenzio. È la stessa invariante di handlePutProfile e di startProfileCmd.
    const String nome = "Cold crash";
    if (!r_.profStore->saveActive(c_().chamber, nome, steps, n)) {
      sendErr(500, "salvataggio fallito: il profilo in corso non è stato toccato");
      return;
    }
    c_().engine->stop();
    c_().engine->setSteps(steps, n);
    if (c_().activeProfileName) *c_().activeProfileName = nome;
    // Si avvia il motore direttamente, senza passare dal comando di avvio profilo: quello
    // azzererebbe l'OG della cotta, e con essa attenuazione e ABV finali.
    if (!c_().engine->startProfile(r_.time->nowEpoch(), ctl.c, ctl.valid, 0)) {
      sendErr(400, "avvio del cold crash fallito");
      return;
    }
    r_.persistNow();
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
      sendErr(409, "profilo gia' in corso: fermalo prima di riavviarlo");
      return;
    }
    uint32_t delayedAt = 0;
    int profileId = 0;
    String batch = "";
    if (server_.arg("plain").length()) {
      DynamicJsonDocument doc(512);  // heap: il percorso di start è profondo di stack
      if (deserializeJson(doc, server_.arg("plain"))) {
        sendErr(400, "json non valido");  // mai avviare col profilo sbagliato in silenzio
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
      sendErr(404, "profilo inesistente");
      return;
    }
    if (rc == 500) {
      sendErr(500, "scrittura del profilo attivo fallita");
      return;
    }
    if (rc == 409) {
      // Messaggio azionabile, non generico: dice cosa manca E come rimediare. Un
      // "nessuno step abilitato" qui manderebbe l'utente a cercare il difetto nel
      // profilo, che invece e' corretto.
      sendErr(409, "il profilo ha step a densita' ma questa camera non ha un densimetro "
                   "assegnato: assegnalo in Impostazioni, oppure disabilita quegli step");
      return;
    }
    if (rc != 0) {
      sendErr(400, "nessuno step abilitato");
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
      sendErr(400, "nessun profilo in corso");
      return;
    }
  }
  r_.persistNow();
  sendOk();
}

void WebApi::handleSetpoint() {
  if (!auth()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server_.arg("plain")) || doc["value"].isNull()) {
    sendErr(400, "atteso {\"value\": <float>}");
    return;
  }
  float v = doc["value"];
  // Floor a -1 (antigelo), coerente con validateSteps e cold crash: sotto -1 l'overshoot
  // del frigo porta la birra verso il congelamento. Prima accettava fino a -10.
  // isfinite: un NaN passerebbe il solo controllo di range (ogni confronto con NaN è falso)
  // e finirebbe in /api/status come "nan", rendendo illeggibile l'intera risposta.
  if (!isfinite(v) || v < -1 || v > 40) {
    sendErr(400, "setpoint fuori range (-1..40)");
    return;
  }
  // overrideSetpoint è un no-op a modalità Off o a profilo completato: rispondere 200 lì
  // farebbe credere applicato un setpoint che resta invariato. Meglio un 409 esplicito.
  ax::RunMode m = c_().engine->mode();
  bool delayedWait = c_().engine->snapshot().delayedStartEpoch != 0;
  if (m == ax::RunMode::Off ||
      (m == ax::RunMode::Profile && (c_().engine->completed() || delayedWait))) {
    sendErr(409, "nessun setpoint attivo da modificare: avvia un profilo o il termostato");
    return;
  }
  c_().engine->overrideSetpoint(v);
  r_.persistNow();
  sendOk();
}

void WebApi::handleThermostat() {
  if (!auth()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, "json non valido");
    return;
  }
  bool on = doc["on"] | false;
  float sp = doc["setpoint"] | c_().engine->thermostatSetpoint();
  // Accendendo si valida il setpoint per dare un 400 esplicito. (Spegnendo non si rifiuta: il
  // clamp in setThermostat sotto porta comunque a range QUALSIASI valore — anche un +inf da
  // "1e40" del client → 40 — quindi niente valore pericoloso né JSON di /status rotto.)
  if (on && (!isfinite(sp) || sp < -1 || sp > 40)) {
    sendErr(400, "setpoint fuori range (-1..40)");
    return;
  }
  // Passare al termostato mentre un profilo è in CORSA azzera lo snapshot del motore (setThermostat
  // fa s_ = EngineSnapshot{}): step/giorni/OG/batch persi e cotta abbandonata in SILENZIO — e questo
  // vale ANCHE spegnendo (on=false), non solo accendendo. Per fermare una cotta si usa lo stop del
  // profilo. Il guard copre entrambi i versi. (Il gemello MQTT climate_mode=auto agisce solo da Off.)
  if (c_().engine->running()) {
    sendErr(409, "profilo in corso: fermalo (stop profilo) prima di usare il termostato");
    return;
  }
  // setThermostat clampa il setpoint a [-1,40] da qualunque sorgente: un client non può memorizzare
  // un valore fuori range o non-finito (che romperebbe /status via serialized(String(sp,2))).
  c_().engine->setThermostat(on, sp);
  r_.persistNow();
  sendOk();
}

void WebApi::handleArm() {
  if (!auth()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, "json non valido");
    return;
  }
  uint8_t cool = doc["cool"] | 0;
  uint8_t heat = doc["heat"] | 0;
  if (cool == heat || cool > 8 || heat > 8) {
    sendErr(400, "mapping non valido: cool e heat devono essere rele' distinti (1..8, 0=nessuno)");
    return;
  }
  if (!c_().arm(cool, heat)) {
    sendErr(409, "impossibile armare: esci dalla modalita' test, oppure il rele' e' "
                 "gia' assegnato a un'altra camera");
    return;
  }
  r_.persistNow();
  sendOk();
}

void WebApi::handleIspindel() {
  // Nessuna auth di sessione: e' lo iSpindel a pubblicare (formato Generic HTTP JSON).
  // Difese contro iniezioni da chiunque sulla LAN: token opzionale + clamp di plausibilita'.
  DynamicJsonDocument doc(768);
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, "json non valido");
    return;
  }
  if (r_.cfg->ispindelToken.length()) {
    String tok = doc["token"] | String("");
    if (tok != r_.cfg->ispindelToken) {
      sendErr(401, "token non valido");
      return;
    }
  }
  if (doc["gravity"].isNull()) {
    sendErr(400, "campo gravity mancante");
    return;
  }
  float gravity = doc["gravity"];
  float temp = doc["temperature"] | 0.0f;
  float batt = doc["battery"] | 0.0f;
  float angle = doc["angle"] | 0.0f;
  int rssi = doc["RSSI"] | (int)(doc["rssi"] | 0);
  // Range plausibili (SG 0.98..1.20; Plato -1..40; temperatura -10..50): fuori = scartato,
  // cosi' una lettura falsa non sposta l'avanzamento di uno step a densita'.
  bool plato = r_.cfg->gravityUnit == "plato";
  float gmin = plato ? -1.0f : 0.90f, gmax = plato ? 45.0f : 1.30f;
  if (gravity < gmin || gravity > gmax || temp < -10.0f || temp > 50.0f) {
    sendErr(400, "valori fuori range plausibile");
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
    sendErr(400, "json non valido");
    return;
  }
  float og = doc["og"] | 0.0f;  // 0 (o assente) = azzera -> ricattura automatica
  if (og > 0) {
    // Clamp di plausibilita' (come /ispindel): evita che un refuso tipo 1010 al posto
    // di 1.010 avveleni le metriche di fermentazione.
    bool plato = r_.cfg->gravityUnit == "plato";
    float gmin = plato ? 1.0f : 1.0f, gmax = plato ? 40.0f : 1.20f;
    if (og < gmin || og > gmax) {
      sendErr(400, plato ? "OG fuori range (1..40 P)" : "OG fuori range (1.000..1.200 SG)");
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
    // Taratura della camera 0: e' il riferimento storico del pannello. La mappa
    // completa camera->densimetro sta in "chambers" piu' sotto, ed e' da li' che
    // l'interfaccia assegna gli strumenti alle camere.
    doc["correction"] = serialized(String(r_.cfg->gravityCorrection[0], 4));
    doc["deviceId"] = r_.gravityAny->deviceId();
    doc["deviceName"] = r_.calibDeviceName ? *r_.calibDeviceName : String("");
  }
  doc["unit"] = r_.cfg->gravityUnit;
  doc["maxPoints"] = CalibStore::kMaxPoints;
  doc["activeDevice"] = r_.cfg->gravityDevice[0];  // storico: la camera 0
  // Mappa densimetro->camera (D7). Senza questa, una camera diversa dalla prima
  // resterebbe inassegnabile dall'interfaccia.
  {
    JsonArray cams = doc.createNestedArray("chambers");
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
      JsonObject o = cams.createNestedObject();
      o["label"] = r_.cfg->chamberLabel[i];
      o["densimetro"] = r_.cfg->gravityDevice[i];
      o["correzione"] = serialized(String(r_.cfg->gravityCorrection[i], 4));
    }
  }
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
  if (doc.overflowed()) { sendErr(500, "calibrazione troppo grande"); return; }  // no punti persi in silenzio
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebApi::handleCalibAdd() {
  if (!auth()) return;
  if (!r_.calib) {
    sendErr(500, "archivio calibrazione non disponibile");
    return;
  }
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server_.arg("plain"))) {
    sendErr(400, "json non valido");
    return;
  }
  if (doc["gravity"].isNull()) {
    sendErr(400, "densita' misurata mancante");
    return;
  }
  float g = doc["gravity"];
  // Stessi limiti di plausibilità dell'ingest iSpindel: un refuso di battitura
  // (1.40 invece di 1.040) deformerebbe il polinomio su tutta la curva.
  bool plato = r_.cfg->gravityUnit == "plato";
  if (plato ? (g < -1 || g > 45) : (g < 0.90f || g > 1.30f)) {
    sendErr(400, "densita' fuori range plausibile");
    return;
  }
  // Mescolare SG e °Plato nello stesso dataset produce una regressione insensata.
  if (r_.calib->count() > 0 && r_.calib->storedPlato() != plato) {
    sendErr(409, "i punti esistenti usano un'altra unita': azzera la calibrazione");
    return;
  }
  if (!r_.gravityAny->hasValue()) {
    sendErr(409, "nessuna lettura dall'iSpindel: attendi il primo invio");
    return;
  }
  int64_t age = r_.gravityAny->signedAgeSec(r_.time->nowEpoch());
  if (age < 0) {
    sendErr(409, "orologio incoerente con la lettura: sistema data e ora");
    return;
  }
  if (age > kCalibMaxAgeSec) {
    sendErr(409, "lettura troppo vecchia: attendi il prossimo invio dell'iSpindel");
    return;
  }
  uint32_t readEpoch = r_.gravityAny->updatedEpoch();
  // La lettura deve essere ARRIVATA DOPO l'immersione dichiarata dal client:
  // altrimenti si registrerebbe l'angolo della soluzione precedente.
  if (!doc["afterEpoch"].isNull()) {
    uint32_t after = doc["afterEpoch"];
    if (readEpoch <= after) {
      sendErr(409, "lettura precedente all'immersione: attendi il prossimo invio");
      return;
    }
  }
  // Due punti dalla stessa trasmissione avrebbero lo stesso angolo.
  if (r_.calib->hasReadEpoch(readEpoch)) {
    sendErr(409, "punto gia' registrato per questa lettura: attendi la prossima");
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
    sendErr(409, "angolo non disponibile: aggiorna il firmware dell'iSpindel");
    return;
  }
  if (!r_.calib->add(p)) {
    sendErr(409, "numero massimo di punti raggiunto");
    return;
  }
  if (!r_.calib->save()) {
    r_.calib->remove(r_.calib->count() - 1);  // RAM e disco non devono divergere
    sendErr(500, "salvataggio fallito");
    return;
  }
  sendOk();
}

void WebApi::handleCalibDelete() {
  if (!auth()) return;
  if (!r_.calib) {
    sendErr(500, "archivio calibrazione non disponibile");
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
      sendErr(404, "punto inesistente");
      return;
    }
    r_.calib->remove((uint8_t)idx);
  } else {
    r_.calib->clear();
  }
  if (!r_.calib->save()) {
    r_.calib->load();  // ripristina lo stato reale del file
    sendErr(500, "salvataggio fallito");
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
  DynamicJsonDocument doc((size_t)ProbeBus::kProbes * 1024u);
  uint32_t up = millis();
  doc["uptimeSec"] = up / 1000;
  // Endpoint di DISPOSITIVO: interessa se sta fermentando una camera QUALSIASI,
  // non solo la prima (c_() qui varrebbe sempre la camera 0).
  doc["armed"] = r_.anyArmed ? r_.anyArmed() : c_().isArmed();
  // Storico degli avvii: un riavvio non sorvegliato lascia altrimenti solo un buco
  // nel registro, indistinguibile da un blackout.
  if (r_.boots) {
    JsonArray bs = doc.createNestedArray("boots");
    for (uint8_t i = 0; i < r_.boots->count(); i++) {
      const BootEntry* b = r_.boots->entry(i);
      JsonObject o = bs.createNestedObject();
      o["t"] = b->epoch;
      o["r"] = b->reason;
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
    sendErr(500, "diagnostica troppo grande");
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
    sendErr(400, "json non valido");
    return;
  }
  // Il riferimento deve essere un numero: un "0" testuale o una stringa vuota
  // verrebbero letti come 0 °C — il punto di ghiaccio — falsando ogni offset.
  if (!doc["reference"].is<float>() && !doc["reference"].is<int>()) {
    sendErr(400, "temperatura di riferimento mancante o non numerica");
    return;
  }
  float ref = doc["reference"];
  if (isnan(ref) || isinf(ref) || ref < -20.0f || ref > 60.0f) {
    sendErr(400, "riferimento fuori range (-20..60 \xC2\xB0""C)");
    return;
  }
  // La selezione è obbligatoria: "campo assente = tara tutte" è il default
  // sbagliato da cui difendersi, perché applicherebbe il bagno di riferimento
  // anche a sonde rimaste nel fermentatore.
  if (!doc["probes"].is<JsonArray>()) {
    sendErr(400, "elenco sonde mancante");
    return;
  }
  JsonArray sel = doc["probes"];
  bool wanted[ProbeBus::kProbes] = {false};
  uint8_t nSel = 0;
  for (JsonVariant v : sel) {
    if (!v.is<int>()) {
      sendErr(400, "indice sonda non valido");
      return;
    }
    int idx = v.as<int>();
    if (idx < 0 || idx >= (int)ProbeBus::kProbes) {
      sendErr(400, "indice sonda fuori range");
      return;
    }
    if (!wanted[idx]) { wanted[idx] = true; nSel++; }
  }
  if (!nSel) {
    sendErr(400, "nessuna sonda selezionata");
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
    sendErr(409, "sonda di controllo di una camera armata o con profilo in marcia: "
                 "disarma quella camera (o metti in pausa il profilo) prima di tararla");
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
    sendErr(500, "salvataggio fallito");
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
  if (out.overflowed()) { sendErr(500, "risposta taratura troppo grande"); return; }
  String body;
  serializeJson(out, body);
  sendJson(200, body);
}

void WebApi::handleLogCsv() {
  if (!auth()) return;
  uint32_t hours = server_.hasArg("hours") ? server_.arg("hours").toInt() : 0;
  uint32_t now = r_.time->nowEpoch();
  uint32_t from = hours > 0 && now > hours * 3600 ? now - hours * 3600 : 0;
  // Mai iterare oltre la retention (senza 'hours' un epoch reale farebbe girare
  // il loop su ~20 000 giorni inesistenti).
  uint32_t retSec = (uint32_t)r_.cfg->logRetentionDays * 86400UL;
  if (now > retSec && from < now - retSec) from = now - retSec;
  const bool plato = r_.cfg->gravityUnit == "plato";
  const float gdiv = plato ? 100.0f : 10000.0f;
  server_.chunkedResponseModeStart(200, "text/csv");
  server_.sendContent("epoch;data_locale;t_controllo;t_aux1;t_aux2;setpoint;caldo;freddo;"
                      "armato;allarme;densita;batteria\r\n");
  String line;
  line.reserve(128);
  // Tetto di righe: l'export gira in modo sincrono dentro il loop, quindi ferma il
  // controllo per la sua durata. Il caso patologico (campionamento fitto su lunga
  // retention) è limitato qui; oltre il tetto si tronca in modo VISIBILE, non in silenzio.
  const uint32_t kMaxRows = 50000;
  uint32_t rows = 0;
  bool truncated = false;
  r_.log->forEach(from, now, c_().chamber, [&](const DataLog::Sample& s) {
    // L'export gira SINCRONO dentro il loop e con il tetto di 50.000 righe supera
    // largamente i 60 s del failsafe: senza questo, un download del registro farebbe
    // scattare la protezione su un'operazione perfettamente legittima, imponendo il
    // riposo pieno del compressore (5 min di default) e, con un client che scarica a
    // ripetizione, mandando la cotta in deriva termica. Il failsafe verrebbe cioe' a
    // danneggiare cio' che deve proteggere.
    // Si rinvia SOLO qui, dove c'e' progresso dimostrato (una riga prodotta): e' la
    // differenza con il blocco che il failsafe deve intercettare, dove il loop e'
    // parcheggiato in attesa e non avanza di un byte.
    if (r_.progress) r_.progress();
    if (rows++ >= kMaxRows) { truncated = true; return false; }
    line = String(s.epoch) + ";" + r_.time->localString(s.epoch) + ";";
    line += s.tControl == DataLog::kMissing ? "" : String(s.tControl / 100.0f, 2);
    line += ";";
    line += s.tAux1 == DataLog::kMissing ? "" : String(s.tAux1 / 100.0f, 2);
    line += ";";
    line += s.tAux2 == DataLog::kMissing ? "" : String(s.tAux2 / 100.0f, 2);
    line += ";";
    line += (s.flags & 0x10) && s.setpoint != DataLog::kMissing
                ? String(s.setpoint / 100.0f, 2) : "";
    line += ";";
    line += (s.flags & 0x01) ? "1;" : "0;";
    line += (s.flags & 0x02) ? "1;" : "0;";
    line += (s.flags & 0x04) ? "1;" : "0;";
    line += (s.flags & 0x08) ? "1;" : "0;";
    line += s.gravity ? String(s.gravity / gdiv, plato ? 2 : 4) : "";
    line += ";";
    line += s.battery ? String(s.battery / 20.0f, 2) : "";
    line += "\r\n";
    server_.sendContent(line);
    return true;
  });
  if (truncated)
    server_.sendContent("# export troncato a 50000 righe: restringi con ?hours=<n>\r\n");
  server_.chunkedResponseFinalize();
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
  // Decimazione: mai piu' di ~600 punti al grafico.
  uint32_t expected = hours * 3600 / ((uint32_t)r_.cfg->logSampleMin * 60);
  uint32_t stride = expected > 600 ? (expected + 599) / 600 : 1;

  server_.chunkedResponseModeStart(200, "application/json");
  server_.sendContent("[");
  bool first = true;
  uint32_t i = 0;
  String obj;
  obj.reserve(160);
  r_.log->forEach(from, now, c_().chamber, [&](const DataLog::Sample& s) {
    // L'export gira SINCRONO dentro il loop e con il tetto di 50.000 righe supera
    // largamente i 60 s del failsafe: senza questo, un download del registro farebbe
    // scattare la protezione su un'operazione perfettamente legittima, imponendo il
    // riposo pieno del compressore (5 min di default) e, con un client che scarica a
    // ripetizione, mandando la cotta in deriva termica. Il failsafe verrebbe cioe' a
    // danneggiare cio' che deve proteggere.
    // Si rinvia SOLO qui, dove c'e' progresso dimostrato (una riga prodotta): e' la
    // differenza con il blocco che il failsafe deve intercettare, dove il loop e'
    // parcheggiato in attesa e non avanza di un byte.
    if (r_.progress) r_.progress();
    if (i++ % stride != 0) return true;
    obj = first ? "" : ",";
    first = false;
    obj += "{\"e\":" + String(s.epoch);
    if (s.tControl != DataLog::kMissing) obj += ",\"t\":" + String(s.tControl / 100.0f, 2);
    if (s.tAux1 != DataLog::kMissing) obj += ",\"a\":" + String(s.tAux1 / 100.0f, 2);
    if (s.tAux2 != DataLog::kMissing) obj += ",\"b\":" + String(s.tAux2 / 100.0f, 2);
    if ((s.flags & 0x10) && s.setpoint != DataLog::kMissing)
      obj += ",\"s\":" + String(s.setpoint / 100.0f, 2);
    if (s.gravity) obj += ",\"g\":" + String(s.gravity / gdiv, plato ? 2 : 4);
    obj += ",\"h\":" + String((s.flags & 0x01) ? 1 : 0);
    obj += ",\"c\":" + String((s.flags & 0x02) ? 1 : 0);
    obj += "}";
    server_.sendContent(obj);
    return true;
  });
  server_.sendContent("]");
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
    sendErr(400, "json non valido");
    return;
  }
  uint8_t n = doc["n"] | 0;
  bool on = doc["on"] | false;
  if (n < 1 || n > 8) {
    sendErr(400, "n deve essere 1..8");
    return;
  }
  // Stato live (non la cache di status, vecchia fino a 1 s) e su TUTTE le camere: gli 8
  // canali sono del dispositivo, e un test puo' eccitare un carico mentre un'altra
  // camera sta fermentando. Stessa regola della CLI seriale (anyChamberArmed).
  bool armed = r_.anyArmed ? r_.anyArmed() : c_().isArmed();
  if (armed) {
    sendErr(409, "almeno una camera e' ARMATA: disarmare prima del test rele'");
    return;
  }
  if (!r_.outputs->testMode() && !r_.outputs->enterTestMode(armed)) {
    sendErr(409, "impossibile entrare in modalita' test");
    return;
  }
  if (!r_.outputs->testRelay(n, on)) {
    sendErr(500, "comando rele' fallito (MCP23008 assente?)");
    return;
  }
  sendOk();
}

void WebApi::handleTime() {
  if (!auth()) return;
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server_.arg("plain")) || doc["epoch"].isNull()) {
    sendErr(400, "atteso {\"epoch\": <utc>}");
    return;
  }
  if (!r_.time->setManual(doc["epoch"])) {
    sendErr(400, "epoch non plausibile");
    return;
  }
  r_.persistNow();
  sendOk();
}

void WebApi::handleUpdateFinal() {
  // Doppio controllo: sia il flag dell'upload sia una verifica auth fresca sulla
  // richiesta finale (una POST senza multipart non attiva l'upload handler e non deve
  // poter cavalcare un flag rimasto da un upload precedente annullato).
  if (!otaAuthorized_ || !authValid()) {
    otaAuthorized_ = false;
    sendErr(401, "non autorizzato");
    return;
  }
  otaAuthorized_ = false;
  if (Update.hasError()) {
    sendErr(500, "aggiornamento fallito");
    return;
  }
  sendJson(200, "{\"ok\":true,\"reboot\":true}");
  r_.scheduleReboot(1500);
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
    otaAuthorized_ = r_.cfg->otaEnabled && authValid() &&
                     !r_.cfg->usingDefaultUiPassword() && r_.cfg->uiPassword.length() > 0;
    if (r_.cfg->usingDefaultUiPassword() || r_.cfg->uiPassword.length() == 0)
      Serial.println(F("[ota] RIFIUTATO: impostare una password UI (non default, non vuota) prima di aggiornare"));
    if (!otaAuthorized_) {
      // Si distingue il NON AUTENTICATO da chi e' autenticato ma ha l'OTA sbarrata
      // (password di default o vuota, o ota disabilitata). Al primo si chiude la
      // connessione: il parser continuerebbe altrimenti a consumare l'intero corpo, e una
      // richiesta senza credenziali comprerebbe tempo di loop. Al secondo NO: merita di
      // ricevere l'errore da handleUpdateFinal, invece di vedersi cadere la connessione
      // senza sapere perche'.
      if (!authValid()) server_.client().stop();
      return;
    }
    // NB: durante il flash il loop è congelato e i relè restano latchati nell'ultimo stato per la
    // durata di upload+reboot (residuo MINORE noto: massa termica di ore → frazione di grado, azione
    // rara/autenticata). NON si fa allOff() qui: spegnere il compressore senza sincronizzare la
    // guardia (assumeJustStopped) provocherebbe, su OTA ABORTITA (nessun reboot), un corto ciclo alla
    // ripresa del loop (guardia con on_ stantio → mustStayOn rieccita subito). La guardia va tenuta
    // coerente; il rischio dell'OTA-freeze è inferiore a quello del corto ciclo del compressore.
    uint32_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    Serial.printf("[ota] upload %s (max %u B)\n", up.filename.c_str(), maxSpace);
    if (!Update.begin(maxSpace)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    // Progresso dimostrato: un blocco e' stato ricevuto e scritto. Rinviando qui, la
    // durata dell'OTA smette di contare contro il failsafe — e cade il margine sottile
    // che i 60 s lasciavano su un firmware da ~730 KB con WiFi lento. Restano coperte le
    // attese SENZA progresso, che sono esattamente quelle da intercettare.
    if (r_.progress) r_.progress();
    if (otaAuthorized_ && Update.write(up.buf, up.currentSize) != up.currentSize)
      Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaAuthorized_) {
      if (Update.end(true)) Serial.printf("[ota] ok, %u B\n", up.totalSize);
      else Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    otaAuthorized_ = false;  // non lasciare l'autorizzazione appesa a un upload interrotto
    Update.end(false);
  }
}
