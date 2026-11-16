#include "svc/Alarms.h"

namespace {
struct AlarmInfo {
  const char* code;
  const char* oled;
};
#define AX_AL(id, c, o) const char kAlC_##id[] PROGMEM = c; const char kAlO_##id[] PROGMEM = o;
AX_AL(0, "SONDA_GUASTA", "SONDA GUASTA")
AX_AL(1, "SONDA_SOSPETTA", "SONDA MOSTO SOSPETTA")
AX_AL(2, "USCITE_KO", "USCITE (MCP) KO")
AX_AL(3, "CONFIG_NON_VALIDA", "CONFIG NON VALIDA")
AX_AL(4, "CALDO_BLOCCATO", "CALDO BLOCCATO")
AX_AL(5, "MOSTO_TROPPO_FREDDO", "MOSTO TROPPO FREDDO")
AX_AL(6, "FRIGO_NON_SI_FERMA", "FRIGO NON SI FERMA")
AX_AL(7, "LIMITE_ASSOLUTO", "LIMITE ASSOLUTO T")
AX_AL(8, "RIAVVII_A_CATENA", "RIAVVII A CATENA")
AX_AL(9, "SCOSTAMENTO", "SCOSTAMENTO T")
AX_AL(10, "COLD_CRASH_LENTO", "COLD CRASH LENTO")
AX_AL(11, "SCRITTURA_FLASH", "SCRITTURA FLASH KO")
AX_AL(12, "FAILSAFE_NON_ATTIVO", "FAILSAFE NON ATTIVO")
AX_AL(13, "FAILSAFE", "FAILSAFE: LOOP BLOCCATO")
AX_AL(14, "SONDA_CAMERA", "SONDA CAMERA KO")
AX_AL(15, "DENSITA_STANTIA", "DENSITA' STANTIA")
AX_AL(16, "RAMPA_FERMA", "RAMPA FERMA")
AX_AL(17, "PIU_DENSIMETRI", "PIU' DENSIMETRI")
AX_AL(18, "OROLOGIO", "OROLOGIO INAFFIDABILE")
AX_AL(19, "BATTERIA_DENSIMETRO", "BATT. iSPINDEL")
AX_AL(20, "FERMENTAZIONE_FERMA", "FERMENTAZ. FERMA?")
#undef AX_AL

const AlarmInfo kAlarmInfo[AL_COUNT] PROGMEM = {
    {kAlC_0, kAlO_0},   {kAlC_1, kAlO_1},   {kAlC_2, kAlO_2},   {kAlC_3, kAlO_3},
    {kAlC_4, kAlO_4},   {kAlC_5, kAlO_5},   {kAlC_6, kAlO_6},   {kAlC_7, kAlO_7},
    {kAlC_8, kAlO_8},   {kAlC_9, kAlO_9},   {kAlC_10, kAlO_10}, {kAlC_11, kAlO_11},
    {kAlC_12, kAlO_12}, {kAlC_13, kAlO_13}, {kAlC_14, kAlO_14}, {kAlC_15, kAlO_15},
    {kAlC_16, kAlO_16}, {kAlC_17, kAlO_17}, {kAlC_18, kAlO_18}, {kAlC_19, kAlO_19},
    {kAlC_20, kAlO_20},
};

// Priorita' dell'OLED (vedi Alarms.h). La fermentazione ferma e' un avviso: sta prima
// dell'orologio (che spesso resta acceso a lungo e la nasconderebbe) e della batteria.
const uint8_t kAlarmOrder[AL_COUNT] PROGMEM = {
    AL_SONDA,           AL_SONDA_SOSPETTA, AL_USCITE,             AL_CONFIG,
    AL_CALDO_BLOCCATO,  AL_TROPPO_FREDDO,  AL_FRIGO_NON_SI_FERMA, AL_LIMITE_ASSOLUTO,
    AL_RIAVVII,         AL_SCOSTAMENTO,    AL_CRASH_LENTO,        AL_FLASH,
    AL_FAILSAFE_INERTE, AL_FAILSAFE,       AL_SONDA_CAMERA,       AL_DENSITA_STANTIA,
    AL_RAMPA_FERMA,     AL_PIU_DENSIMETRI, AL_FERM_FERMA,         AL_OROLOGIO,
    AL_BATTERIA,
};
}  // namespace

const __FlashStringHelper* alarmCode(uint8_t id) {
  if (id >= AL_COUNT) return F("?");
  return (const __FlashStringHelper*)pgm_read_ptr(&kAlarmInfo[id].code);
}
const __FlashStringHelper* alarmOled(uint8_t id) {
  if (id >= AL_COUNT) return F("?");
  return (const __FlashStringHelper*)pgm_read_ptr(&kAlarmInfo[id].oled);
}
uint8_t alarmByPriority(uint8_t i) { return i < AL_COUNT ? pgm_read_byte(&kAlarmOrder[i]) : 0; }
