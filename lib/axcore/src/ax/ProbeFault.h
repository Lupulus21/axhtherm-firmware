#pragma once
#include <stdint.h>

namespace ax {

// Perche' una lettura della DS18B20 e' fallita. La libreria restituisce -127 per qualunque
// guasto; rileggendo lo scratchpad (i 9 byte che la sonda espone) si capisce di che tipo e',
// e il tipo dice dove guardare. Il bus e' UNICO per tutte le sonde: al reset basta che ne
// risponda una perche' ci sia presenza.
enum class ProbeFault : uint8_t {
  None = 0,      // dati integri e plausibili: il guasto era transitorio (lettura recuperata)
  NoResponse,    // nessuna sonda risponde al reset: linea dati a massa, bus interrotto vicino
                 // alla scheda, pull-up mancante
  AllOnes,       // tutti 0xFF: QUESTA sonda non trasmette (staccata, cavo o connettore suo)
  AllZeros,      // tutti 0x00: linea a massa durante la lettura (corto intermittente)
  Crc,           // dati corrotti: disturbi, cavo lungo, pull-up debole
  Reset,         // 85 °C di accensione: la sonda si e' riavviata (alimentazione instabile)
  OutOfRange,    // dati integri ma temperatura fuori dalla scala plausibile
};
static constexpr uint8_t kProbeFaultKinds = 7;

// CRC-8 Dallas/Maxim (polinomio x^8+x^5+x^4+1, riflesso), come OneWire::crc8.
inline uint8_t dallasCrc8(const uint8_t* d, uint8_t len) {
  uint8_t crc = 0;
  while (len--) {
    uint8_t b = *d++;
    for (uint8_t i = 0; i < 8; i++) {
      const uint8_t mix = (crc ^ b) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      b >>= 1;
    }
  }
  return crc;
}

// Famiglie con il formato di temperatura della DS18B20 (valore/16, registro di configurazione).
inline bool isB20Family(uint8_t family) { return family == 0x28 || family == 0x22; }

// Classifica uno scratchpad letto (presence = qualche sonda ha risposto al reset). Per le
// famiglie DS18B20 (0x28) e DS1822 (0x22) scrive in *tempC la temperatura quando il
// risultato e' None; per le altre (DS18S20, e 0x3B che puo' essere una MAX31850 con il suo
// bit di guasto) *tempC non viene toccato e None vuol dire solo "dati integri".
inline ProbeFault classifyScratchpad(bool presence, const uint8_t sp[9], uint8_t family,
                                     float minC, float maxC, float* tempC) {
  if (!presence) return ProbeFault::NoResponse;
  bool ones = true, zeros = true;
  for (uint8_t i = 0; i < 9; i++) {
    if (sp[i] != 0xFF) ones = false;
    if (sp[i] != 0x00) zeros = false;
  }
  if (ones) return ProbeFault::AllOnes;
  if (zeros) return ProbeFault::AllZeros;  // prima del CRC: otto zeri hanno CRC zero
  if (dallasCrc8(sp, 8) != sp[8]) return ProbeFault::Crc;
  if (!isB20Family(family)) return ProbeFault::None;
  int16_t raw = (int16_t)(((uint16_t)sp[1] << 8) | sp[0]);
  if (raw == 0x0550) return ProbeFault::Reset;  // 85,0 °C: valore di accensione
  // Bit non significativi alle risoluzioni minori (registro di configurazione, byte 4).
  switch ((sp[4] >> 5) & 0x03) {
    case 0: raw &= ~7; break;
    case 1: raw &= ~3; break;
    case 2: raw &= ~1; break;
    default: break;
  }
  const float t = raw / 16.0f;
  if (!(t > minC && t < maxC)) return ProbeFault::OutOfRange;
  if (tempC) *tempC = t;
  return ProbeFault::None;
}

}  // namespace ax
