#pragma once
#include <stdint.h>
#include <string.h>

namespace ax {

// Registro eventi ad anello su una memoria non volatile a pagine (3.15.0: la EEPROM AT24C32
// del Tiny RTC). Niente intestazione da riscrivere a ogni evento: ogni record porta un numero
// di sequenza, e all'avvio si scorrono tutti gli slot per trovare il piu' recente. Cosi'
// ogni cella viene riscritta una sola volta ogni kSlots eventi (usura distribuita).
//
// Un record vale solo con CRC giusto e sequenza diversa da 0 e da 0xFFFFFFFF (EEPROM vergine
// o con contenuti estranei). In lettura si accettano solo record con sequenza consecutiva
// a partire dal piu' recente: un vecchio record sopravvissuto a un buco non si mescola.
//
// Store: oggetto con
//   bool read(uint16_t mem, uint8_t* buf, uint16_t len);
//   bool writePage(uint16_t mem, const uint8_t* buf, uint8_t len);
static constexpr uint8_t kEventMagic = 0xA5;
struct __attribute__((packed)) EventRec {
  uint32_t seq;
  uint32_t epoch;  // 0 = ora non ancora nota
  uint8_t code;
  uint8_t a;
  uint16_t b;
  int16_t v;
  uint8_t magic;  // kMagic: con il CRC, una EEPROM con contenuti casuali sembra valida
                  // in un caso su 65.536 per slot invece di uno su 256
  uint8_t crc;    // CRC-8 (poly 0x07) dei primi 15 byte
};
static_assert(sizeof(EventRec) == 16, "record evento = 16 byte");

inline uint8_t crc8(const uint8_t* p, uint8_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= *p++;
    for (uint8_t i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

template <class Store, uint16_t kSlots = 256>
class EventRing {
 public:
  static constexpr uint16_t kRec = sizeof(EventRec);

  // Scorre tutti gli slot. Ritorna false se la memoria non si legge.
  bool begin(Store* st) {
    st_ = st;
    ok_ = false;
    head_ = 0;
    lastSeq_ = 0;
    uint16_t lastSlot = kSlots - 1;
    EventRec r[2];
    for (uint16_t s = 0; s < kSlots; s += 2) {
      if (!st_->read((uint16_t)(s * kRec), (uint8_t*)r, 2 * kRec)) return false;
      for (uint8_t k = 0; k < 2; k++) {
        if (valid(r[k]) && r[k].seq > lastSeq_) {
          lastSeq_ = r[k].seq;
          lastSlot = s + k;
        }
      }
    }
    head_ = lastSeq_ ? (uint16_t)((lastSlot + 1) % kSlots) : 0;
    ok_ = true;
    return true;
  }

  bool ok() const { return ok_; }
  uint32_t lastSeq() const { return lastSeq_; }

  bool add(uint32_t epoch, uint8_t code, uint8_t a, uint16_t b, int16_t v) {
    if (!ok_) return false;
    EventRec r;
    memset(&r, 0, sizeof(r));
    r.seq = lastSeq_ + 1;
    r.epoch = epoch;
    r.code = code;
    r.a = a;
    r.b = b;
    r.v = v;
    r.magic = kEventMagic;
    r.crc = crc8((const uint8_t*)&r, kRec - 1);
    if (!st_->writePage((uint16_t)(head_ * kRec), (const uint8_t*)&r, (uint8_t)kRec)) return false;
    lastSeq_ = r.seq;
    head_ = (uint16_t)((head_ + 1) % kSlots);
    return true;
  }

  // Record con sequenza 'seq' (deve stare negli ultimi kSlots). False se non c'e' o non vale.
  bool readSeq(uint32_t seq, EventRec& out) {
    if (!ok_ || seq == 0 || seq > lastSeq_ || lastSeq_ - seq >= kSlots) return false;
    const uint32_t back = lastSeq_ - seq;  // 0 = il piu' recente
    const uint16_t slot = (uint16_t)((head_ + kSlots - 1 - (back % kSlots)) % kSlots);
    if (!st_->read((uint16_t)(slot * kRec), (uint8_t*)&out, kRec)) return false;
    return valid(out) && out.seq == seq;
  }

 private:
  static bool valid(const EventRec& r) {
    if (r.seq == 0 || r.seq == 0xFFFFFFFFUL || r.magic != kEventMagic) return false;
    return crc8((const uint8_t*)&r, kRec - 1) == r.crc;
  }
  Store* st_ = nullptr;
  bool ok_ = false;
  uint16_t head_ = 0;     // prossimo slot da scrivere
  uint32_t lastSeq_ = 0;  // 0 = registro vuoto
};

// Allarmi nel registro senza farsi riempire da un allarme che va e viene (3.15.0): un
// cambiamento si registra solo quando la maschera resta diversa da quella gia' registrata
// per holdMs di fila. Un'oscillazione piu' rapida non scrive nulla.
class AlarmDebounce {
 public:
  explicit AlarmDebounce(uint32_t holdMs = 30000) : holdMs_(holdMs) {}
  // Ritorna true quando c'e' da registrare: on = allarmi accesi, off = allarmi spenti
  // rispetto all'ultima registrazione.
  bool tick(uint32_t mask, uint32_t nowMs, uint32_t& on, uint32_t& off) {
    on = off = 0;
    if (mask == logged_) {
      pending_ = false;
      return false;
    }
    if (!pending_) {
      pending_ = true;
      sinceMs_ = nowMs;
      return false;
    }
    if (nowMs - sinceMs_ < holdMs_) return false;
    on = mask & ~logged_;
    off = logged_ & ~mask;
    logged_ = mask;
    pending_ = false;
    return true;
  }
  uint32_t logged() const { return logged_; }

 private:
  uint32_t holdMs_;
  uint32_t logged_ = 0;
  uint32_t sinceMs_ = 0;
  bool pending_ = false;
};

}  // namespace ax
