#include "svc/TimeService.h"

void TimeService::begin(RtcClock* rtc, uint32_t persistedLastSeenEpoch, bool ntpEnabled,
                        const String& ntpServer, const String& tz) {
  rtc_ = rtc;
  ntpEnabled_ = ntpEnabled;
  ntpServer_ = ntpServer;
  tz_ = tz;
  lastMillis_ = millis();

  // TZ impostata subito: serve per la sola visualizzazione locale.
  setenv("TZ", tz_.c_str(), 1);
  tzset();

  uint32_t rtcEpoch = 0;
  bool halted = true;
  bool rtcOk = false;
  for (int i = 0; i < 3 && !rtcOk; i++) {  // un glitch I2C non deve costare la fiducia
    rtcOk = rtc_ && rtc_->present() && rtc_->readEpoch(&rtcEpoch, &halted);
    if (!rtcOk) delay(10);
  }
  if (rtcOk) {
    // Verifica di monotonia: se l'RTC è più indietro dell'ultimo istante visto
    // (con 2 min di margine), la batteria è sospetta: non fidarsi.
    if (persistedLastSeenEpoch > 0 && rtcEpoch + 120 < persistedLastSeenEpoch) {
      trusted_ = false;
    } else {
      base_ = rtcEpoch;
      trusted_ = true;
    }
  } else {
    trusted_ = false;
  }
  rtcHalted_ = halted;
}

void TimeService::loop(bool staConnected) {
  accumulate();

  if (!ntpEnabled_) return;
  if (!ntpStarted_ && staConnected) {
    configTime(tz_.c_str(), ntpServer_.c_str());
    ntpStarted_ = true;
    lastNtpCheckMs_ = millis();
    return;
  }
  if (ntpStarted_ && millis() - lastNtpCheckMs_ >= 10000) {
    lastNtpCheckMs_ = millis();
    time_t t = time(nullptr);
    if (t > 1600000000) {  // SNTP ha sincronizzato
      int64_t drift = (int64_t)t - (int64_t)nowEpoch();
      if (drift > 60 || drift < -60) {
        // Salto grande: NON applicare base_ ora. Lo fa consumeClockJump(), insieme allo
        // shiftClock delle àncore, così nessun handler eseguito prima della consumazione
        // scrive un'àncora dal nuovo epoch (che verrebbe poi ri-traslata: doppia traslazione).
        pendingJumpSec_ += drift;
      } else if (drift > 2 || drift < -2) {
        // Anche una correzione PICCOLA (2..60 s) passa per pendingJumpSec_, così consumeClockJump()
        // la applica a base_ INSIEME allo shiftClock delle àncore (guardia compressore, latch
        // isteresi, finestre profilo). Applicarla qui direttamente gonfierebbe elapsed(now, àncora)
        // di "drift" → riposo/attesa FANTASMA; col riposo minimo vicino al limite, un corto ciclo
        // del compressore. La banda morta di ±2 s evita churn di shiftClock a regime.
        pendingJumpSec_ += drift;
      }
      ntpSynced_ = true;
      trusted_ = true;
      // Aggiorna il DS1307 al massimo una volta ogni 6 ore.
      if (rtc_ && rtc_->present() &&
          (lastRtcWriteMs_ == 0 || millis() - lastRtcWriteMs_ > 6UL * 3600UL * 1000UL)) {
        uint32_t rtcEpoch = 0;
        bool needWrite = true;
        // Confronto/scrittura contro t (il tempo NTP autorevole), non nowEpoch(): con
        // base_ differito, nowEpoch() è ancora il tempo vecchio in questo punto.
        if (rtc_->readEpoch(&rtcEpoch)) {
          int64_t d = (int64_t)t - (int64_t)rtcEpoch;
          needWrite = d > 5 || d < -5;
        }
        if (needWrite) rtc_->writeEpoch((uint32_t)t);
        lastRtcWriteMs_ = millis();
      }
    }
  }
}

bool TimeService::setManual(uint32_t epoch) {
  if (epoch < 1577836800UL) return false;  // prima del 2020: rifiutata
  accumulate();
  int64_t jump = (int64_t)epoch - (int64_t)nowEpoch();
  if (jump > 60 || jump < -60) {
    // Salto grande: lo applica consumeClockJump() (atomico con lo shiftClock delle
    // àncore), non qui — così nessun handler scrive un'àncora dal nuovo epoch prima
    // della traslazione. base_ resta invariato finché il salto non è consumato.
    pendingJumpSec_ += jump;
  } else if (jump > 2 || jump < -2) {
    pendingJumpSec_ += jump;  // 2..60 s: applicato da consumeClockJump con lo shiftClock delle àncore
  } else {
    base_ = (uint64_t)epoch - totalMs_ / 1000ULL;  // <=2 s: banda morta, correzione diretta trascurabile
  }
  trusted_ = true;
  if (rtc_ && rtc_->present()) rtc_->writeEpoch(epoch);
  return true;
}

String TimeService::localString(uint32_t epoch) const {
  time_t t = (time_t)epoch;
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &tmv);
  return String(buf);
}
