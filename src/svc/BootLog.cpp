#include "svc/BootLog.h"
#include <LittleFS.h>
#include "svc/FsCommit.h"

bool BootLog::load() {
  n_ = 0;
  File f = LittleFS.open(kPath, "r");
  if (!f) return false;
  DynamicJsonDocument doc(1536);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  JsonArray arr = doc["boots"];
  if (arr.isNull()) return false;
  for (JsonObject o : arr) {
    if (n_ >= kMax) break;
    BootEntry b;
    b.epoch = o["t"] | 0;
    b.upSec = o["u"] | 0;
    b.intent = o["i"] | 0;
    b.exc = o["x"] | 0;
    b.epc = o["e"] | 0;
    strlcpy(b.reason, o["r"] | "", sizeof(b.reason));
    e_[n_++] = b;
  }
  total_ = doc["n"] | (uint32_t)n_;  // file precedente senza contatore: si parte dalle righe
  return true;
}

bool BootLog::record(const String& reason, uint32_t epoch, uint32_t prevUpSec, uint8_t intent,
                     uint8_t exc, uint32_t epc) {
  BootEntry b;
  b.epoch = epoch;
  b.upSec = prevUpSec;
  b.intent = intent;
  b.exc = exc;
  b.epc = epc;
  total_++;
  strlcpy(b.reason, reason.c_str(), sizeof(b.reason));
  if (n_ >= kMax) {  // scorre: si tengono gli otto più recenti
    for (uint8_t i = 1; i < kMax; i++) e_[i - 1] = e_[i];
    n_ = kMax - 1;
  }
  e_[n_++] = b;
  return save();
}

bool BootLog::stampEpoch(uint32_t epoch) {
  if (!n_ || e_[n_ - 1].epoch != 0 || epoch == 0) return false;
  e_[n_ - 1].epoch = epoch;
  return save();
}

bool BootLog::save() const {
  DynamicJsonDocument doc(1536);
  doc["n"] = total_;
  JsonArray arr = doc.createNestedArray("boots");
  for (uint8_t i = 0; i < n_; i++) {
    JsonObject o = arr.createNestedObject();
    o["t"] = e_[i].epoch;
    o["u"] = e_[i].upSec;
    o["r"] = e_[i].reason;
    if (e_[i].intent) o["i"] = e_[i].intent;
    if (e_[i].exc || e_[i].epc) {
      o["x"] = e_[i].exc;
      o["e"] = e_[i].epc;
    }
  }
  if (doc.overflowed()) return false;  // mai promuovere un doc troncato (uniforme agli altri store)
  File f = LittleFS.open("/boot.tmp", "w");
  if (!f) return false;
  const size_t want = measureJson(doc);
  const size_t written = serializeJson(doc, f);
  f.close();
  if (written != want) {
    LittleFS.remove("/boot.tmp");
    return false;
  }
  return fsCommitAtomic("/boot.tmp", kPath, want);  // rename atomico + verifica del flush
}
