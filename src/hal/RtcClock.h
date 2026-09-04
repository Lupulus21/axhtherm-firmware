#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "hardware_map.h"
#include "ax/TimeUtil.h"

// DS1307 via Wire, senza librerie esterne: BCD nei registri 0..6, bit CH (clock halt)
// nel bit 7 del registro 0. Tutte le epoch sono UTC.
class RtcClock {
 public:
  bool begin() {
    Wire.beginTransmission(I2C_ADDR_DS1307);
    present_ = Wire.endTransmission() == 0;
    return present_;
  }

  bool present() const { return present_; }

  // Ritorna false se assente, fermo (CH=1) o con data implausibile.
  bool readEpoch(uint32_t* epoch, bool* halted = nullptr) {
    uint8_t r[7];
    if (!readRegs(0x00, r, 7)) return false;
    bool ch = (r[0] & 0x80) != 0;
    if (halted) *halted = ch;
    if (ch) return false;
    unsigned sec = bcd(r[0] & 0x7F), min = bcd(r[1]);
    uint8_t hourReg = r[2];
    unsigned hour;
    if (hourReg & 0x40) {  // modo 12h: convertiamo (non dovrebbe accadere, ma il chip lo consente)
      hour = bcd(hourReg & 0x1F) % 12 + ((hourReg & 0x20) ? 12 : 0);
    } else {
      hour = bcd(hourReg & 0x3F);
    }
    unsigned day = bcd(r[4] & 0x3F), month = bcd(r[5] & 0x1F);
    int year = 2000 + (int)bcd(r[6]);
    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12)
      return false;
    if (year < 2020 || year > 2099) return false;  // data implausibile: batteria andata
    *epoch = ax::epochFromYmdHms(year, month, day, hour, min, sec);
    return true;
  }

  bool writeEpoch(uint32_t epoch) {
    int y;
    unsigned mo, d, h, mi, s;
    ax::ymdHmsFromEpoch(epoch, &y, &mo, &d, &h, &mi, &s);
    if (y < 2000 || y > 2099) return false;
    uint8_t r[7];
    r[0] = toBcd(s);  // CH=0: orologio in marcia
    r[1] = toBcd(mi);
    r[2] = toBcd(h);  // modo 24h
    r[3] = 1;         // giorno della settimana: non usato
    r[4] = toBcd(d);
    r[5] = toBcd(mo);
    r[6] = toBcd((unsigned)(y - 2000));
    Wire.beginTransmission(I2C_ADDR_DS1307);
    Wire.write((uint8_t)0x00);
    Wire.write(r, 7);
    return Wire.endTransmission() == 0;
  }

 private:
  static unsigned bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
  static uint8_t toBcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

  bool readRegs(uint8_t reg, uint8_t* out, uint8_t n) {
    Wire.beginTransmission(I2C_ADDR_DS1307);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((int)I2C_ADDR_DS1307, (int)n) != n) return false;
    for (uint8_t i = 0; i < n; i++) out[i] = Wire.read();
    return true;
  }

  bool present_ = false;
};
