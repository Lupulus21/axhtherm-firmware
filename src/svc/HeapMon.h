#pragma once
#include <Arduino.h>
#include <umm_malloc/umm_malloc.h>

// Minimi di memoria dall'avvio: sono loro, non il valore istantaneo, a dire quanto
// margine c'e' davvero. Un esaurimento lento o un picco dentro un handler web si vedono
// qui prima che diventino un crash.
// - freeMin: minimo storico dell'heap libero, misurato dall'allocatore a OGNI allocazione
//   (UMM_STATS_FULL in platformio.ini), quindi cattura anche i picchi dentro gli handler.
// - maxBlockMin: minimo del blocco contiguo piu' grande, campionato una volta al secondo:
//   e' il numero che decide se un documento JSON grande potra' essere allocato.
// - stackFreeMin: stack libero minimo del loop (lo stack "cont" da 4 KB, verniciato dal core).
// - oom: allocazioni FALLITE dall'avvio. Diverso da zero = qualcosa e' gia' andato storto.
struct HeapMon {
  uint32_t maxBlockMin = UINT32_MAX;

  void sample() {
    const uint32_t b = ESP.getMaxFreeBlockSize();
    if (b < maxBlockMin) maxBlockMin = b;
  }
  static uint32_t freeMin() { return umm_free_heap_size_min(); }
  static uint32_t oom() { return umm_get_oom_count(); }
  static uint32_t stackFreeMin() { return ESP.getFreeContStack(); }
};
