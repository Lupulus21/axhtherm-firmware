#include "svc/OledUi.h"
#include <SSD1306Wire.h>
#include <SH1106Wire.h>
#include "hardware_map.h"
#include "svc/NetworkManager.h"
#include "svc/Alarms.h"

bool OledUi::begin(bool enabled, const String& driver, uint8_t addr, bool flip,
                   uint8_t brightness) {
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
  setBrightness(brightness);
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

void OledUi::setBrightness(uint8_t b) {
  if (!disp_) return;
  disp_->setBrightness(b < 5 ? 5 : b);
}

void OledUi::loop(uint32_t nowMs, const AppStatus& st) {
  if (!disp_) return;
  if (nowMs - lastDrawMs_ < 1000) return;
  lastDrawMs_ = nowMs;
  draw(st, nowMs);
}

void OledUi::drawSetup(const AppStatus& st) {
  disp_->clear();
  disp_->setTextAlignment(TEXT_ALIGN_LEFT);
  disp_->setFont(ArialMT_Plain_10);
  disp_->drawString(0, 0, F("PRIMA CONFIGURAZIONE"));
  disp_->drawString(0, 13, NetworkManager::apSsidFor(*host_));
  disp_->drawString(0, 26, String(F("pw rete: ")) + NetworkManager::apPassFor(*apPass_));
  disp_->drawString(0, 39, String(F("web: ")) + st.ip);
  disp_->drawString(0, 52, String(F("admin: ")) + *uiPass_);
  disp_->display();
}

// Primo allarme in ordine di priorita' (svc/Alarms.h); AL_COUNT se nessuno.
static uint8_t primoAllarme(uint32_t mask) {
  for (uint8_t i = 0; i < AL_COUNT; i++) {
    const uint8_t id = alarmByPriority(i);
    if (mask & (1UL << id)) return id;
  }
  return AL_COUNT;
}

static uint8_t contaAllarmi(uint32_t mask) {
  uint8_t n = 0;
  for (; mask; mask &= mask - 1) n++;
  return n;
}

bool OledUi::pageAvailable(uint8_t p, const AppStatus& st, uint32_t mask) const {
  switch (p) {
    case PAGE_STATO: return true;
    case PAGE_FERM: return st.mode == 2 || st.gravityHas;
    case PAGE_ALLARMI: return contaAllarmi(mask) >= 2;
  }
  return false;
}

void OledUi::draw(const AppStatus& st, uint32_t nowMs) {
  if (st.apUp && showSetup_ && *showSetup_ && host_ && apPass_ && uiPass_ && staSsid_ &&
      staSsid_->length() == 0) {
    drawSetup(st);
    return;
  }
  const uint32_t mask = alarmMask(st);
  // Anti burn-in: 0, 1, 2, 1, 0... pixel ogni 2 minuti.
  {
    static const uint8_t kPasso[4] = {0, 1, 2, 1};
    dx_ = kPasso[(nowMs / 120000UL) % 4];
  }
  // Pagina successiva ogni 5 s, saltando quelle che non hanno nulla da dire.
  if (nowMs - pageSinceMs_ >= 5000 || !pageAvailable(page_, st, mask)) {
    pageSinceMs_ = nowMs;
    for (uint8_t k = 1; k <= 3; k++) {
      const uint8_t p = (uint8_t)((page_ + k) % 3);
      if (pageAvailable(p, st, mask)) {
        page_ = p;
        break;
      }
    }
  }
  disp_->clear();
  disp_->setTextAlignment(TEXT_ALIGN_LEFT);
  if (page_ == PAGE_FERM) drawFerm(st, mask);
  else if (page_ == PAGE_ALLARMI) drawAllarmi(mask);
  else drawStato(st, mask);
  disp_->display();
}

// Riga in basso: il primo allarme (con quanti altri ce ne sono) oppure l'informazione data.
static String rigaAllarme(uint32_t mask) {
  const uint8_t id = primoAllarme(mask);
  if (id >= AL_COUNT) return String();
  String s = F("! ");
  s += alarmOled(id);
  const uint8_t n = contaAllarmi(mask);
  if (n > 1) {
    s += F(" +");
    s += String(n - 1);
  }
  return s;
}

void OledUi::drawStato(const AppStatus& st, uint32_t mask) {
  const int x = dx_, xr = 126 + dx_;
  // Temperatura di controllo, grande.
  disp_->setFont(ArialMT_Plain_24);
  String t = st.tControlValid ? String(st.tControl, 1) + "\xB0" : String("--.-");
  disp_->drawString(x, 0, t);

  // Setpoint a destra.
  disp_->setFont(ArialMT_Plain_16);
  disp_->setTextAlignment(TEXT_ALIGN_RIGHT);
  if (st.hasSetpoint) disp_->drawString(xr, 4, "> " + String(st.setpoint, 1));
  disp_->setTextAlignment(TEXT_ALIGN_LEFT);

  // Riga modalità.
  disp_->setFont(ArialMT_Plain_10);
  String mode;
  switch (st.mode) {
    case 2:
      // Cold crash: si dice quello, non "profilo 1/1".
      if (st.crashActive) mode = st.crashReachedEpoch ? F("COLD CRASH: OK") : F("COLD CRASH");
      else mode = "PROFILO " + String(st.stepEnIdx + 1) + "/" + String(st.stepEnCount);
      if (st.paused) mode += F(" PAUSA");
      if (st.waitingConfirm) mode += F(" CONFERMA");
      if (st.completed && !st.crashActive) mode += F(" FINE");
      if (st.waitingDelayedStart) mode += F(" ATTESA");
      break;
    case 1: mode = F("TERMOSTATO"); break;
    default: mode = F("FERMO"); break;
  }
  if (!st.armed) mode += F("  [DISARMATO]");
  disp_->drawString(x, 27, mode);

  // Riga uscite.
  String out;
  if (st.testMode) out = F("TEST RELE' ATTIVO");  // un relè può essere eccitato a mano
  else if (st.heat) out = F("RISCALDA");
  else if (st.cool) out = F("RAFFREDDA");
  else if (st.coolBlockedSec > 0 && st.hasSetpoint)
    out = "attesa frigo " + String(st.coolBlockedSec) + "s";
  else out = F("riposo");
  if (st.fan && !st.testMode) out += F(" +vent.");
  disp_->drawString(x, 39, out);

  // Riga inferiore: il primo allarme (ordine in svc/Alarms.h) oppure l'IP.
  String bottom = rigaAllarme(mask);
  if (!bottom.length()) bottom = st.ip + (st.apUp ? " (AP)" : "");
  disp_->drawString(x, 51, bottom);
}

// Tempo che manca: "3g 04h", "5h 20m", "12m".
static String durata(uint32_t sec) {
  char b[16];
  if (sec >= 86400UL) snprintf(b, sizeof(b), "%lug %02luh", (unsigned long)(sec / 86400UL),
                               (unsigned long)((sec % 86400UL) / 3600UL));
  else if (sec >= 3600UL) snprintf(b, sizeof(b), "%luh %02lum", (unsigned long)(sec / 3600UL),
                                   (unsigned long)((sec % 3600UL) / 60UL));
  else snprintf(b, sizeof(b), "%lum", (unsigned long)(sec / 60UL));
  return String(b);
}

void OledUi::drawFerm(const AppStatus& st, uint32_t mask) {
  const int x = dx_, xr = 126 + dx_;
  // Densita', grande, con l'OG a destra.
  disp_->setFont(ArialMT_Plain_16);
  String g = F("--");
  if (st.gravityHas)
    g = st.gravityPlato ? String(st.gravity, 1) + F(" \xB0P") : String(st.gravity, 3);
  disp_->drawString(x, 0, g);
  disp_->setFont(ArialMT_Plain_10);
  if (st.gravityOg > 0) {
    disp_->setTextAlignment(TEXT_ALIGN_RIGHT);
    disp_->drawString(xr, 5, String(F("OG ")) +
                                 (st.gravityPlato ? String(st.gravityOg, 1) : String(st.gravityOg, 3)));
    disp_->setTextAlignment(TEXT_ALIGN_LEFT);
  }
  // Attenuazione e ABV, oppure da quanto tace il densimetro.
  String r2;
  if (st.gravityHas && st.gravityOg > 0)
    r2 = String(F("Att. ")) + String(st.attenuation, 0) + F("%  ABV ") + String(st.abv, 1) + F("%");
  else if (!st.gravityHas)
    r2 = F("nessun dato del densimetro");
  if (st.gravityHas && st.gravityStale) r2 = F("densita' STANTIA");
  disp_->drawString(x, 20, r2);

  // Step e tempo che manca.
  String r3;
  if (st.mode == 2) {
    if (st.crashActive) r3 = st.crashReachedEpoch ? F("cold crash al target") : F("cold crash in discesa");
    else if (st.completed) r3 = F("profilo completato");
    else {
      r3 = String(F("Step ")) + String(st.stepEnIdx + 1) + "/" + String(st.stepEnCount) + "  ";
      if (st.paused) r3 += F("in pausa");
      else if (st.waitingConfirm) r3 += F("CONFERMA!");
      else if (st.waitingDelayedStart) r3 += F("avvio differito");
      else if (st.waitingTarget) r3 += F("attesa target");
      else if (st.profileWaitsGravity) r3 += F("attesa densita'");
      else if (st.stepEndsAt > st.nowEpoch && st.nowEpoch > 1500000000UL)
        r3 += String(F("resta ")) + durata(st.stepEndsAt - st.nowEpoch);
    }
  } else {
    r3 = st.mode == 1 ? F("termostato") : F("fermo");
  }
  disp_->drawString(x, 34, r3);

  // In basso: il primo allarme, la fine della fermentazione, l'andamento o l'eta' della lettura.
  String bottom = rigaAllarme(mask);
  if (!bottom.length() && st.fermDone) bottom = F("FERMENTAZIONE FINITA");
  if (!bottom.length() && st.gravityHas && st.fermSlopeValid && st.fermTrend >= 1 && st.fermTrend <= 2) {
    bottom = st.fermTrend == 1 ? F("attiva ") : F("rallenta ");
    bottom += String(st.fermSlopeDay, st.gravityPlato ? 1 : 3) + F("/g");
  }
  if (!bottom.length() && st.gravityHas && st.fermTrend == 3) bottom = F("densita' ferma da 48 h");
  if (!bottom.length() && st.gravityHas)
    bottom = String(F("letta ")) + durata(st.gravityAgeSec) + F(" fa");
  disp_->drawString(x, 51, bottom);
}

void OledUi::drawAllarmi(uint32_t mask) {
  const int x = dx_;
  disp_->setFont(ArialMT_Plain_10);
  const uint8_t n = contaAllarmi(mask);
  disp_->drawString(x, 0, String(F("ALLARMI (")) + String(n) + F(")"));
  uint8_t riga = 0;
  for (uint8_t i = 0; i < AL_COUNT && riga < 4; i++) {
    const uint8_t id = alarmByPriority(i);
    if (!(mask & (1UL << id))) continue;
    if (riga == 3 && n > 4) {
      disp_->drawString(x, 13 + riga * 12, String(F("... e altri ")) + String(n - 3));
      break;
    }
    disp_->drawString(x, 13 + riga * 12, String(F("! ")) + alarmOled(id));
    riga++;
  }
}
