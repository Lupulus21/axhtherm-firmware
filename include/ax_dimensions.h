// ax_dimensions.h — UNICA fonte del dimensionamento del sistema.
// Header volutamente minimale (solo <stdint.h>): lo includono sia la configurazione
// sia i driver, e non deve trascinare Arduino/OneWire dentro AppConfig.
#pragma once
#include <stdint.h>

// Numero di camere di fermentazione. Parametrico per costruzione: il codice non deve
// MAI assumere 1 né 2. A N=1 il comportamento è identico alla v2.23.2 (criterio di
// accettazione del refactor). Sovrascrivibile da build_flags: -DAX_N_CHAMBERS=2.
#ifndef AX_N_CHAMBERS
#define AX_N_CHAMBERS 1
#endif

namespace ax {

// Sonde per camera: 1 di controllo (mosto, pilota i relè) + 2 ausiliarie (camera,
// ambiente). Le ausiliarie servono al cross-check di plausibilità della sonda di
// controllo, che richiede >= 2 ausiliarie VALIDE per non degenerare.
constexpr uint8_t kProbesPerChamber = 3;
constexpr uint8_t kAuxPerChamber = kProbesPerChamber - 1;

// Sonde totali sul bus OneWire. Limite di FIRMWARE, non di hardware: il bus regge N
// dispositivi indirizzati per ROM. Dimensiona tutti gli array per-sonda di ProbeBus
// e di AppConfig, quindi ogni sonda in più costa ~121 B di RAM.
constexpr uint8_t kMaxProbes = (uint8_t)(AX_N_CHAMBERS * kProbesPerChamber);
// Il cast a uint8_t troncherebbe in silenzio oltre 85 camere, e gli indici sonda
// calcolati da assignDefaultProbes finirebbero fuori dagli array. Limite ampiamente
// teorico su ESP8266 (la RAM finirebbe molto prima), ma un troncamento silenzioso su
// un indice che pilota attuatori non va lasciato possibile.
static_assert(AX_N_CHAMBERS >= 1 && AX_N_CHAMBERS <= 8, "AX_N_CHAMBERS fuori range (1..8)");

// Uscite per camera: freddo + caldo.
constexpr uint8_t kOutputsPerChamber = 2;

// Sfasamento dell'istante in cui il riposo del compressore scade, per camera.
// Dopo un blackout tutte le camere chiamano assumeJustStopped() con lo STESSO epoch:
// senza sfasamento i loro riposi scadrebbero nello stesso identico secondo e i
// compressori partirebbero insieme, sommando gli spunti di corrente (5-8x la corrente
// di marcia) sulla stessa linea. 30 s sono largamente oltre la durata di uno spunto e
// del tutto trascurabili contro un riposo di 5 minuti.
constexpr uint32_t kCompressorStaggerSec = 30;

// Etichetta di default della sonda i. A camera singola restituisce esattamente
// "mosto"/"camera"/"ambiente", come la v2.23.2.
inline const char* defaultProbeRole(uint8_t i) {
  switch (i % kProbesPerChamber) {
    case 0: return "mosto";
    case 1: return "camera";
    default: return "ambiente";
  }
}

}  // namespace ax
