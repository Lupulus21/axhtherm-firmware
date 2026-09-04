#include "svc/DeviceRegistry.h"
#include <LittleFS.h>
#include "svc/FsCommit.h"

bool DeviceRegistry::load() {
  n_ = 0;
  File f = LittleFS.open(kPath, "r");
  if (!f) return false;
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  JsonArray arr = doc["devices"];
  if (arr.isNull()) return false;
  for (JsonObject o : arr) {
    if (n_ >= kMax) break;
    KnownDevice d;
    strlcpy(d.name, o["n"] | "", sizeof(d.name));
    d.id = o["i"] | 0;
    d.lastSeen = o["t"] | 0;
    devs_[n_++] = d;
  }
  return true;
}

bool DeviceRegistry::save() const {
  DynamicJsonDocument doc(512);
  JsonArray arr = doc.createNestedArray("devices");
  for (uint8_t i = 0; i < n_; i++) {
    JsonObject o = arr.createNestedObject();
    o["n"] = devs_[i].name;
    o["i"] = devs_[i].id;
    o["t"] = devs_[i].lastSeen;
  }
  if (doc.overflowed()) return false;  // mai promuovere un doc troncato (uniforme agli altri store)
  File f = LittleFS.open("/devices.tmp", "w");
  if (!f) return false;
  const size_t want = measureJson(doc);
  const size_t written = serializeJson(doc, f);
  f.close();
  if (written != want) {
    LittleFS.remove("/devices.tmp");
    return false;
  }
  return fsCommitAtomic("/devices.tmp", kPath, want);  // rename atomico + verifica del flush
}
