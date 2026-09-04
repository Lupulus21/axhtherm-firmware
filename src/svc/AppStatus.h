#pragma once
#include <Arduino.h>

// Fotografia dello stato per OLED, MQTT e /api/status — costruita una volta al secondo.
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
  String ip = "-";
  bool staUp = false, apUp = false;
  int rssi = 0;
  uint32_t nowEpoch = 0;
  bool timeTrusted = false;
  uint32_t coolBlockedSec = 0;
  float pidDuty = 0;
  float predCoolCph = 0, predHeatCph = 0;  // coefficienti spegnimento predittivo (°C/h)
  uint32_t freeHeap = 0;
};
