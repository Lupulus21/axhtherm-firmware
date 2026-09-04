#include "svc/CalibStore.h"
#include <LittleFS.h>
#include "svc/FsCommit.h"

bool CalibStore::load() {
  n_ = 0;
  File f = LittleFS.open(kPath, "r");
  if (!f) return false;
  DynamicJsonDocument doc(3072);  // 12 punti; nomi densimetro fino a 32 byte
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  JsonArray arr = doc["points"];
  if (arr.isNull()) return false;
  for (JsonObject o : arr) {
    if (n_ >= kMaxPoints) break;
    CalibPoint p;
    p.angle = o["a"] | 0.0f;
    p.gravity = o["g"] | 0.0f;
    p.temp = o["t"] | 0.0f;
    p.epoch = o["e"] | 0;
    p.readEpoch = o["r"] | 0;
    p.plato = o["p"] | false;
    p.deviceId = o["d"] | 0;
    strlcpy(p.deviceName, o["n"] | "", sizeof(p.deviceName));
    pts_[n_++] = p;
  }
  return true;
}

bool CalibStore::save() const {
  DynamicJsonDocument doc(3072);
  JsonArray arr = doc.createNestedArray("points");
  for (uint8_t i = 0; i < n_; i++) {
    JsonObject o = arr.createNestedObject();
    o["a"] = pts_[i].angle;
    o["g"] = pts_[i].gravity;
    o["t"] = pts_[i].temp;
    o["e"] = pts_[i].epoch;
    o["r"] = pts_[i].readEpoch;
    o["p"] = pts_[i].plato;
    o["d"] = pts_[i].deviceId;
    o["n"] = pts_[i].deviceName;
  }
  if (doc.overflowed()) return false;  // doc troppo piccolo (nomi lunghi): mai scrivere troncato
  File f = LittleFS.open("/calib.tmp", "w");
  if (!f) return false;
  const size_t want = measureJson(doc);
  const size_t written = serializeJson(doc, f);
  f.close();
  // Con filesystem pieno serializeJson scrive solo i primi chunk: promuovere un
  // file troncato distruggerebbe i punti buoni (al riavvio load() fallirebbe).
  if (written != want) {
    LittleFS.remove("/calib.tmp");
    return false;
  }
  return fsCommitAtomic("/calib.tmp", kPath, want);  // rename atomico + verifica del flush
}
