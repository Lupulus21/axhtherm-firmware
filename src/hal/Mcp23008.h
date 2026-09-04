#pragma once
#include <Arduino.h>
#include <Wire.h>

// Driver minimo MCP23008: solo GP0..GP4 come uscite (REL4..REL8 su ULN I4..I8).
// GP5..GP7 (IOE1..3 su P1) restano ingressi: non ci appartengono.
class Mcp23008 {
 public:
  static constexpr uint8_t REG_IODIR = 0x00;
  static constexpr uint8_t REG_GPIO = 0x09;
  static constexpr uint8_t REG_OLAT = 0x0A;

  bool begin(uint8_t addr) {
    addr_ = addr;
    olat_ = 0x00;
    present_ = write8(REG_OLAT, 0x00) && write8(REG_IODIR, 0xE0);  // GP0..4 out, GP5..7 in
    return present_;
  }

  bool present() const { return present_; }

  // pin 0..4; uscite spente = livello basso (ULN non invertente).
  // Su NACK tenta subito un recovery (begin() riparte da OLAT=0: tutto spento, stato
  // sicuro) e riscrive; se anche il retry fallisce present_ resta falso e ci pensa
  // il retry periodico di OutputBus::loop() + l'allarme.
  bool setPin(uint8_t pin, bool on) {
    if (pin > 4) return false;
    if (!present_ && !recover()) return false;
    uint8_t v = olat_;
    if (on) v |= (1u << pin);
    else v &= ~(1u << pin);
    if (v == olat_) return true;
    if (!write8(REG_OLAT, v)) {
      present_ = false;
      if (!recover() || !write8(REG_OLAT, v)) {
        present_ = false;
        return false;
      }
    }
    olat_ = v;
    return true;
  }

  bool allOff() {
    if (!present_) return false;
    olat_ = 0x00;
    if (!write8(REG_OLAT, 0x00)) { present_ = false; return false; }
    return true;
  }

  // Riaggancio dopo un errore I2C transitorio.
  bool recover() { return begin(addr_); }

 private:
  bool write8(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr_);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
  }
  uint8_t addr_ = 0x20;
  uint8_t olat_ = 0;
  bool present_ = false;
};
