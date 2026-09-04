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
  // dopo il test manuale. Gli 8 canali fisici sono una risorsa di DISPOSITIVO che le
  // camere si spartiscono: due camere non possono condividere lo stesso relè.
  void setMapping(uint8_t ch, uint8_t coolRel, uint8_t heatRel) {
    if (ch >= AX_N_CHAMBERS) return;
    map_[ch].cool = coolRel <= 8 ? coolRel : 0;
    map_[ch].heat = heatRel <= 8 ? heatRel : 0;
  }
  uint8_t coolRel(uint8_t ch) const { return ch < AX_N_CHAMBERS ? map_[ch].cool : 0; }
  uint8_t heatRel(uint8_t ch) const { return ch < AX_N_CHAMBERS ? map_[ch].heat : 0; }

  // Vero se il relè `rel` è già assegnato a una camera DIVERSA da `ch`. Due camere
  // sullo stesso relè significherebbe che il controllo di una comanda il carico
  // dell'altra: l'ultima ad attuare nel tick vince e i due regolatori si combattono
  // sullo stesso attuatore. Va rifiutato all'armamento, non scoperto sul campo.
  bool relayTakenByOther(uint8_t ch, uint8_t rel) const {
    if (!rel) return false;
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
      if (i == ch) continue;
      if (map_[i].cool == rel || map_[i].heat == rel) return true;
    }
    return false;
  }

  // Applica l'esito della pipeline di UNA camera. In modalità test è un no-op.
  void applyAutomation(uint8_t ch, bool heat, bool cool) {
    if (testMode_ || ch >= AX_N_CHAMBERS) return;
    if (map_[ch].heat) writeRel(map_[ch].heat, heat);
    if (map_[ch].cool) writeRel(map_[ch].cool, cool);
    map_[ch].heatOn = heat && map_[ch].heat;
    map_[ch].coolOn = cool && map_[ch].cool;
  }

  // Stato sicuro di DISPOSITIVO: spegne tutti gli 8 canali, di tutte le camere.
  void allOff() {
    for (uint8_t r = 1; r <= 8; r++) writeRel(r, false);
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) map_[i].heatOn = map_[i].coolOn = false;
  }

  // Spegne le sole uscite di UNA camera, lasciando intatte quelle delle altre: un
  // disarmo o un guasto su una camera non deve fermare la cotta dell'altra.
  void chamberOff(uint8_t ch) {
    if (ch >= AX_N_CHAMBERS) return;
    if (map_[ch].heat) writeRel(map_[ch].heat, false);
    if (map_[ch].cool) writeRel(map_[ch].cool, false);
    map_[ch].heatOn = map_[ch].coolOn = false;
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

  // Vero se il driver I2C delle uscite è in errore; rilevante solo per le camere il
  // cui mapping usa REL4..8 (mcpNeeded). Con più camere il guasto del MCP deve
  // togliere il consenso SOLO a chi dipende davvero da lui: se la camera 1 sta sui
  // GPIO diretti, un I2C morto non deve fermare la sua cotta.
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
    bool coolOn = false, heatOn = false;
  };
  Mcp23008* mcp_ = nullptr;
  ChMap map_[AX_N_CHAMBERS];
  bool states_[8] = {false};
  bool testMode_ = false;
  uint32_t testModeSinceMs_ = 0;
  uint32_t lastMcpRetryMs_ = 0;
};
