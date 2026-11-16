#pragma once
#include <Arduino.h>

// Fotografia dello stato per OLED, MQTT e /api/status — costruita una volta al secondo.
// Motivo del blocco del caldo (ax::HeatLock) con il rimedio, in flash. Unica fonte del
// testo: lo usano /api/status e la seriale.
inline const __FlashStringHelper* heatLockText(uint8_t code) {
  switch (code) {
    case 1: return F("CALDO BLOCCATO: caldo acceso di continuo ma la sonda di controllo non sale "
                     "(sonda ferma o fuori dal mosto?). Controlla la sonda, poi sblocca.");
    case 2: return F("CALDO BLOCCATO: caldo acceso di continuo oltre il tempo massimo. Controlla "
                     "riscaldatore e sonda, poi sblocca.");
    case 3: return F("CALDO BLOCCATO: temperatura in salita rapida a caldo SPENTO (rele'/SSR del "
                     "caldo incollato?). Controlla l'SSR, poi sblocca.");
    case 4: return F("CALDO BLOCCATO: freddo acceso da ore e temperatura che non scende (caldo che "
                     "lavora contro il frigo, o frigo guasto?). Controlla SSR e frigo, poi sblocca.");
    default: return F("CALDO BLOCCATO");
  }
}

// Cold crash: nome del profilo attivo che lo identifica e le due velocita' di discesa
// scelte dall'utente: 3 °C ogni 6 ore (28/10/2026) e 1,5 °C ogni 6 ore (29/10/2026).
static constexpr const char* kColdCrashName = "Cold crash";
static constexpr float kColdCrashCph = 0.5f;
static constexpr float kColdCrashSlowCph = 0.25f;

struct AppStatus {
  float tControl = 0, tAux1 = 0, tAux2 = 0;
  bool tControlValid = false, tAux1Valid = false, tAux2Valid = false;
  bool hasSetpoint = false;
  float setpoint = 0;
  bool heat = false, cool = false, armed = false;
  uint8_t mode = 0;  // 0=off 1=termostato 2=profilo
  bool paused = false, completed = false, waitingDelayedStart = false;
  uint8_t stepIdx = 0, stepCount = 0;      // indice/conteggio ASSOLUTI (indicizzazione)
  uint8_t stepEnIdx = 0, stepEnCount = 0;  // basati sugli step ABILITATI (visualizzazione)
  uint32_t stepEndsAt = 0;
  // Step "attendi il target": il mosto non e' ancora arrivato (il tempo non conta), o il
  // tempo e' partito per scadenza dell'attesa di 24 ore invece che per arrivo.
  bool waitingTarget = false, waitTimedOut = false;
  String profileName = "", batchName = "";
  uint32_t stepStartEpoch = 0;
  float stepStartTemp = 0;
  bool gravityHas = false, gravityStale = false;
  float gravity = 0, gravityBattery = 0;
  uint32_t gravityAgeSec = 0;
  float gravityOg = 0;       // densità iniziale (0 = non impostata)
  float gravityTemp = 0;     // temperatura riportata dallo iSpindel
  float gravityAngle = 0;    // angolo di galleggiamento dello iSpindel
  int gravityRssi = 0;       // segnale WiFi dello iSpindel
  float attenuation = 0;     // % attenuazione apparente (se og+sg disponibili)
  float abv = 0;             // % ABV stimato
  uint32_t profileStartEpoch = 0;  // inizio del profilo, per i giorni di fermentazione
  bool profileWaitsGravity = false;
  bool profileNoLimit = false;      // step a densità senza timeout: non avanzerà mai da solo  // step a densità fermo: allarme solo se il dato esisteva
  bool alarmProbe = false, alarmConfig = false, alarmRtc = false, alarmGravity = false,
       alarmBattery = false, alarmOutputs = false;
  bool alarmRampStall = false;    // rampa °C/h che l'impianto non raggiunge (profilo appeso)
  bool alarmStorage = false;      // una scrittura su LittleFS è fallita (stato non persistito)
  bool alarmMultiGravity = false; // più densimetri attivi senza densimetro scelto
  bool testMode = false;          // modalità test relè attiva: un relè può essere eccitato a mano
  bool alarmThermal = false;      // T lontana dal setpoint stabile da ore (impianto non ce la fa)
  // Il failsafe su timer hardware ha spento i rele' perche' il loop di controllo era
  // fermo da oltre un minuto. E' un evento raro e grave: resta visibile finche' non si
  // riarma, altrimenti non ne resterebbe traccia.
  bool alarmFailsafe = false;
  // Il timer del failsafe non sta girando: la protezione dichiarata NON e' attiva. Un
  // dispositivo di sicurezza che fallisce in silenzio e' peggio che non averlo.
  bool alarmFailsafeInerte = false;
  bool alarmProbeSuspect = false; // sonda di controllo divergente da tutte le ausiliarie (sospetta)
  // Caldo bloccato da un rilevatore di guasto del riscaldatore (ax::HeatLock, 0 = no) e
  // istante del blocco. Resta finché l'utente non sblocca, anche attraverso i riavvii.
  // Pendenza del mosto (°C/h) e tempo d'arrivo al setpoint (min, -1 = non stimabile).
  bool slopeValid = false;
  float slopeCph = 0.0f;
  int32_t etaMin = -1;
  // Limiti attivi in questo momento: freddo/caldo sospesi dall'aria della camera, limiti
  // assoluti sul mosto. La sonda della camera configurata ma non valida li disattiva.
  bool airCoolLimited = false, airHeatLimited = false, absLimited = false;
  bool alarmAirProbe = false;
  bool alarmCoolUncommanded = false;  // relè del frigo incollato? (solo allarme)
  // Cold crash in corso: target, istante d'arrivo (0 = non ancora), stima dell'arrivo
  // (minuti, -1 = nessuna; misurata o sulla velocita' programmata), discesa delle ultime
  // ore e soglia dell'allarme.
  bool crashActive = false;
  float crashTargetC = 0.0f;
  uint32_t crashStartEpoch = 0, crashReachedEpoch = 0;
  int32_t crashEtaMin = -1;
  bool crashEtaMeasured = false;
  float crashDropC = 0.0f, crashWindowH = 0.0f, crashMinDropC = 0.0f, crashCph = 0.0f;
  bool alarmCrashSlow = false;
  bool antigelo = false;  // caldo riacceso come antigelo su uno step che lo vieta
  bool alarmTooCold = false;  // mosto sotto -2 °C, o 2 °C sotto il setpoint con l'antigelo
  uint32_t loopMaxLateMs = 0;    // ritardo massimo del tick dall'avvio (LoopStats)
  bool alarmCrashLoop = false;    // riavvii a catena: modalita' ridotta (MQTT spento)
  uint8_t heatLock = 0;
  uint32_t heatLockEpoch = 0;
  String ip = "-";
  bool staUp = false, apUp = false;
  int rssi = 0;
  uint32_t nowEpoch = 0;
  bool timeTrusted = false;
  uint32_t coolBlockedSec = 0;
  float pidDuty = 0;
  float predCoolCph = 0, predHeatCph = 0;  // coefficienti spegnimento predittivo (°C/h)
  uint32_t freeHeap = 0;
  // 3.15.0: stato di MQTT (0 spento, 1 non connesso, 2 connesso) e tentativi falliti di fila
  // (ID-08); registro eventi nella EEPROM attivo.
  bool gravityPlato = false;  // densita' in °Plato (altrimenti SG): per l'OLED
  // 3.16.0: fine fermentazione e step che aspetta la conferma dell'utente.
  bool waitingConfirm = false;
  uint8_t fermTrend = 0;       // ax::GravityTrend::State: 0 ?, 1 attiva, 2 rallenta, 3 ferma
  bool fermSlopeValid = false;
  float fermSlopeDay = 0.0f;   // nell'unita' della densita', al giorno (negativa = in calo)
  bool fermDone = false;       // ferma da 48 h con attenuazione sufficiente (avviso)
  bool fermStuck = false;      // ferma ma sopra l'atteso (avviso, disattivabile)
  // 3.17.0: ventola (rele' effettivo, 0 = nessuno) e conflitto con caldo/freddo.
  bool fan = false;
  uint8_t fanRel = 0;
  bool fanConflict = false;
  uint8_t mqttState = 0;
  uint16_t mqttFails = 0;
  bool eventsOn = false;
  uint32_t uptimeSec = 0;
};
