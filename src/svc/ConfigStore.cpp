#include "svc/ConfigStore.h"
#include <LittleFS.h>
#include "svc/FsCommit.h"

static const char* kMask = "***";

bool ConfigStore::load(AppConfig* cfg) {
  File f = LittleFS.open(kPath, "r");
  if (!f) return false;
  DynamicJsonDocument doc(kConfigDoc);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  fromJson(cfg, doc);
  cfg->configRev = doc["configRev"] | (uint32_t)0;  // letta a parte: fromJson non la tocca
  cfg->valid = true;
  // Una configurazione scritta da un firmware precedente puo' avere valori che oggi stanno
  // fuori dagli intervalli plausibili: vengono riportati nei limiti, e va detto.
  const String corretti = correctedFields(doc, *cfg);
  if (corretti.length())
    Serial.printf_P(PSTR("[cfg] valori riportati nei limiti all'avvio: %s (salva le impostazioni "
                         "per renderlo definitivo)\n"), corretti.c_str());
  return true;
}

bool ConfigStore::save(const AppConfig& cfg) {
  DynamicJsonDocument doc(kConfigDoc);
  toJson(cfg, doc, false);
  // Se la config non entra nel buffer, meglio fallire che scrivere un JSON monco (le
  // sezioni finali — mqtt/ota — sparirebbero in silenzio e non si ricaricherebbero al boot).
  if (doc.overflowed()) return false;
  File f = LittleFS.open("/config.tmp", "w");
  if (!f) return false;
  const size_t want = measureJson(doc);
  size_t n = serializeJson(doc, f);
  f.close();
  // Con filesystem pieno serializeJson scrive solo i primi chunk: n>0 ma n<misura.
  // Promuovere un tmp troncato distruggerebbe il config buono (al boot deserializeJson
  // fallirebbe → default + uscite disabilitate). Stessa guardia di Calib/State/Boot/Devices.
  if (n != want) {
    LittleFS.remove("/config.tmp");
    return false;
  }
  return fsCommitAtomic("/config.tmp", kPath, want);  // rename atomico + verifica del flush
}

void ConfigStore::toJson(const AppConfig& c, JsonDocument& doc, bool mask) const {
  doc["v"] = AppConfig::kSchemaVersion;  // versione dello schema (assente = v1)
  doc["configRev"] = c.configRev;  // per il locking ottimistico (vedi handlePutConfig)
  JsonArray chambers = doc.createNestedArray("chambers");
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
    JsonObject cam = chambers.createNestedObject();
    cam["label"] = c.chamberLabel[i];
    // Densimetro e sua taratura: appartengono alla CAMERA perché appartengono allo
    // strumento immerso in quel fermentatore.
    cam["densimetro"] = c.gravityDevice[i];
    cam["correzione"] = c.gravityCorrection[i];
  }
  JsonObject sys = doc.createNestedObject("system");
  sys["hostname"] = c.hostname;
  sys["uiPassword"] = mask ? String(kMask) : c.uiPassword;
  sys["pwdAuto"] = c.pwdAuto;

  JsonObject net = doc.createNestedObject("network");
  net["ssid"] = c.ssid;
  net["password"] = mask ? String(kMask) : c.wifiPassword;
  net["dhcp"] = c.dhcp;
  net["ip"] = c.ip;
  net["mask"] = c.mask;
  net["gw"] = c.gw;
  net["dns"] = c.dns;
  net["softApPassword"] = mask ? String(kMask) : c.softApPassword;
  net["softApAfterMin"] = c.softApAfterMin;

  JsonObject tm = doc.createNestedObject("time");
  tm["ntpEnabled"] = c.ntpEnabled;
  tm["ntpServer"] = c.ntpServer;
  tm["tz"] = c.tz;

  JsonArray probes = doc.createNestedArray("probes");
  for (int i = 0; i < (int)ax::kMaxProbes; i++) {
    JsonObject p = probes.createNestedObject();
    p["id"] = c.probeId[i];
    p["offset"] = c.probeOffset[i];
    p["label"] = c.probeLabel[i];
  }

  JsonObject ctl = doc.createNestedObject("control");
  ctl["usePid"] = c.usePid;
  ctl["predictive"] = c.predictive;
  ctl["tolerance"] = c.toleranceC;
  ctl["kp"] = c.pidKp;
  ctl["ki"] = c.pidKi;
  ctl["kd"] = c.pidKd;
  ctl["windowMs"] = c.pidWindowMs;
  ctl["fullPowerBelow"] = c.pidFullPowerBelow;
  ctl["cutAbove"] = c.pidCutAbove;
  ctl["compressorRestMin"] = c.compressorRestMin;
  ctl["compressorMinOnSec"] = c.compressorMinOnSec;
  ctl["minStateSec"] = c.minStateSec;
  ctl["minSwitchSec"] = c.minSwitchSec;
  ctl["airMinC"] = c.airMinC;
  ctl["airBelowSpC"] = c.airBelowSpC;
  ctl["airAboveSpC"] = c.airAboveSpC;

  JsonObject isp = doc.createNestedObject("ispindel");
  isp["staleMin"] = c.gravityStaleMin;
  // SPECCHIO della camera 0, non fonte di verità: la fonte è chambers[i]. Serve a due
  // cose — un firmware precedente (che legge solo qui) continua a funzionare dopo un
  // ritorno indietro, e una config scritta da questo firmware resta leggibile da quello.
  isp["correction"] = c.gravityCorrection[0];
  isp["batteryAlert"] = c.batteryAlertV;
  isp["stuckAlert"] = c.fermStuckAlert;
  isp["unit"] = c.gravityUnit;
  isp["token"] = mask && c.ispindelToken.length() ? String(kMask) : c.ispindelToken;
  isp["device"] = c.gravityDevice[0];  // specchio della camera 0 — vedi sopra

  JsonObject fan = doc.createNestedObject("fan");
  fan["relay"] = c.fanRel;
  fan["mode"] = c.fanMode;
  fan["postRunSec"] = c.fanPostRunSec;
  fan["cycleOnMin"] = c.fanCycleOnMin;
  fan["cycleEveryMin"] = c.fanCycleEveryMin;

  JsonObject dsp = doc.createNestedObject("display");
  dsp["enabled"] = c.oledEnabled;
  dsp["driver"] = c.oledDriver;
  dsp["address"] = c.oledAddress;
  dsp["flip"] = c.oledFlip;
  dsp["brightness"] = c.oledBrightness;

  JsonObject lg = doc.createNestedObject("log");
  lg["sampleMin"] = c.logSampleMin;
  lg["retentionDays"] = c.logRetentionDays;

  JsonObject mq = doc.createNestedObject("mqtt");
  mq["enabled"] = c.mqttEnabled;
  mq["host"] = c.mqttHost;
  mq["port"] = c.mqttPort;
  mq["user"] = c.mqttUser;
  mq["pass"] = mask ? String(kMask) : c.mqttPass;
  mq["baseTopic"] = c.mqttBaseTopic;
  mq["haDiscovery"] = c.haDiscovery;

  JsonObject ota = doc.createNestedObject("ota");
  ota["enabled"] = c.otaEnabled;

  JsonObject saf = doc.createNestedObject("safety");
  saf["heatMaxOnH"] = c.heatMaxOnH;
  saf["heatStuckMin"] = c.heatStuckMin;
}

static void setSecret(String* dst, JsonVariantConst v) {
  if (v.isNull()) return;
  String s = v.as<String>();
  if (s == kMask) return;
  *dst = s;
}

void ConfigStore::fromJson(AppConfig* c, JsonDocument& doc) const {
  JsonObject sys = doc["system"];
  if (!sys.isNull()) {
    c->hostname = sys["hostname"] | c->hostname;
    setSecret(&c->uiPassword, sys["uiPassword"]);
    c->pwdAuto = sys["pwdAuto"] | c->pwdAuto;
  }
  JsonObject net = doc["network"];
  if (!net.isNull()) {
    c->ssid = net["ssid"] | c->ssid;
    setSecret(&c->wifiPassword, net["password"]);
    c->dhcp = net["dhcp"] | c->dhcp;
    c->ip = net["ip"] | c->ip;
    c->mask = net["mask"] | c->mask;
    c->gw = net["gw"] | c->gw;
    c->dns = net["dns"] | c->dns;
    setSecret(&c->softApPassword, net["softApPassword"]);
    c->softApAfterMin = net["softApAfterMin"] | c->softApAfterMin;
  }
  JsonObject tm = doc["time"];
  if (!tm.isNull()) {
    c->ntpEnabled = tm["ntpEnabled"] | c->ntpEnabled;
    c->ntpServer = tm["ntpServer"] | c->ntpServer;
    c->tz = tm["tz"] | c->tz;
  }
  JsonArray probes = doc["probes"];
  if (!probes.isNull()) {
    // i < probes.size(): una config MONO-camera (3 sonde) si carica senza errori e
    // gli slot in piu' restano non configurati -> nessun falso guasto.
    for (int i = 0; i < (int)ax::kMaxProbes && i < (int)probes.size(); i++) {
      JsonObject p = probes[i];
      c->probeId[i] = p["id"] | c->probeId[i];
      c->probeOffset[i] = p["offset"] | c->probeOffset[i];
      // Un offset senza limiti falserebbe la temperatura di controllo restando
      // "valida": stessa soglia della taratura guidata.
      if (!(c->probeOffset[i] >= -5.0f && c->probeOffset[i] <= 5.0f)) c->probeOffset[i] = 0.0f;
      c->probeLabel[i] = p["label"] | c->probeLabel[i];
      // Un'etichetta senza limite gonfia /api/status fino a far traboccare (in
      // silenzio) il documento JSON della risposta: 24 byte bastano alla UI.
      if (c->probeLabel[i].length() > 24) {
        unsigned cut = 24;
        // non spezzare una sequenza UTF-8: arretra all'inizio del carattere
        while (cut > 0 && ((uint8_t)c->probeLabel[i][cut] & 0xC0) == 0x80) cut--;
        c->probeLabel[i] = c->probeLabel[i].substring(0, cut);
      }
    }
  }
  // Sezione per camera. Assente in una config v1: le etichette restano ai default.
  // chambers[] è la FONTE DI VERITÀ per densimetro e taratura; la sezione ispindel,
  // che questo file scrive come specchio della camera 0, è solo il fallback per le
  // config v1 e per il ritorno indietro a un firmware precedente.
  // Ma ispindel viene letta DOPO (più sotto), quindi senza una guardia esplicita lo
  // specchio sovrascriverebbe la fonte: da qui i due flag.
  // ATTENZIONE al discriminante: chambers[] esiste già dalla 2.24.0 con la sola
  // "label", quindi la sua PRESENZA non dice nulla sul densimetro — va interrogata
  // chiave per chiave.
  bool densimetroDaChambers = false, correzioneDaChambers = false;
  JsonArrayConst chambers = doc["chambers"];
  if (!chambers.isNull())
    for (uint8_t i = 0; i < AX_N_CHAMBERS && i < chambers.size(); i++) {
      const char* lab = chambers[i]["label"];
      if (lab && *lab) c->chamberLabel[i] = lab;
      // containsKey, non l'idioma "| default": qui la stringa VUOTA è un valore
      // legittimo e significativo (disassegna il densimetro). Con "| default"
      // l'utente non potrebbe più togliere uno strumento assegnato per errore.
      if (chambers[i].containsKey("densimetro")) {
        const char* dev = chambers[i]["densimetro"];
        c->gravityDevice[i] = dev ? dev : "";
        if (c->gravityDevice[i].length() > 32)
          c->gravityDevice[i] = c->gravityDevice[i].substring(0, 32);
        if (i == 0) densimetroDaChambers = true;
      }
      if (chambers[i].containsKey("correzione")) {
        c->gravityCorrection[i] = chambers[i]["correzione"] | 0.0f;
        if (i == 0) correzioneDaChambers = true;
        // Stesso test di appartenenza applicato alla camera 0 poco sopra, e per lo
        // stesso motivo (String(float) su buffer fisso): vale per OGNI camera.
        if (!(c->gravityCorrection[i] >= -0.1f && c->gravityCorrection[i] <= 0.1f))
          c->gravityCorrection[i] = 0.0f;
      }
    }
  JsonObject ctl = doc["control"];
  if (!ctl.isNull()) {
    c->usePid = ctl["usePid"] | c->usePid;
    c->predictive = ctl["predictive"] | c->predictive;
    c->toleranceC = ctl["tolerance"] | c->toleranceC;
    c->pidKp = ctl["kp"] | c->pidKp;
    c->pidKi = ctl["ki"] | c->pidKi;
    c->pidKd = ctl["kd"] | c->pidKd;
    c->pidWindowMs = ctl["windowMs"] | c->pidWindowMs;
    c->pidFullPowerBelow = ctl["fullPowerBelow"] | c->pidFullPowerBelow;
    c->pidCutAbove = ctl["cutAbove"] | c->pidCutAbove;
    c->compressorRestMin = ctl["compressorRestMin"] | c->compressorRestMin;
    // Riposo minimo del compressore >= 1 minuto: 0 disattiverebbe la guardia in silenzio e
    // il compressore potrebbe cortocircuitarsi (avvii ripetuti fino a bruciarlo, §0 classe 1).
    if (c->compressorRestMin < 1) c->compressorRestMin = 1;
    // ...e tetto: un riposo enorme (stessa confusione minuti↔secondi) terrebbe il frigo di fatto
    // disabilitato per ore → la birra si scalda senza controllo. 60 min è ampio per qualunque
    // protezione anti-corto-ciclo reale.
    if (c->compressorRestMin > 60) c->compressorRestMin = 60;
    c->compressorMinOnSec = ctl["compressorMinOnSec"] | c->compressorMinOnSec;
    c->minStateSec = ctl["minStateSec"] | c->minStateSec;
    c->minSwitchSec = ctl["minSwitchSec"] | c->minSwitchSec;
    // Tetto ai tempi di temporizzazione (default 60/60/120 s). NB: nemmeno la UI limita questi campi,
    // quindi un valore enorme — confusione minuti↔secondi, uno zero di troppo, o uno script —
    // INCHIODEREBBE un relè per ore: il latch d'isteresi/interlock e la guardia compressore non
    // tornano indietro nemmeno a temperatura ben oltre il setpoint → birra congelata (frigo) o cotta
    // (riscaldatore), e nessun percorso taglia l'uscita (thermalDeviation è solo allarme, dopo 3 h).
    // 1800 s (30 min) è ampio per qualunque anti-chatter/marcia-minima reale (glicole incluso) ma
    // limita l'overshoot da mis-configurazione.
    if (c->compressorMinOnSec > 1800) c->compressorMinOnSec = 1800;
    if (c->minStateSec > 1800) c->minStateSec = 1800;
    if (c->minSwitchSec > 1800) c->minSwitchSec = 1800;
    c->airMinC = ctl["airMinC"] | c->airMinC;
    c->airBelowSpC = ctl["airBelowSpC"] | c->airBelowSpC;
    c->airAboveSpC = ctl["airAboveSpC"] | c->airAboveSpC;
    // Limiti plausibili sui parametri di regolazione. Un refuso (tolleranza 5 invece di
    // 0,5, un segno meno, i millisecondi scritti in secondi) degraderebbe la regolazione e i
    // profili in silenzio: la tolleranza decide anche quando una rampa e' arrivata. Il valore
    // fuori limite viene riportato nel limite, e la risposta del PUT dice quali campi.
    auto clampF = [](float& v, float lo, float hi) { if (!(v >= lo)) v = lo; if (v > hi) v = hi; };
    clampF(c->toleranceC, 0.1f, 3.0f);
    clampF(c->pidKp, 0.0f, 50.0f);
    clampF(c->pidKi, 0.0f, 5.0f);
    clampF(c->pidKd, 0.0f, 100.0f);
    if (c->pidWindowMs < 2000) c->pidWindowMs = 2000;
    if (c->pidWindowMs > 600000) c->pidWindowMs = 600000;
    // fullPowerBelow sotto 0,5 terrebbe il caldo al 100% fino al setpoint o oltre.
    clampF(c->pidFullPowerBelow, 0.5f, 20.0f);
    clampF(c->pidCutAbove, 0.0f, 5.0f);
    clampF(c->airMinC, -10.0f, 10.0f);
    clampF(c->airBelowSpC, 0.0f, 30.0f);
    clampF(c->airAboveSpC, 0.0f, 30.0f);
  }
  JsonObject isp = doc["ispindel"];
  if (!isp.isNull()) {
    c->gravityStaleMin = isp["staleMin"] | c->gravityStaleMin;
    // 0 (campo svuotato dalla UI) → finestra di staleness 0 → densità SEMPRE stantia → alarmGravity
    // permanente e step a densità bloccati. Minimo 1 minuto.
    if (c->gravityStaleMin < 1) c->gravityStaleMin = 1;
    // Formato v1 (mono-camera) e specchio del formato attuale: alimenta la CAMERA 0,
    // ma SOLO se chambers[] non ha già parlato — vedi i due flag più sopra.
    if (!correzioneDaChambers)
      c->gravityCorrection[0] = isp["correction"] | c->gravityCorrection[0];
    // Stessa difesa gia' applicata a probeOffset poche righe sopra, e per lo stesso
    // motivo: il valore finisce TESTUALMENTE nelle risposte via String(float, 4), che sul
    // core ESP8266 usa un char buf[33] e dtostrf SENZA controllo di lunghezza (WString.cpp
    // :108). dtostrf gestisce NaN e inf, ma un FINITO grande no: da ~1e27 in su scrive
    // oltre il fondo del buffer di stack, e vicino a FLT_MAX sono ~45 byte in 33.
    // Aggravante: il valore e' persistito, e sommato alla densita' puo' finire nell'OG
    // catturata in /state.json, riproducendosi a ogni /api/status.
    // Test di APPARTENENZA, non due confronti: cosi' rifiuta anche NaN.
    // 0,1 e' larghissimo per una correzione di densita' (SG o gradi Plato).
    if (!(c->gravityCorrection[0] >= -0.1f && c->gravityCorrection[0] <= 0.1f))
      c->gravityCorrection[0] = 0.0f;
    c->batteryAlertV = isp["batteryAlert"] | c->batteryAlertV;
    c->fermStuckAlert = isp["stuckAlert"] | c->fermStuckAlert;
    c->gravityUnit = isp["unit"] | c->gravityUnit;
    // Whitelist: ogni consumatore confronta == "plato" in minuscolo, quindi una variante
    // ("Plato", "°P", refuso) selezionerebbe in silenzio il ramo SG. Fuori whitelist -> "sg".
    if (c->gravityUnit != "sg" && c->gravityUnit != "plato") c->gravityUnit = "sg";
    setSecret(&c->ispindelToken, isp["token"]);
    if (!densimetroDaChambers) {
      c->gravityDevice[0] = isp["device"] | c->gravityDevice[0];
      if (c->gravityDevice[0].length() > 32)
        c->gravityDevice[0] = c->gravityDevice[0].substring(0, 32);
    }
  }
  JsonObject fan = doc["fan"];
  if (!fan.isNull()) {
    c->fanRel = fan["relay"] | c->fanRel;
    if (c->fanRel > 8) c->fanRel = 0;
    c->fanMode = fan["mode"] | c->fanMode;
    if (c->fanMode > 1) c->fanMode = 0;
    c->fanPostRunSec = fan["postRunSec"] | c->fanPostRunSec;
    if (c->fanPostRunSec > 1800) c->fanPostRunSec = 1800;
    c->fanCycleOnMin = fan["cycleOnMin"] | c->fanCycleOnMin;
    c->fanCycleEveryMin = fan["cycleEveryMin"] | c->fanCycleEveryMin;
    if (c->fanCycleEveryMin > 1440) c->fanCycleEveryMin = 1440;
    // Ricircolo sensato solo con una pausa: «5 ogni 5» sarebbe «sempre accesa».
    if (c->fanCycleOnMin >= c->fanCycleEveryMin) c->fanCycleOnMin = c->fanCycleEveryMin = 0;
  }
  JsonObject dsp = doc["display"];
  if (!dsp.isNull()) {
    c->oledEnabled = dsp["enabled"] | c->oledEnabled;
    c->oledDriver = dsp["driver"] | c->oledDriver;
    c->oledAddress = dsp["address"] | c->oledAddress;
    c->oledFlip = dsp["flip"] | c->oledFlip;
    c->oledBrightness = dsp["brightness"] | c->oledBrightness;
    if (c->oledBrightness < 5) c->oledBrightness = 5;  // mai uno schermo "spento" per sbaglio
  }
  JsonObject lg = doc["log"];
  if (!lg.isNull()) {
    c->logSampleMin = lg["sampleMin"] | c->logSampleMin;
    if (c->logSampleMin < 1) c->logSampleMin = 1;
    c->logRetentionDays = lg["retentionDays"] | c->logRetentionDays;
    if (c->logRetentionDays < 1) c->logRetentionDays = 1;
    // Tetto: senza, il registro può riempire la LittleFS (~2 MB) e far fallire ogni
    // scrittura successiva (config/stato/log). 90 giorni restano sotto il limite anche
    // a campionamento fitto; l'iterazione dell'export è comunque limitata da questo.
    if (c->logRetentionDays > 90) c->logRetentionDays = 90;
  }
  JsonObject mq = doc["mqtt"];
  if (!mq.isNull()) {
    c->mqttEnabled = mq["enabled"] | c->mqttEnabled;
    c->mqttHost = mq["host"] | c->mqttHost;
    c->mqttPort = mq["port"] | c->mqttPort;
    c->mqttUser = mq["user"] | c->mqttUser;
    setSecret(&c->mqttPass, mq["pass"]);
    c->mqttBaseTopic = mq["baseTopic"] | c->mqttBaseTopic;
    c->haDiscovery = mq["haDiscovery"] | c->haDiscovery;
  }
  JsonObject ota = doc["ota"];
  if (!ota.isNull()) {
    c->otaEnabled = ota["enabled"] | c->otaEnabled;
  }
  JsonObject saf = doc["safety"];
  if (!saf.isNull()) {
    c->heatMaxOnH = saf["heatMaxOnH"] | c->heatMaxOnH;
    c->heatStuckMin = saf["heatStuckMin"] | c->heatStuckMin;
    // Il tempo massimo NON si disattiva: e' la protezione che non si fida della sonda.
    // Sotto le 2 ore scatterebbe su un riscaldamento lento ma legittimo (20 L con un
    // tappetino da 25 W salgono di ~1 °C l'ora).
    if (c->heatMaxOnH < 2) c->heatMaxOnH = 2;
    if (c->heatMaxOnH > 48) c->heatMaxOnH = 48;
    // Sonda ferma: 0 = spento (riscaldatore debole su volume grande: falsi blocchi).
    if (c->heatStuckMin && c->heatStuckMin < 15) c->heatStuckMin = 15;
    if (c->heatStuckMin > 240) c->heatStuckMin = 240;
  }
  // Tetti di lunghezza sui campi liberi: senza, stringhe lunghe accumulate su più PUT
  // possono far traboccare il documento (sezioni finali perse in silenzio) e impedire il
  // ricaricamento al boot. Limiti generosi per l'uso reale.
  auto cap = [](String& s, unsigned n) { if (s.length() > n) s = s.substring(0, n); };
  cap(c->hostname, 32); cap(c->ssid, 32); cap(c->wifiPassword, 64);
  cap(c->softApPassword, 63); cap(c->uiPassword, 64);
  cap(c->ntpServer, 48); cap(c->tz, 48);
  cap(c->mqttHost, 48); cap(c->mqttUser, 32); cap(c->mqttPass, 64); cap(c->mqttBaseTopic, 32);
  cap(c->ip, 20); cap(c->mask, 20); cap(c->gw, 20); cap(c->dns, 20);
  // Anche l'etichetta di camera: era l'unica stringa libera della configurazione senza
  // tetto. Finisce in /config.json (documento da 6144 su ~17 KB di heap), in /api/status
  // e nei nomi delle entita' Home Assistant: senza limite un nome lunghissimo poteva far
  // traboccare quei documenti.
  // NB: non si usa cap(), che taglia per BYTE. L'etichetta e' testo libero e puo'
  // contenere accenti: spezzare una sequenza UTF-8 a meta' produrrebbe un carattere
  // invalido dentro /config.json e /api/status. Si arretra all'inizio del carattere,
  // come gia' fa probeLabel poco sopra.
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
    if (c->chamberLabel[i].length() <= 24) continue;
    unsigned cut = 24;
    while (cut > 0 && ((uint8_t)c->chamberLabel[i][cut] & 0xC0) == 0x80) cut--;
    c->chamberLabel[i] = c->chamberLabel[i].substring(0, cut);
  }
}

String ConfigStore::correctedFields(JsonDocument& doc, const AppConfig& c) {
  struct Campo { const char* sez; const char* chiave; float val; };
  const Campo campi[] = {
      {"control", "tolerance", c.toleranceC},        {"control", "kp", c.pidKp},
      {"control", "ki", c.pidKi},                    {"control", "kd", c.pidKd},
      {"control", "windowMs", (float)c.pidWindowMs},
      {"control", "fullPowerBelow", c.pidFullPowerBelow},
      {"control", "cutAbove", c.pidCutAbove},
      {"control", "compressorRestMin", (float)c.compressorRestMin},
      {"control", "compressorMinOnSec", (float)c.compressorMinOnSec},
      {"control", "minStateSec", (float)c.minStateSec},
      {"control", "minSwitchSec", (float)c.minSwitchSec},
      {"control", "airMinC", c.airMinC},             {"control", "airBelowSpC", c.airBelowSpC},
      {"control", "airAboveSpC", c.airAboveSpC},
      {"safety", "heatMaxOnH", (float)c.heatMaxOnH},
      {"safety", "heatStuckMin", (float)c.heatStuckMin},
  };
  String out;
  for (const Campo& k : campi) {
    JsonVariantConst v = doc[k.sez][k.chiave];
    if (v.isNull()) continue;
    const float inviato = v.as<float>();
    if (inviato > k.val + 0.0005f || inviato < k.val - 0.0005f) {
      if (out.length()) out += ',';
      out += '"';
      out += k.chiave;
      out += '"';
    }
  }
  return out;
}
