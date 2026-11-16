#pragma once
#include <Arduino.h>
#include "hardware_map.h"
#include "hal/Mcp23008.h"
#include "ax_dimensions.h"

// Unico punto di contatto con i relè. Relè 1..3 su GPIO12/13/15, relè 4..8 su MCP23008.
// ATTENZIONE: D6/D7/D8 pilotano anche i LED — nessun uso dei pin fuori da questa classe.
// La modalità test (diagnostica, solo a sistema disarmato) sospende l'automazione.
class OutputBus {
 public:
  // Da chiamare PRIMA di ogni altra cosa in setup(): tutte le uscite GPIO spente.
  void safeInitEarly() {
    pinMode(PIN_REL1, OUTPUT);
    digitalWrite(PIN_REL1, LOW);
    pinMode(PIN_REL2, OUTPUT);
    digitalWrite(PIN_REL2, LOW);
    pinMode(PIN_REL3, OUTPUT);
    digitalWrite(PIN_REL3, LOW);
  }

  void attachMcp(Mcp23008* mcp) { mcp_ = mcp; }

  // Mapping (camera, funzione) -> relè (1..8, 0 = non assegnato). Configurato da UI
  // dopo il test manuale. Il firmware gestisce una sola camera (ch = 0).
  void setMapping(uint8_t ch, uint8_t coolRel, uint8_t heatRel) {
    if (ch >= AX_N_CHAMBERS) return;
    map_[ch].cool = coolRel <= 8 ? coolRel : 0;
    map_[ch].heat = heatRel <= 8 ? heatRel : 0;
  }
  uint8_t coolRel(uint8_t ch) const { return ch < AX_N_CHAMBERS ? map_[ch].cool : 0; }
  uint8_t heatRel(uint8_t ch) const { return ch < AX_N_CHAMBERS ? map_[ch].heat : 0; }

  // Applica l'esito della pipeline di UNA camera. In modalità test è un no-op.
  void applyAutomation(uint8_t ch, bool heat, bool cool) {
    if (testMode_ || ch >= AX_N_CHAMBERS) return;
    if (map_[ch].heat) writeRel(map_[ch].heat, heat);
    if (map_[ch].cool) writeRel(map_[ch].cool, cool);
    map_[ch].heatOn = heat && map_[ch].heat;
    map_[ch].coolOn = cool && map_[ch].cool;
  }

  // Ventola (3.17.0): rele' 1..8, 0 = nessuna. Chi chiama garantisce che non coincida con
  // caldo o freddo; qui si rifiuta comunque, per difesa. Cambiando rele' il vecchio si spegne.
  void setFan(uint8_t ch, uint8_t rel) {
    if (ch >= AX_N_CHAMBERS) return;
    if (rel > 8 || (rel && (rel == map_[ch].cool || rel == map_[ch].heat))) rel = 0;
    if (rel == map_[ch].fan) return;
    if (map_[ch].fan && !testMode_ && map_[ch].fan != map_[ch].cool && map_[ch].fan != map_[ch].heat)
      writeRel(map_[ch].fan, false);
    map_[ch].fan = rel;
    map_[ch].fanOn = false;
  }
  uint8_t fanRel(uint8_t ch) const { return ch < AX_N_CHAMBERS ? map_[ch].fan : 0; }
  void applyFan(uint8_t ch, bool on) {
    if (testMode_ || ch >= AX_N_CHAMBERS || !map_[ch].fan) return;
    if (on != map_[ch].fanOn || on != relayState(map_[ch].fan)) writeRel(map_[ch].fan, on);
    map_[ch].fanOn = on;
  }
  bool fanActive(uint8_t ch) const { return ch < AX_N_CHAMBERS && map_[ch].fanOn; }

  // Stato sicuro di DISPOSITIVO: spegne tutti gli 8 canali, di tutte le camere.
  void allOff() {
    for (uint8_t r = 1; r <= 8; r++) writeRel(r, false);
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) map_[i].heatOn = map_[i].coolOn = map_[i].fanOn = false;
  }

  // Spegne SOLO il caldo della camera (inizio di un upload OTA, SF-14, 3.14.0). Il freddo
  // resta com'e': spegnerlo senza risincronizzare la guardia del compressore darebbe un
  // corto ciclo se l'OTA si interrompe (vedi WebApi::handleUpdateUpload). Il caldo non ha
  // guardia: se l'OTA fallisce, il primo tick lo riaccende se serve.
  void heatOff(uint8_t ch) {
    if (testMode_ || ch >= AX_N_CHAMBERS || !map_[ch].heat) return;
    writeRel(map_[ch].heat, false);
    map_[ch].heatOn = false;
  }

  // Spegne le sole uscite mappate della camera (non i canali lasciati eccitati da un
  // test manuale: per quelli c'è allOff()).
  void chamberOff(uint8_t ch) {
    if (ch >= AX_N_CHAMBERS) return;
    if (map_[ch].heat) writeRel(map_[ch].heat, false);
    if (map_[ch].cool) writeRel(map_[ch].cool, false);
    if (map_[ch].fan) writeRel(map_[ch].fan, false);
    map_[ch].heatOn = map_[ch].coolOn = map_[ch].fanOn = false;
  }

  // --- Modalità test manuale (pagina diagnostica / seriale) ---
  bool enterTestMode(bool systemArmed) {
    if (systemArmed) return false;  // mai test con automazione armata
    testMode_ = true;
    testModeSinceMs_ = millis();
    return true;
  }
  void exitTestMode() {
    if (testMode_) {
      testMode_ = false;
      allOff();
    }
  }
  bool testMode() const { return testMode_; }
  // Timeout di sicurezza del test + retry periodico del MCP23008 dopo un errore I2C.
  void loop() {
    if (testMode_ && millis() - testModeSinceMs_ > 10UL * 60UL * 1000UL) exitTestMode();
    if (mcp_ && !mcp_->present() && millis() - lastMcpRetryMs_ > 5000) {
      lastMcpRetryMs_ = millis();
      if (mcp_->recover()) {
        // Il recovery riparte da OLAT=0: i relè 4..8 sono fisicamente spenti.
        for (uint8_t r = 4; r <= 8; r++) states_[r - 1] = false;
      }
    }
  }

  // Vero se il driver I2C delle uscite è in errore; rilevante solo se il mapping usa
  // REL4..8 (mcpNeeded): con caldo e freddo sui GPIO diretti un I2C morto non deve
  // fermare la cotta.
  bool mcpFault() const { return mcp_ && !mcp_->present(); }
  bool mcpNeeded(uint8_t ch) const {
    return ch < AX_N_CHAMBERS && (map_[ch].cool >= 4 || map_[ch].heat >= 4);
  }
  bool testRelay(uint8_t rel, bool on) {
    if (!testMode_ || rel < 1 || rel > 8) return false;
    testModeSinceMs_ = millis();
    return writeRel(rel, on);
  }

  bool heatActive(uint8_t ch) const { return ch < AX_N_CHAMBERS && map_[ch].heatOn; }
  bool coolActive(uint8_t ch) const { return ch < AX_N_CHAMBERS && map_[ch].coolOn; }
  bool relayState(uint8_t rel) const { return rel >= 1 && rel <= 8 ? states_[rel - 1] : false; }

 private:
  bool writeRel(uint8_t rel, bool on) {
    // Guardia sull'indice: states_[rel - 1] con rel == 0 scriverebbe FUORI dall'array.
    // Oggi non e' raggiungibile (tutti i chiamanti filtrano lo 0), ma questo e' il punto
    // unico di scrittura dei rele' e non deve dipendere dalla diligenza dei chiamanti.
    if (rel < 1 || rel > 8) return false;
    bool ok = true;
    switch (rel) {
      case 1: digitalWrite(PIN_REL1, on ? HIGH : LOW); break;
      case 2: digitalWrite(PIN_REL2, on ? HIGH : LOW); break;
      case 3: digitalWrite(PIN_REL3, on ? HIGH : LOW); break;
      default:
        ok = mcp_ && mcp_->setPin(rel - 4, on);
        break;
    }
    if (ok) states_[rel - 1] = on;
    return ok;
  }

  // Mapping e stato logico per camera. Lo stato FISICO dei relè resta in states_[8]:
  // i canali sono del dispositivo, l'assegnazione è della camera.
  struct ChMap {
    uint8_t cool = 0, heat = 0;   // 1..8, 0 = non assegnato
    uint8_t fan = 0;              // ventola (3.17.0); fuori da mcpNeeded: un guasto dell'MCP
                                  // sul rele' della ventola non deve fermare la cotta
    bool coolOn = false, heatOn = false, fanOn = false;
  };
  Mcp23008* mcp_ = nullptr;
  ChMap map_[AX_N_CHAMBERS];
  bool states_[8] = {false};
  bool testMode_ = false;
  uint32_t testModeSinceMs_ = 0;
  uint32_t lastMcpRetryMs_ = 0;
};
