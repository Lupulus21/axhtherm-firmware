#pragma once
#include <Arduino.h>
#include "svc/AppStatus.h"
#include "ax_dimensions.h"

class OLEDDisplay;

// Display di stato opzionale. Controller UNVERIFIED sullo schema: driver e indirizzo
// arrivano dalla configurazione (default SSD1306 @ 0x3C, alternativa SH1106).
class OledUi {
 public:
  bool begin(bool enabled, const String& driver, uint8_t addr, bool flip);
  void splash(const String& l1, const String& l2);
  // Con piu' camere il pannello (128x64) non si divide: si ALTERNA. Dimezzare lo
  // schermo renderebbe illeggibile la temperatura grande, che e' il dato per cui il
  // display esiste. A una sola camera non cambia nulla, nemmeno un indicatore.
  // idx/n servono solo al marcatore di camera; la rotazione la decide il chiamante,
  // che ha gia' gli stati sottomano — passarli tutti qui costringerebbe a copiarli.
  void loop(uint32_t nowMs, const AppStatus& st, uint8_t idx, uint8_t n);
  static constexpr uint32_t kPageMs = 4000;  // permanenza di una camera sul pannello
  bool active() const { return disp_ != nullptr; }

 private:
  void draw(const AppStatus& st, uint8_t idx, uint8_t n);
  OLEDDisplay* disp_ = nullptr;
  uint32_t lastDrawMs_ = 0;
};
