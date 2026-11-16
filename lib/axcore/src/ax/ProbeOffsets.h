#pragma once
#include <stdint.h>

namespace ax {

// La taratura appartiene alla sonda FISICA, non alla posizione. Dopo un salvataggio, per ogni
// posizione in cui la sonda e' cambiata e la correzione non e' stata toccata (newOff uguale
// a quella di prima), la correzione segue la sonda: quella che aveva in un'altra posizione,
// oppure 0 per una sonda nuova. moved[i] = true dove e' cambiata; ritorna quante.
// S e' il tipo di stringa (String sulla scheda, std::string nei test); same(a, b) confronta
// due ID senza badare alle maiuscole, empty(a) dice se la posizione e' vuota.
template <typename S, typename Same, typename Empty>
uint8_t followProbeOffsets(const S* oldId, const float* oldOff, const S* newId, float* newOff,
                           uint8_t n, Same same, Empty empty, bool* moved) {
  uint8_t k = 0;
  for (uint8_t i = 0; i < n; i++) {
    moved[i] = false;
    if (same(newId[i], oldId[i])) continue;
    if (newOff[i] != oldOff[i]) continue;  // impostata ora dall'utente: si rispetta
    float segue = 0.0f;
    for (uint8_t j = 0; j < n; j++)
      if (!empty(newId[i]) && same(newId[i], oldId[j])) segue = oldOff[j];
    if (segue == newOff[i]) continue;
    newOff[i] = segue;
    moved[i] = true;
    k++;
  }
  return k;
}

}  // namespace ax
