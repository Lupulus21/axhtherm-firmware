#include "svc/StateStore.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "svc/FsCommit.h"

// Dimensione del documento JSON. Cresce con le camere ma NON con un unico blocco
// monolitico: ogni camera è un oggetto dell'array "chambers". A N=1 il file è
// leggermente più grande della v1 (la nidificazione costa), ma resta ampiamente
// dentro il budget. La guardia overflowed() qui sotto è la rete di sicurezza:
// mai promuovere uno stato troncato.
static size_t stateDocSize() { return 320u + (size_t)AX_N_CHAMBERS * 1024u; }

// --- serializzazione di UNA camera -------------------------------------------------

static void chamberToJson(const ChamberState& c, JsonObject o) {
  o["mode"] = (uint8_t)c.engine.mode;
  o["stepIdx"] = c.engine.stepIdx;
  o["paused"] = c.engine.paused;
  o["completed"] = c.engine.completed;
  o["stepStartEpoch"] = c.engine.stepStartEpoch;
  o["stepStartTemp"] = c.engine.stepStartTemp;
  o["stepStartTempValid"] = c.engine.stepStartTempValid;
  o["profileStartEpoch"] = c.engine.profileStartEpoch;
  o["delayedStartEpoch"] = c.engine.delayedStartEpoch;
  o["pausedAtEpoch"] = c.engine.pausedAtEpoch;
  o["thermostatSetpoint"] = c.engine.thermostatSetpoint;
  o["overrideActive"] = c.engine.overrideActive;
  o["overrideSetpoint"] = c.engine.overrideSetpoint;
  o["stableRefGravity"] = c.engine.stableRefGravity;
  o["stableSinceEpoch"] = c.engine.stableSinceEpoch;
  o["rampReanchored"] = c.engine.rampReanchored;
  o["coolLastOn"] = c.guard.lastOnEpoch;
  o["coolLastOff"] = c.guard.lastOffEpoch;
  o["coolWasOn"] = c.coolWasOn;
  o["armed"] = c.armed;
  o["coolRel"] = c.coolRel;
  o["heatRel"] = c.heatRel;
  o["batchName"] = c.batchName;
  o["ogGravity"] = c.ogGravity;
  o["coolEstCph"] = c.coolEstCph;
  o["heatEstCph"] = c.heatEstCph;
}

// Legge una camera da un oggetto JSON. Funziona sia su un elemento di "chambers" (v2)
// sia sulla RADICE del documento (v1): le chiavi per camera hanno lo stesso nome, ed è
// esattamente ciò che rende la migrazione una riga sola invece di un percorso separato.
static void chamberFromJson(ChamberState* c, JsonObjectConst o) {
  c->engine.mode = (ax::RunMode)(uint8_t)(o["mode"] | 0);
  c->engine.stepIdx = o["stepIdx"] | 0;
  c->engine.paused = o["paused"] | false;
  c->engine.completed = o["completed"] | false;
  c->engine.stepStartEpoch = o["stepStartEpoch"] | 0;
  c->engine.stepStartTemp = o["stepStartTemp"] | 0.0f;
  c->engine.stepStartTempValid = o["stepStartTempValid"] | false;
  c->engine.profileStartEpoch = o["profileStartEpoch"] | 0;
  c->engine.delayedStartEpoch = o["delayedStartEpoch"] | 0;
  c->engine.pausedAtEpoch = o["pausedAtEpoch"] | 0;
  c->engine.thermostatSetpoint = o["thermostatSetpoint"] | 18.0f;
  c->engine.overrideActive = o["overrideActive"] | false;
  c->engine.overrideSetpoint = o["overrideSetpoint"] | 0.0f;
  c->engine.stableRefGravity = o["stableRefGravity"] | 0.0f;
  c->engine.stableSinceEpoch = o["stableSinceEpoch"] | 0;
  c->engine.rampReanchored = o["rampReanchored"] | false;
  c->guard.lastOnEpoch = o["coolLastOn"] | 0;
  c->guard.lastOffEpoch = o["coolLastOff"] | 0;
  c->coolWasOn = o["coolWasOn"] | false;
  c->armed = o["armed"] | false;
  c->coolRel = o["coolRel"] | 0;
  c->heatRel = o["heatRel"] | 0;
  c->batchName = o["batchName"] | String("");
  c->ogGravity = o["ogGravity"] | 0.0f;
  c->coolEstCph = o["coolEstCph"] | 0.0f;
  c->heatEstCph = o["heatEstCph"] | 0.0f;
}

// --- load / save -------------------------------------------------------------------

bool StateStore::load(PersistState* st) {
  migratedV1_ = false;
  chambersRead_ = 0;
  excessChambers_ = 0;
  File f = LittleFS.open(kPath, "r");
  if (!f) return false;
  // Su heap: lo stack "cont" dell'ESP8266 è 4 KB e questa save/load può girare in
  // fondo alla catena dei handler web (causa accertata di reset da stack overflow).
  // Capacita' di LETTURA ricavata dalla DIMENSIONE DEL FILE, non da AX_N_CHAMBERS.
  //
  // Dimensionarla sul firmware era un difetto reale e misurato: deserializeJson
  // materializza l'INTERO documento nel pool prima che il ciclo qui sotto possa
  // limitarsi ad AX_N_CHAMBERS, quindi un file scritto con piu' camere falliva con
  // NoMemory e load() ritornava false. Il chiamante non poteva distinguerlo da uno
  // /state.json corrotto: tutte le camere ripartivano Off/disarmate, perdendo step, OG,
  // epoch della guardia compressore e coefficienti appresi.
  // Peggio: rendeva IRRAGGIUNGIBILE la gestione esplicita di excessChambers_ poche righe
  // sotto, che esiste proprio per quel caso.
  // La soglia dipendeva dalla LUNGHEZZA DEL NOME DELLA COTTA (il pool cresce di 1 B per
  // carattere): a N=1 la capacita' nominale e' 1344 B e un file a 2 camere ne costa 1293
  // + i due batchName, quindi rompeva oltre ~51 caratteri complessivi. Un guasto che
  // dipende da come hai chiamato la birra e' anche pessimo da diagnosticare.
  //
  // ~1,5x la dimensione del file copre il rapporto pool/testo di questo schema (slot da
  // 16 B con deduplica delle chiavi); mai meno del nominale, e con un tetto duro perche'
  // un file assurdo non deve poter chiedere tutto l'heap.
  size_t cap = f.size() + f.size() / 2 + 256;
  if (cap < stateDocSize()) cap = stateDocSize();
  if (cap > 6144) cap = 6144;
  DynamicJsonDocument doc(cap);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  st->lastSeenEpoch = doc["lastSeenEpoch"] | 0;
  st->epochsTrusted = doc["epochsTrusted"] | false;
  st->failsafePending = doc["failsafePending"] | false;

  JsonArrayConst chambers = doc["chambers"];
  if (chambers.isNull()) {
    // SCHEMA v1 (fino alla v2.23.2): nessun campo "v", nessun array "chambers", tutte
    // le chiavi per camera in radice. Il file descrive UNA camera: è la camera 0.
    // Le altre restano ai default (non armate, motore Off): corretto, non esistevano.
    chamberFromJson(&st->ch[0], doc.as<JsonObjectConst>());
    migratedV1_ = true;
    chambersRead_ = 1;
    return true;
  }
  // SCHEMA v2. Si leggono al più AX_N_CHAMBERS camere: un file scritto con più camere
  // di quante ne gestisce questo firmware NON deve scrivere fuori dall'array.
  for (uint8_t i = 0; i < AX_N_CHAMBERS && i < chambers.size(); i++) {
    chamberFromJson(&st->ch[i], chambers[i].as<JsonObjectConst>());
    chambersRead_ = (uint8_t)(i + 1);
  }
  // Il file descrive PIU' camere di quante ne gestisce questo firmware: succede
  // riportando indietro il firmware dopo aver girato con piu' camere. Lo stato in
  // eccesso viene perso, e perderlo in SILENZIO sarebbe la cosa peggiore.
  excessChambers_ = chambers.size() > AX_N_CHAMBERS
                        ? (uint8_t)(chambers.size() - AX_N_CHAMBERS) : 0;
  return true;
}

bool StateStore::save(const PersistState& st) {
  DynamicJsonDocument doc(stateDocSize());  // heap, non stack: vedi load()
  doc["v"] = PersistState::kSchemaVersion;
  doc["lastSeenEpoch"] = st.lastSeenEpoch;
  doc["epochsTrusted"] = st.epochsTrusted;
  doc["failsafePending"] = st.failsafePending;
  JsonArray chambers = doc.createNestedArray("chambers");
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++)
    chamberToJson(st.ch[i], chambers.createNestedObject());

  String out;
  serializeJson(doc, out);
  if (doc.overflowed()) return false;  // doc troppo piccolo: mai promuovere uno stato troncato

  // NB: qui la v2.23.2 confrontava con una copia dell'ultimo salvataggio per saltare le
  // scritture inutili. Quel confronto non poteva riuscire MAI: lastSeenEpoch viene
  // riassegnato a ogni persistNow (main.cpp), quindi la stringa differiva sempre. Era
  // quindi solo una copia permanente dell'intero stato tenuta sullo heap — e con più
  // camere sarebbe raddoppiata. Rimossa: stesso identico comportamento di scrittura,
  // meno heap occupato. (Residuo noto documentato nella campagna di revisione.)
  File f = LittleFS.open("/state.tmp", "w");
  if (!f) return false;
  size_t n = f.print(out);
  f.close();
  if (n != out.length()) { LittleFS.remove("/state.tmp"); return false; }  // no tmp troncato in giro
  // fsCommitAtomic ri-verifica la dimensione COMMITTATA su flash (f.print bufferizza e close()
  // scarta l'esito del flush) e poi rinomina atomicamente: nessuna finestra in cui /state.json
  // non esiste, e nessuna promozione di un tmp troncato dall'ultimo flush fallito.
  return fsCommitAtomic("/state.tmp", kPath, out.length());
}
