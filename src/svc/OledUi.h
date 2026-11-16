#pragma once
#include <Arduino.h>
#include "svc/AppStatus.h"

class OLEDDisplay;

// Display di stato opzionale. Controller UNVERIFIED sullo schema: driver e indirizzo
// arrivano dalla configurazione (default SSD1306 @ 0x3C, alternativa SH1106).
//
// Dalla 3.15.0 a pagine, ogni 5 s: 1) stato (temperatura, setpoint, modo, uscite, primo
// allarme o IP); 2) fermentazione (densita', attenuazione, step e tempo che manca), solo con
// un profilo o un densimetro; 3) tutti gli allarmi, solo se sono piu' d'uno. Il contenuto si
// sposta di 0-2 pixel ogni 2 minuti contro il burn-in.
class OledUi {
 public:
  bool begin(bool enabled, const String& driver, uint8_t addr, bool flip, uint8_t brightness);
  void splash(const String& l1, const String& l2);
  void loop(uint32_t nowMs, const AppStatus& st);
  void setBrightness(uint8_t b);
  // Dati per la schermata di prima configurazione: mostrata solo con la rete di
  // configurazione attiva, finché le password sono quelle generate al primo avvio (*show)
  // E finché non e' impostata una rete di casa (*staSsid vuoto). Con una rete di casa l'AP
  // e' quello di emergenza: lo schermo resta sullo stato (temperatura, allarmi), che in
  // quel momento e' l'unico controllo locale. Puntatori alla configurazione: nessuna copia.
  void setSetupInfo(const String* host, const String* apPass, const String* uiPass,
                    const bool* show, const String* staSsid) {
    host_ = host; apPass_ = apPass; uiPass_ = uiPass; showSetup_ = show; staSsid_ = staSsid;
  }
  bool active() const { return disp_ != nullptr; }

 private:
  enum Page : uint8_t { PAGE_STATO = 0, PAGE_FERM = 1, PAGE_ALLARMI = 2 };
  void draw(const AppStatus& st, uint32_t nowMs);
  void drawSetup(const AppStatus& st);
  void drawStato(const AppStatus& st, uint32_t mask);
  void drawFerm(const AppStatus& st, uint32_t mask);
  void drawAllarmi(uint32_t mask);
  bool pageAvailable(uint8_t p, const AppStatus& st, uint32_t mask) const;
  const String* host_ = nullptr;
  const String* apPass_ = nullptr;
  const String* uiPass_ = nullptr;
  const bool* showSetup_ = nullptr;
  const String* staSsid_ = nullptr;
  OLEDDisplay* disp_ = nullptr;
  uint32_t lastDrawMs_ = 0;
  uint32_t pageSinceMs_ = 0;
  uint8_t page_ = PAGE_STATO;
  uint8_t dx_ = 0;  // spostamento anti burn-in
};
