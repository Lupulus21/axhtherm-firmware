#include "svc/ProfileStore.h"
#include <LittleFS.h>
#include "svc/FsCommit.h"

using ax::StepDef;
using ax::StepType;

static const char* typeName(StepType t) {
  switch (t) {
    case StepType::Fixed: return "fixed";
    case StepType::RampTime: return "ramp_time";
    case StepType::RampRate: return "ramp_rate";
    case StepType::Gravity: return "gravity";
  }
  return "fixed";
}

static StepType typeFromName(const String& s) {
  if (s == "ramp_time") return StepType::RampTime;
  if (s == "ramp_rate") return StepType::RampRate;
  if (s == "gravity") return StepType::Gravity;
  return StepType::Fixed;
}

static String libPath(uint8_t id) { return String("/profiles/p") + id + ".json"; }

// Un singolo step -> oggetto JSON (14 campi). Usato sia dalla serializzazione in blocco
// (stepsToJson, per la GET) sia da quella in streaming (writeFile): un solo punto in cui
// definire i campi, così i due percorsi non possono divergere.
static void stepToJson(const StepDef& s, JsonObject o) {
  o["type"] = typeName(s.type);
  o["enabled"] = s.enabled;
  o["target"] = s.targetC;
  o["fromCurrent"] = s.fromCurrent;
  o["start"] = s.startC;
  o["durationSec"] = s.durationSec;
  o["degPerHour"] = s.degPerHour;
  o["gravityTarget"] = s.gravityTarget;
  o["gravityMode"] = (uint8_t)s.gravityMode;
  o["maxDurationSec"] = s.maxDurationSec;
  o["stableBand"] = s.stableBand;
  o["stableHours"] = s.stableHours;
  o["allowHeat"] = s.allowHeat;
  o["allowCool"] = s.allowCool;
}

void ProfileStore::stepsToJson(const StepDef* steps, uint8_t count, JsonDocument& doc) {
  JsonArray arr = doc.createNestedArray("steps");
  for (uint8_t i = 0; i < count; i++) stepToJson(steps[i], arr.createNestedObject());
}

uint8_t ProfileStore::stepsFromJson(JsonDocument& doc, StepDef* steps) {
  JsonArray arr = doc["steps"];
  if (arr.isNull()) return 0;
  uint8_t n = 0;
  for (JsonObject o : arr) {
    if (n >= ax::kMaxSteps) break;
    StepDef s;
    s.type = typeFromName(o["type"] | String("fixed"));
    s.enabled = o["enabled"] | false;
    s.targetC = o["target"] | 18.0f;
    s.fromCurrent = o["fromCurrent"] | true;
    s.startC = o["start"] | 18.0f;
    s.durationSec = o["durationSec"] | 0;
    s.degPerHour = o["degPerHour"] | 0.5f;
    s.gravityTarget = o["gravityTarget"] | 1.010f;
    // Campi assenti nei profili pre-v2.12: default identici al vecchio comportamento.
    uint8_t gm = o["gravityMode"] | (uint8_t)0;
    s.gravityMode = gm > 2 ? ax::GravityMode::Below : (ax::GravityMode)gm;
    s.maxDurationSec = o["maxDurationSec"] | 0;
    s.stableBand = o["stableBand"] | 0.002f;
    s.stableHours = o["stableHours"] | 48.0f;
    s.allowHeat = o["allowHeat"] | true;
    s.allowCool = o["allowCool"] | true;
    steps[n++] = s;
  }
  return n;
}

bool ProfileStore::readFile(const String& path, String* name, StepDef steps[],
                            uint8_t* count) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  // NON reso in streaming, DELIBERATAMENTE. La SCRITTURA lo e' (writeFile, sotto: picco
  // < 1 KB invece di 6), perche' li' il difetto era reale e osservato in campo: il doc di
  // parsing dell'handler HTTP restava vivo INSIEME a questo, due allocazioni da 6 KB
  // contemporanee su ~17 KB di heap frammentato -> profili salvati come "{}".
  // In LETTURA la coesistenza non c'e': verificato che ogni chiamante libera questo doc
  // prima di allocare il proprio (WebApi.cpp handleProfileById: load() ritorna, poi
  // alloca la risposta), e al boot le camere si leggono in sequenza. Il picco resta di UN
  // solo documento, com'era nella v2.23.2.
  // ArduinoJson 6 non offre un parser incrementale: uno streaming vero richiederebbe un
  // parser scritto a mano, e ridurre la capacita' a occhio rischierebbe di rendere
  // ILLEGGIBILI profili validi (deserializeJson fallisce con NoMemory). Va fatto misurando
  // il caso peggiore reale a 12 step sul dispositivo, non a stima.
  DynamicJsonDocument doc(kProfileDoc);  // derivata dalla struttura: vedi ProfileStore.h
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  *name = doc["name"] | String("");
  *count = stepsFromJson(doc, steps);
  return true;
}

bool ProfileStore::writeFile(const String& path, const String& tmpPath, const String& name,
                             const StepDef* steps, uint8_t count) {
  // Serializzazione IN STREAMING, uno step alla volta. Costruire l'intero profilo in un
  // DynamicJsonDocument(6144) e poi serializzarlo raddoppiava il picco di heap: il gestore
  // HTTP tiene ancora vivo il suo doc di parsing quando questa funzione ne allocava
  // un secondo, e su un heap ESP8266 frammentato (~17 KB liberi) la seconda allocazione
  // poteva fallire. Nel firmware pre-guardia ciò produceva silenziosamente un profilo
  // "{}" (nome e step persi -> "Profilo N" in libreria); con la guardia diventava
  // "salvataggio fallito". Qui il picco è di un solo piccolo doc (un oggetto step) alla
  // volta, quindi il salvataggio non dipende più dalla disponibilità di un blocco da 6 KB.
  File f = LittleFS.open(tmpPath, "w");
  if (!f) return false;

  size_t want = 0, got = 0;  // byte attesi vs scritti: se divergono la scrittura è troncata
  auto emit = [&](const char* lit) {
    size_t l = strlen(lit);
    want += l;
    got += f.print(lit);
  };
  bool ok = true;

  emit("{\"name\":");
  {
    // Un doc minimo solo per l'escaping JSON corretto del nome (virgolette, backslash,
    // accentate/°): scriverlo a mano rischierebbe JSON non valido.
    DynamicJsonDocument nd(256);  // nome <= 32 byte, ampiamente sufficiente
    nd.set(name);
    if (nd.overflowed()) ok = false;
    want += measureJson(nd);
    got += serializeJson(nd, f);
  }
  emit(",\"steps\":[");
  for (uint8_t i = 0; ok && i < count; i++) {
    if (i) emit(",");
    DynamicJsonDocument sd(JSON_OBJECT_SIZE(14) + 64);  // un solo step (14 campi)
    JsonObject o = sd.to<JsonObject>();
    stepToJson(steps[i], o);
    if (sd.overflowed()) {
      ok = false;
      break;
    }
    want += measureJson(sd);
    got += serializeJson(sd, f);
  }
  emit("]}");
  f.close();

  // Tmp troncato (FS pieno: got<want) o step troppo grande (ok=false): mai promuoverlo.
  // Promuovere un file troncato distruggerebbe il profilo buono; al boot loadActive
  // fallirebbe -> engine senza step -> mode Off -> cotta senza controllo, in silenzio.
  if (!ok || got != want) {
    LittleFS.remove(tmpPath);
    return false;
  }
  return fsCommitAtomic(tmpPath.c_str(), path.c_str(), want);  // rename atomico + verifica del flush
}

bool ProfileStore::loadActive(uint8_t ch, String* name, StepDef steps[], uint8_t* count) {
  return readFile(activePath(ch), name, steps, count);
}

bool ProfileStore::saveActive(uint8_t ch, const String& name, const StepDef* steps, uint8_t count) {
  const String path = activePath(ch);
  return writeFile(path, path.substring(0, path.length() - 5) + ".tmp", name, steps, count);
}

bool ProfileStore::load(uint8_t id, String* name, StepDef steps[], uint8_t* count) {
  if (id < 1 || id > kMaxProfiles) return false;
  return readFile(libPath(id), name, steps, count);
}

bool ProfileStore::save(uint8_t id, const String& name, const StepDef* steps,
                        uint8_t count) {
  if (id < 1 || id > kMaxProfiles) return false;
  if (!LittleFS.exists("/profiles")) LittleFS.mkdir("/profiles");
  return writeFile(libPath(id), "/profiles/tmp", name, steps, count);
}

bool ProfileStore::remove(uint8_t id) {
  if (id < 1 || id > kMaxProfiles) return false;
  return LittleFS.remove(libPath(id));
}

bool ProfileStore::exists(uint8_t id) const {
  if (id < 1 || id > kMaxProfiles) return false;
  return LittleFS.exists(libPath(id));
}

void ProfileStore::list(JsonArray& arr) {
  StepDef steps[ax::kMaxSteps];
  uint8_t n = 0;
  String name;
  for (uint8_t id = 1; id <= kMaxProfiles; id++) {
    if (!readFile(libPath(id), &name, steps, &n)) {
      // readFile ritorna false sia per "assente" sia per OOM/parse-error. Se il file
      // ESISTE ma non si è letto, NON va omesso: il client lo crederebbe un id libero e
      // lo sovrascriverebbe. Lo si elenca con un marcatore d'errore.
      if (LittleFS.exists(libPath(id))) {
        JsonObject e = arr.createNestedObject();
        e["id"] = id;
        e["name"] = String("(illeggibile)");
        e["error"] = true;
      }
      continue;
    }
    JsonObject o = arr.createNestedObject();
    o["id"] = id;
    o["name"] = name.length() ? name : String("Profilo ") + id;
    uint8_t en = 0;
    uint32_t dur = 0;
    bool grav = false;
    for (uint8_t i = 0; i < n; i++) {
      if (!steps[i].enabled) continue;
      en++;
      if (steps[i].type == StepType::Fixed || steps[i].type == StepType::RampTime)
        dur += steps[i].durationSec;
      else if (steps[i].type == StepType::Gravity)
        grav = true;
    }
    o["steps"] = en;
    o["durationSec"] = dur;
    o["gravity"] = grav;
    yield();
  }
}
