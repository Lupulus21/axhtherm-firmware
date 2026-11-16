#include "svc/NetworkManager.h"
#include <ESP8266mDNS.h>

void NetworkManager::begin(const String& hostname, const String& ssid, const String& pass,
                           bool dhcp, const String& ip, const String& mask, const String& gw,
                           const String& dns, const String& apPassword, uint8_t apAfterMin) {
  hostname_ = hostname;
  ssid_ = ssid;
  pass_ = pass;
  apPassword_ = apPassFor(apPassword);
  apAfterMs_ = (uint32_t)apAfterMin * 60000UL;
  hasSta_ = ssid_.length() > 0;

  WiFi.persistent(false);  // le credenziali le gestiamo noi su LittleFS
  WiFi.setAutoReconnect(true);
  WiFi.hostname(hostname_);

  if (hasSta_) {
    WiFi.mode(WIFI_STA);
    if (!dhcp && ip.length()) {
      IPAddress i, m, g, d;
      if (i.fromString(ip) && m.fromString(mask) && g.fromString(gw)) {
        if (!d.fromString(dns)) d = g;
        WiFi.config(i, g, m, d);
      }
    }
    WiFi.begin(ssid_, pass_);
    staDownSinceMs_ = millis();
  } else {
    // Nessuna rete configurata: SoftAP subito, è la prima configurazione.
    WiFi.mode(WIFI_AP);
    startAp();
  }
}

void NetworkManager::startAp() {
  if (apActive_) return;
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  String apSsid = apSsidFor(hostname_);
  apActive_ = WiFi.softAP(apSsid, apPassword_);
  if (apActive_) Serial.printf_P(PSTR("[net] SoftAP attivo: %s / 192.168.4.1\n"), apSsid.c_str());
  else Serial.println(F("[net] ERRORE avvio SoftAP"));
}

void NetworkManager::stopAp() {
  if (!apActive_) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive_ = false;
  Serial.println(F("[net] SoftAP spento (STA connessa)"));
}

void NetworkManager::loop(uint32_t nowMs) {
  if (nowMs - lastCheckMs_ < 1000) {
    if (mdnsStarted_) MDNS.update();
    return;
  }
  lastCheckMs_ = nowMs;

  bool sta = staConnected();
  if (sta) {
    if (staUpSinceMs_ == 0) {
      staUpSinceMs_ = nowMs;
      Serial.printf_P(PSTR("[net] STA connessa: %s\n"), WiFi.localIP().toString().c_str());
      if (!mdnsStarted_) {
        if (MDNS.begin(hostname_)) {
          MDNS.addService("http", "tcp", 80);
          mdnsStarted_ = true;
        }
      }
    }
    staDownSinceMs_ = 0;
    // AP di soccorso ancora su e nessuno collegato da 60 s di STA stabile: si spegne.
    if (apActive_ && hasSta_ && nowMs - staUpSinceMs_ > 60000 && WiFi.softAPgetStationNum() == 0) {
      stopAp();
    }
  } else {
    staUpSinceMs_ = 0;
    if (hasSta_) {
      if (staDownSinceMs_ == 0) staDownSinceMs_ = nowMs;
      if (nowMs - lastRetryMs_ > 30000) {
        lastRetryMs_ = nowMs;
        WiFi.reconnect();
      }
      if (!apActive_ && nowMs - staDownSinceMs_ > apAfterMs_) {
        WiFi.mode(WIFI_AP_STA);
        startAp();
        WiFi.begin(ssid_, pass_);  // la STA continua a ritentare sotto l'AP
      }
    }
  }
  if (mdnsStarted_) MDNS.update();
}
