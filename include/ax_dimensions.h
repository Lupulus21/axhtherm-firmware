// ax_dimensions.h — UNICA fonte del dimensionamento del sistema.
// Header volutamente minimale (solo <stdint.h>): lo includono sia la configurazione
// sia i driver, e non deve trascinare Arduino/OneWire dentro AppConfig.
#pragma once
#include <stdint.h>

// Numero di camere di fermentazione: UNA. Dalla 3.1.0 il firmware gestisce una sola
// camera per scheda; i percorsi per più camere della 3.0.0 sono stati rimossi (restano
// nel tag git v3.0.0). Gli array per camera (ChamberCtx, mapping delle uscite, stato
// persistito) restano dimensionati su questa costante, che quindi non va cambiata.
#define AX_N_CHAMBERS 1

namespace ax {

// Sonde: 1 di controllo (mosto, pilota i relè) + 2 ausiliarie (camera, ambiente).
// Le ausiliarie servono al cross-check di plausibilità della sonda di controllo, che
// richiede >= 2 ausiliarie VALIDE per non degenerare.
constexpr uint8_t kProbesPerChamber = 3;
constexpr uint8_t kAuxPerChamber = kProbesPerChamber - 1;

// Sonde totali sul bus OneWire. Limite di FIRMWARE, non di hardware: il bus regge N
// dispositivi indirizzati per ROM. Dimensiona tutti gli array per-sonda di ProbeBus
// e di AppConfig, quindi ogni sonda in più costa ~121 B di RAM.
constexpr uint8_t kMaxProbes = (uint8_t)(AX_N_CHAMBERS * kProbesPerChamber);
static_assert(AX_N_CHAMBERS == 1, "il firmware gestisce una sola camera dalla 3.1.0");

// Etichetta di default della sonda i: "mosto"/"camera"/"ambiente".
inline const char* defaultProbeRole(uint8_t i) {
  switch (i % kProbesPerChamber) {
    case 0: return "mosto";
    case 1: return "camera";
    default: return "ambiente";
  }
}

}  // namespace ax
