#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ax {

// Verifica di un firmware.bin ricevuto via rete, a flusso, mentre arriva. Lo strumento di
// compilazione del core (tools/elf2bin.py) scrive dentro l'immagine la sua lunghezza totale
// e un CRC32 dell'intero file, calcolato con quei due campi a zero: sono gli stessi che il
// core usa per controllare il firmware in esecuzione (ESP.checkFlashCRC). Un file troncato,
// anche gia' sul disco prima dell'invio, ha una lunghezza diversa da quella dichiarata; un
// file danneggiato ha un CRC diverso. In entrambi i casi l'aggiornamento non si conferma e
// resta il firmware attuale.
class FwImageCheck {
 public:
  static constexpr uint32_t kAppOff = 0x1000;            // intestazione dell'applicazione
  static constexpr uint32_t kLenOff = kAppOff + 16;      // elf2bin.py: crcsize_offset
  static constexpr uint32_t kValOff = kLenOff + 4;       // elf2bin.py: crcval_offset

  enum class Result : uint8_t { Ok, NotImage, Short, Long, Crc };

  void begin() {
    n_ = 0;
    crc_ = 0xFFFFFFFFu;
    len_ = val_ = 0;
    magic0_ = magicApp_ = 0;
  }

  void feed(const uint8_t* d, size_t k) {
    for (size_t i = 0; i < k; i++, n_++) {
      uint8_t b = d[i];
      if (n_ == 0) magic0_ = b;
      if (n_ == kAppOff) magicApp_ = b;
      if (n_ >= kLenOff && n_ < kValOff + 4) {
        const uint32_t sh = ((n_ - kLenOff) & 3) * 8;
        if (n_ < kValOff) len_ |= (uint32_t)b << sh;
        else val_ |= (uint32_t)b << sh;
        b = 0;  // nel CRC i due campi valgono zero
      }
      crc_ = step(crc_, b);
    }
  }

  Result result() const {
    if (n_ < kValOff + 4 || magic0_ != 0xE9 || magicApp_ != 0xE9 || len_ == 0)
      return Result::NotImage;
    if (n_ < len_) return Result::Short;
    if (n_ > len_) return Result::Long;
    return crc_ == val_ ? Result::Ok : Result::Crc;
  }
  uint32_t received() const { return n_; }
  uint32_t declared() const { return len_; }

 private:
  // CRC32 come elf2bin.py (crc8266) ed eboot: polinomio 0x04C11DB7, MSB per primo,
  // valore iniziale 0xFFFFFFFF, nessuna inversione finale.
  static uint32_t step(uint32_t crc, uint8_t byte) {
    for (uint8_t m = 0x80; m; m >>= 1) {
      bool bit = crc & 0x80000000u;
      if (byte & m) bit = !bit;
      crc <<= 1;
      if (bit) crc ^= 0x04C11DB7u;
    }
    return crc;
  }

  uint32_t n_ = 0, crc_ = 0xFFFFFFFFu, len_ = 0, val_ = 0;
  uint8_t magic0_ = 0, magicApp_ = 0;
};

}  // namespace ax
