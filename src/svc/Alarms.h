#pragma once
#include <Arduino.h>
#include "svc/AppStatus.h"

// Tabella unica degli allarmi (3.15.0): stesso ordine, codici e testi per OLED, registro
// eventi e Home Assistant. L'ordine e' la priorita' dell'OLED (docs/ALLARMI.md): prima i guasti
// che mettono a rischio la cotta o i dati, poi 'orologio inaffidabile' (benigno ma spesso
// PERMANENTE, nasconderebbe i primi), poi gli informativi. Il failsafe NON e' in testa: e' un
// evento gia' avvenuto che resta alzato fino al riarmo, e in cima mascherebbe per sempre i
// guasti in corso e azionabili.
//
// Gli id sono scritti nella EEPROM del registro eventi: mai riordinarli ne' riusarli. Un
// allarme nuovo prende il primo id libero; la priorita' sull'OLED la dà kAlarmOrder.
enum AlarmId : uint8_t {
  AL_SONDA = 0,
  AL_SONDA_SOSPETTA = 1,
  AL_USCITE = 2,
  AL_CONFIG = 3,
  AL_CALDO_BLOCCATO = 4,
  AL_TROPPO_FREDDO = 5,
  AL_FRIGO_NON_SI_FERMA = 6,
  AL_LIMITE_ASSOLUTO = 7,
  AL_RIAVVII = 8,
  AL_SCOSTAMENTO = 9,
  AL_CRASH_LENTO = 10,
  AL_FLASH = 11,
  AL_FAILSAFE_INERTE = 12,
  AL_FAILSAFE = 13,
  AL_SONDA_CAMERA = 14,
  AL_DENSITA_STANTIA = 15,
  AL_RAMPA_FERMA = 16,
  AL_PIU_DENSIMETRI = 17,
  AL_OROLOGIO = 18,
  AL_BATTERIA = 19,
  AL_FERM_FERMA = 20,  // 3.16.0: avviso, non entra nell'aggregato MQTT «alarm»
  AL_COUNT
};
static_assert(AL_COUNT <= 32, "la maschera degli allarmi e' a 32 bit");

// Testi in flash, definiti in Alarms.cpp (una sola copia per tutto il firmware).
const __FlashStringHelper* alarmCode(uint8_t id);  // per HA e per il registro
const __FlashStringHelper* alarmOled(uint8_t id);  // riga dell'OLED senza il "! "

// Maschera degli allarmi attivi (bit = AlarmId).
inline uint32_t alarmMask(const AppStatus& st) {
  uint32_t m = 0;
  auto set = [&m](bool on, AlarmId id) { if (on) m |= (1UL << id); };
  set(st.alarmProbe, AL_SONDA);
  set(st.alarmProbeSuspect, AL_SONDA_SOSPETTA);
  set(st.alarmOutputs, AL_USCITE);
  set(st.alarmConfig, AL_CONFIG);
  set(st.heatLock != 0, AL_CALDO_BLOCCATO);
  set(st.alarmTooCold, AL_TROPPO_FREDDO);
  set(st.alarmCoolUncommanded, AL_FRIGO_NON_SI_FERMA);
  set(st.absLimited, AL_LIMITE_ASSOLUTO);
  set(st.alarmCrashLoop, AL_RIAVVII);
  set(st.alarmThermal, AL_SCOSTAMENTO);
  set(st.alarmCrashSlow, AL_CRASH_LENTO);
  set(st.alarmStorage, AL_FLASH);
  set(st.alarmFailsafeInerte, AL_FAILSAFE_INERTE);
  set(st.alarmFailsafe, AL_FAILSAFE);
  set(st.alarmAirProbe, AL_SONDA_CAMERA);
  set(st.alarmGravity, AL_DENSITA_STANTIA);
  set(st.alarmRampStall, AL_RAMPA_FERMA);
  set(st.alarmMultiGravity, AL_PIU_DENSIMETRI);
  set(st.alarmRtc, AL_OROLOGIO);
  set(st.alarmBattery, AL_BATTERIA);
  set(st.fermStuck, AL_FERM_FERMA);
  return m;
}

// Id dell'i-esimo allarme in ordine di priorita' (OLED).
uint8_t alarmByPriority(uint8_t i);
