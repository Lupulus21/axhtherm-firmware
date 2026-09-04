#include "svc/OledUi.h"
#include <SSD1306Wire.h>
#include <SH1106Wire.h>
#include "hardware_map.h"

bool OledUi::begin(bool enabled, const String& driver, uint8_t addr, bool flip) {
  if (!enabled) return false;
  // Frequenza I2C a 100 kHz: il bus è condiviso col DS1307 (max 100 kHz);
  // il default della libreria (700 kHz) lo manderebbe fuori specifica.
  if (driver == "sh1106") {
    disp_ = new SH1106Wire(addr, PIN_I2C_SDA, PIN_I2C_SCL, GEOMETRY_128_64, I2C_ONE, 100000);
  } else {
    disp_ = new SSD1306Wire(addr, PIN_I2C_SDA, PIN_I2C_SCL, GEOMETRY_128_64, I2C_ONE, 100000);
  }
  if (!disp_) return false;  // new può ritornare nullptr su heap esaurito (no eccezioni): non deref.
  if (!disp_->init()) {
    delete disp_;
    disp_ = nullptr;
    return false;
  }
  if (flip) disp_->flipScreenVertically();
  disp_->clear();
  disp_->display();
  return true;
}

void OledUi::splash(const String& l1, const String& l2) {
  if (!disp_) return;
  disp_->clear();
  disp_->setTextAlignment(TEXT_ALIGN_CENTER);
  disp_->setFont(ArialMT_Plain_16);
  disp_->drawString(64, 14, l1);
  disp_->setFont(ArialMT_Plain_10);
  disp_->drawString(64, 38, l2);
  disp_->display();
}

void OledUi::loop(uint32_t nowMs, const AppStatus& st, uint8_t idx, uint8_t n) {
  if (!disp_) return;
  if (nowMs - lastDrawMs_ < 1000) return;
  lastDrawMs_ = nowMs;
  draw(st, idx, n);
}

void OledUi::draw(const AppStatus& st, uint8_t idx, uint8_t n) {
  disp_->clear();
  disp_->setTextAlignment(TEXT_ALIGN_LEFT);

  // Temperatura di controllo, grande.
  disp_->setFont(ArialMT_Plain_24);
  String t = st.tControlValid ? String(st.tControl, 1) + "\xB0" : String("--.-");
  disp_->drawString(0, 0, t);

  // Setpoint a destra.
  disp_->setFont(ArialMT_Plain_16);
  disp_->setTextAlignment(TEXT_ALIGN_RIGHT);
  if (st.hasSetpoint) disp_->drawString(128, 4, "> " + String(st.setpoint, 1));
  disp_->setTextAlignment(TEXT_ALIGN_LEFT);

  // Riga modalità.
  disp_->setFont(ArialMT_Plain_10);
  String mode;
  switch (st.mode) {
    case 2:
      mode = "PROFILO " + String(st.stepEnIdx + 1) + "/" + String(st.stepEnCount);
      if (st.paused) mode += " PAUSA";
      if (st.completed) mode += " FINE";
      if (st.waitingDelayedStart) mode += " ATTESA";
      break;
    case 1: mode = "TERMOSTATO"; break;
    default: mode = "FERMO"; break;
  }
  if (!st.armed) mode += "  [DISARMATO]";
  // Marcatore di camera solo quando ce n'e' piu' di una: a camera singola il pannello
  // resta identico a quello della v2.23.2, senza indicatori che non servono a nulla.
  if (n > 1) mode = "C" + String(idx + 1) + " " + mode;
  disp_->drawString(0, 27, mode);

  // Riga uscite.
  String out;
  if (st.testMode) out = "TEST RELE' ATTIVO";  // un relè può essere eccitato a mano
  else if (st.heat) out = "RISCALDA";
  else if (st.cool) out = "RAFFREDDA";
  else if (st.coolBlockedSec > 0 && st.hasSetpoint)
    out = "attesa frigo " + String(st.coolBlockedSec) + "s";
  else out = "riposo";
  disp_->drawString(0, 39, out);

  // Riga inferiore: allarmi oppure IP.
  String bottom;
  // Priorità: prima i guasti che mettono a rischio la cotta o i dati, poi 'orologio
  // inaffidabile' (benigno ma spesso PERMANENTE, nasconderebbe i primi), poi gli informativi.
  if (st.alarmProbe) bottom = "! SONDA GUASTA";
  else if (st.alarmProbeSuspect) bottom = "! SONDA MOSTO SOSPETTA";
  else if (st.alarmOutputs) bottom = "! USCITE (MCP) KO";
  else if (st.alarmConfig) bottom = "! CONFIG NON VALIDA";
  else if (st.alarmThermal) bottom = "! SCOSTAMENTO T";
  else if (st.alarmStorage) bottom = "! SCRITTURA FLASH KO";
  // Il failsafe sta QUI, non in testa: e' un evento GIA' AVVENUTO e resta alzato
  // finche' non lo si prende in carico, quindi in cima avrebbe mascherato per sempre
  // i guasti IN CORSO e azionabili (sonda, uscite, config). E' esattamente il
  // criterio dichiarato nel commento in testa a questa catena. Resta comunque primo
  // in /api/status e nell'aggregato MQTT, dove un elenco puo' contenerli tutti.
  else if (st.alarmFailsafeInerte) bottom = "! FAILSAFE NON ATTIVO";
  else if (st.alarmFailsafe) bottom = "! FAILSAFE: LOOP BLOCCATO";
  else if (st.alarmGravity) bottom = "! DENSITA' STANTIA";
  else if (st.alarmRampStall) bottom = "! RAMPA FERMA";
  else if (st.alarmMultiGravity) bottom = "! PIU' DENSIMETRI";
  else if (st.alarmRtc) bottom = "! OROLOGIO INAFFIDABILE";
  else if (st.alarmBattery) bottom = "! BATT. iSPINDEL";
  else bottom = st.ip + (st.apUp ? " (AP)" : "");
  disp_->drawString(0, 51, bottom);

  disp_->display();
}
