#pragma once
#include <Arduino.h>
#include <Wire.h>

// Driver minimo della EEPROM AT24C32 (4 KB, pagine da 32 B) sul modulo Tiny RTC, a 0x50.
// Fino alla 3.13.0 non era usata; dalla 3.15.0 ospita il registro eventi (svc/EventLog).
// Indirizzo a 2 byte. Una scrittura non deve attraversare un confine di pagina: chi chiama
// scrive blocchi allineati (i record dell'EventLog sono da 16 B, allineati a 16).
// Dopo ogni scrittura il chip resta occupato fino a 5 ms (ciclo interno) e non risponde:
// si aspetta con l'ACK polling, al massimo kWriteWaitMs.
class At24c32 {
 public:
  static constexpr uint16_t kSize = 4096;
  static constexpr uint8_t kPage = 32;
  static constexpr uint32_t kWriteWaitMs = 10;

  bool begin(uint8_t addr) {
    addr_ = addr;
    Wire.beginTransmission(addr_);
    present_ = Wire.endTransmission() == 0;
    return present_;
  }
  bool present() const { return present_; }

  // Legge len byte da mem (a pezzi da 32: il buffer di Wire e' da 128, si resta larghi).
  bool read(uint16_t mem, uint8_t* buf, uint16_t len) {
    if (!present_ || mem + len > kSize) return false;
    while (len) {
      const uint8_t n = len > 32 ? 32 : (uint8_t)len;
      Wire.beginTransmission(addr_);
      Wire.write((uint8_t)(mem >> 8));
      Wire.write((uint8_t)(mem & 0xFF));
      if (Wire.endTransmission(false) != 0) return false;
      if (Wire.requestFrom(addr_, n) != n) return false;
      for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
      buf += n;
      mem += n;
      len -= n;
    }
    return true;
  }

  // Scrive len byte (<= 32) a mem, senza attraversare un confine di pagina.
  bool writePage(uint16_t mem, const uint8_t* buf, uint8_t len) {
    if (!present_ || len == 0 || len > kPage || mem + len > kSize) return false;
    if ((mem / kPage) != ((mem + len - 1) / kPage)) return false;  // confine di pagina
    Wire.beginTransmission(addr_);
    Wire.write((uint8_t)(mem >> 8));
    Wire.write((uint8_t)(mem & 0xFF));
    Wire.write(buf, len);
    if (Wire.endTransmission() != 0) return false;
    // ACK polling: il chip torna a rispondere a fine ciclo di scrittura.
    const uint32_t t0 = millis();
    while (millis() - t0 < kWriteWaitMs) {
      Wire.beginTransmission(addr_);
      if (Wire.endTransmission() == 0) return true;
      delayMicroseconds(500);
    }
    return false;
  }

 private:
  uint8_t addr_ = 0x50;
  bool present_ = false;
};
