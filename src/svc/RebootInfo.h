#pragma once
#include <Arduino.h>
#include <user_interface.h>

// Perche' la scheda si e' riavviata, e quanto era durata la sessione prima. Vive nella
// memoria RTC: sopravvive ai reset software, al watchdog e alle eccezioni (non a un
// distacco di alimentazione) e non consuma flash. Il registro durevole e' BootLog.
//
// Riavvii a catena: un firmware che si blocca poco dopo l'avvio (un servizio che esaurisce
// la memoria, una configurazione che lo manda in eccezione) si riavvia all'infinito, e a
// ogni giro la cotta resta qualche secondo senza regolazione. Dopo kSafeStreak blocchi di
// fila, ognuno entro kShortSessionSec dall'avvio, si parte in MODALITA' RIDOTTA: MQTT
// spento (il servizio opzionale piu' complesso) e un allarme. Dopo kStableSec di
// funzionamento la modalita' ridotta finisce da sola (MQTT si riaccende); un riavvio
// voluto chiude la serie.
enum class RebootIntent : uint8_t {
  None = 0,
  User = 1,          // riavvio chiesto dalla pagina o dalla seriale
  Config = 2,        // salvataggio di impostazioni che richiedono il riavvio
  Ota = 3,           // aggiornamento firmware
  FactoryReset = 4,  // reset di fabbrica
};

class RebootInfo {
 public:
  static constexpr uint32_t kSafeStreak = 3;
  static constexpr uint32_t kShortSessionSec = 600;
  static constexpr uint32_t kStableSec = 1800;

  // Da chiamare una volta, all'avvio: legge la memoria RTC e il motivo del reset del core.
  void begin() {
    const rst_info* ri = ESP.getResetInfoPtr();
    rstReason = ri ? (uint8_t)ri->reason : 0;
    excCause = ri ? (uint8_t)ri->exccause : 0;
    epc1 = ri ? ri->epc1 : 0;
    const bool valido = ESP.rtcUserMemoryRead(kOffset, (uint32_t*)&rtc_, sizeof(rtc_)) &&
                        rtc_.magic == kMagic && rtc_.check == checksum(rtc_);
    prevUpSec = valido ? rtc_.upSec : 0;
    prevIntent = valido ? (uint8_t)rtc_.intent : 0;
    uint32_t serie = valido ? rtc_.streak : 0;
    // Blocco del firmware: watchdog hardware (1), eccezione (2), watchdog software (3).
    // In piu' i blocchi SOFTWARE (memoria esaurita in new, abort, assert, stack overflow,
    // panic): passano da postmortem e l'SDK li riporta come riavvio software (4), uguale a
    // ESP.restart(). Li distingue l'intento: ogni riavvio voluto di questo firmware lo
    // scrive prima (scheduleReboot, factoryReset), quindi un 4 senza intento e' un blocco.
    softCrash = valido && rstReason == REASON_SOFT_RESTART && prevIntent == 0;
    crash = rstReason == REASON_WDT_RST || rstReason == REASON_EXCEPTION_RST ||
            rstReason == REASON_SOFT_WDT_RST || softCrash;
    // Un blocco nella finestra fra l'intento e il riavvio resta un blocco: l'intento non
    // deve nasconderlo.
    if (crash) prevIntent = 0;
    // Serie: solo blocchi entro kShortSessionSec dall'avvio. Un blocco dopo una sessione
    // lunga non e' una catena e non conta.
    if (crash) serie = prevUpSec < kShortSessionSec ? serie + 1 : 0;
    else serie = 0;  // alimentazione, reset esterno, riavvio voluto: la serie si chiude
    crashStreak = serie;
    safeMode = serie >= kSafeStreak;
    rtc_ = Rtc{};
    rtc_.magic = kMagic;
    rtc_.streak = serie;
    write();
  }

  // Prima di un riavvio voluto: il prossimo avvio sapra' perche'.
  void setIntent(RebootIntent i) {
    rtc_.intent = (uint32_t)i;
    write();
  }

  // Ogni 10 secondi: durata della sessione (una sessione piu' breve risulta 0, cioe'
  // "meno di 10 s"), e serie azzerata se il firmware e' stabile. Ritorna true UNA volta,
  // quando la modalita' ridotta finisce: il chiamante riaccende cio' che aveva spento.
  bool tick(uint32_t sessionSec) {
    rtc_.upSec = sessionSec;
    bool uscita = false;
    if (sessionSec >= kStableSec) {
      rtc_.streak = 0;
      if (safeMode) { safeMode = false; uscita = true; }
    }
    write();
    return uscita;
  }

  // Esito dell'avvio corrente.
  uint8_t rstReason = 0, excCause = 0, prevIntent = 0;
  uint32_t epc1 = 0, prevUpSec = 0, crashStreak = 0;
  bool crash = false, softCrash = false, safeMode = false;

 private:
  // I primi 128 byte della memoria utente RTC li usa l'aggiornamento OTA (eboot_command):
  // si scrive dal blocco 64 in poi (byte 256).
  static constexpr uint32_t kOffset = 64;
  static constexpr uint32_t kMagic = 0xA7B00701;
  struct Rtc {
    uint32_t magic = 0, intent = 0, upSec = 0, streak = 0, check = 0;
  };
  static uint32_t checksum(const Rtc& r) {
    return (r.magic ^ (r.intent * 0x9E3779B1u) ^ (r.upSec * 0x85EBCA6Bu) ^ (r.streak * 0xC2B2AE35u)) + 0x5AA5;
  }
  void write() {
    rtc_.check = checksum(rtc_);
    ESP.rtcUserMemoryWrite(kOffset, (uint32_t*)&rtc_, sizeof(rtc_));
  }
  Rtc rtc_;
};
