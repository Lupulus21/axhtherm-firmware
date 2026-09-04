#pragma once
#include <Arduino.h>
#include <LittleFS.h>

// Promozione atomica di un file temporaneo su LittleFS con VERIFICA della dimensione committata.
// File::close() è void e SCARTA l'esito di lfs_file_close: un fallimento del flush finale (FS piena
// all'ultimo blocco, o errore program/erase su flash usurata) lascerebbe soddisfatta la guardia
// "byte scritti == attesi" (i byte erano solo nella cache LittleFS) ma il file su flash TRONCATO,
// e il rename lo promuoverebbe sopra quello buono → al boot il load fallisce → stato/config/profilo
// persi in silenzio. Ri-aprendo in lettura si legge la dimensione REALE su flash: se non combacia
// con quella attesa, si scarta il tmp senza promuoverlo (chi chiama tratta false come "save fallito").
inline bool fsCommitAtomic(const char* tmpPath, const char* path, size_t want) {
  // La ri-apertura alloca (FileImpl + cache LittleFS): su heap sottile può FALLIRE. Scelta FAIL-SAFE
  // nel dubbio: se NON si può verificare, NON si promuove (il file buono resta intatto) e si ritorna
  // false → allarme storage / 500 al client, RITENTABILE. Promuovere un tmp non verificato rischierebbe
  // di sovrascrivere il file buono con uno troncato in SILENZIO (corner OOM + FS piena all'ultimo
  // blocco): su una cotta non presidiata la corruzione silenziosa è il male peggiore, un save fallito
  // e ritentabile no. Il falso fallimento è comunque quasi impossibile: il chiamante ha appena
  // allocato un doc da alcuni KB, quindi questa ri-apertura da poche centinaia di byte ha spazio.
  File v = LittleFS.open(tmpPath, "r");
  if (!v) return false;  // non verificabile: NON promuovere, NON cancellare (il prossimo save riscrive il tmp)
  size_t got = v.size();
  v.close();
  if (got != want) {  // dimensione su flash < attesa: flush finale fallito → tmp troncato: scarta
    LittleFS.remove(tmpPath);
    return false;
  }
  return LittleFS.rename(tmpPath, path);  // verificato integro su flash: promozione atomica
}
