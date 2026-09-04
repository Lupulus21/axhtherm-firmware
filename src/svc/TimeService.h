#pragma once
#include <Arduino.h>
#include <time.h>
#include "hal/RtcClock.h"

// Sorgente unica dell'ora. Epoch UTC; base = RTC validato al boot, correzioni da SNTP.
// RTC inaffidabile => trusted=false: si va avanti su una base millis() (epoch piccoli),
// il chiamante deve assumere il ritardo compressore NON trascorso e alzare allarme.
class TimeService {
 public:
  void begin(RtcClock* rtc, uint32_t persistedLastSeenEpoch, bool ntpEnabled,
             const String& ntpServer, const String& tz);

  void loop(bool staConnected);

  uint32_t nowEpoch() const {
    return (uint32_t)(base_ + (totalMs_ + (uint32_t)(millis() - lastMillis_)) / 1000ULL);
  }
  bool trusted() const { return trusted_; }
  bool rtcPresent() const { return rtc_ && rtc_->present(); }
  bool rtcHalted() const { return rtcHalted_; }
  bool ntpSynced() const { return ntpSynced_; }

  // Impostazione manuale dalla UI (epoch UTC): aggiorna base e RTC, diventa trusted.
  bool setManual(uint32_t epoch);

  // Salto d'orologio (>60 s) avvenuto dopo il boot per sync NTP o set manuale.
  // Il chiamante DEVE traslare del delta gli epoch àncora (profilo, guardia
  // compressore), altrimenti i tempi trascorsi risulterebbero falsati.
  int64_t consumeClockJump() {
    int64_t j = pendingJumpSec_;
    pendingJumpSec_ = 0;
    // Il salto grande viene APPLICATO a base_ QUI, non al momento in cui è rilevato: così
    // nowEpoch() e le àncore epoch (traslate dallo shiftClock del chiamante nello stesso
    // blocco) saltano nello stesso istante. Se base_ cambiasse prima, un handler eseguito
    // fra il rilevamento e questa consumazione scriverebbe un'àncora dal nuovo epoch, che
    // lo shiftClock poi traslerebbe una seconda volta (doppia traslazione → àncora nel 2080).
    if (j != 0) base_ += j;
    return j;
  }

  String localString(uint32_t epoch) const;

 private:
  void accumulate() {
    uint32_t m = millis();
    totalMs_ += (uint32_t)(m - lastMillis_);
    lastMillis_ = m;
  }

  RtcClock* rtc_ = nullptr;
  uint64_t base_ = 100;  // base epoch; 100 = fallback senza RTC
  uint64_t totalMs_ = 0;
  uint32_t lastMillis_ = 0;
  bool trusted_ = false;
  bool rtcHalted_ = false;
  bool ntpEnabled_ = false;
  bool ntpStarted_ = false;
  bool ntpSynced_ = false;
  String ntpServer_, tz_;
  uint32_t lastNtpCheckMs_ = 0;
  uint32_t lastRtcWriteMs_ = 0;
  int64_t pendingJumpSec_ = 0;
};
