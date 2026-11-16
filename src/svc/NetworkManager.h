#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>

// STA con retry a backoff + SoftAP di soccorso dopo N minuti di STA giù.
// La STA continua a ritentare anche con l'AP attivo; niente di tutto questo
// blocca mai il loop di controllo.
class NetworkManager {
 public:
  // Nome e password EFFETTIVI della rete di configurazione: un'unica regola, usata da
  // begin(), dalla stampa delle credenziali e dall'OLED, cosi' quello che si legge e'
  // quello che funziona. L'SSID non supera i 32 caratteri del WiFi; una password sotto gli
  // 8 caratteri del WPA2 viene sostituita da quella di fabbrica.
  static String apSsidFor(const String& hostname) { return hostname.substring(0, 26) + F("-setup"); }
  static String apPassFor(const String& pass) { return pass.length() >= 8 ? pass : String(F("axhtherm25")); }

  void begin(const String& hostname, const String& ssid, const String& pass, bool dhcp,
             const String& ip, const String& mask, const String& gw, const String& dns,
             const String& apPassword, uint8_t apAfterMin);
  void loop(uint32_t nowMs);

  bool staConnected() const { return WiFi.status() == WL_CONNECTED; }
  bool apActive() const { return apActive_; }
  String ipString() const {
    if (staConnected()) return WiFi.localIP().toString();
    if (apActive_) return WiFi.softAPIP().toString();
    return String("-");
  }
  int rssi() const { return staConnected() ? WiFi.RSSI() : 0; }

 private:
  void startAp();
  void stopAp();

  String hostname_, ssid_, pass_, apPassword_;
  uint32_t apAfterMs_ = 300000;
  uint32_t staDownSinceMs_ = 0;
  uint32_t lastRetryMs_ = 0;
  uint32_t staUpSinceMs_ = 0;
  uint32_t lastCheckMs_ = 0;
  bool apActive_ = false;
  bool mdnsStarted_ = false;
  bool hasSta_ = false;
};
