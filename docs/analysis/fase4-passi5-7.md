# FASE 4 — Verifica avversariale dei passi 5-7

Campagna del 2026-08-31 sui commit `69a8251`…`7e1266b` (persistenza + migrazione, API per
camera, OLED/MQTT/Home Assistant).

**Metodo:** 5 lenti avversariali indipendenti → **~50 rilievi grezzi**, deduplicati per
`file:riga` prima della confutazione. La campagna precedente aveva sprecato 16 slot di verifica
su 23 in doppioni: qui il dedup viene fatto **prima**.

Ogni rilievo corretto qui sotto è stato **verificato di persona leggendo il codice** (per il
CRITICO, leggendo il sorgente del framework Arduino), non accettato sulla parola.

## Esito della confutazione — e un limite del metodo

10 verdetti raccolti sui rilievi deduplicati: **2 confermati, 8 "non reali"**. Quel rapporto
sarebbe però **fuorviante se letto come 8 falsi allarmi**, e i verdetti stessi lo dicono:

> «il meccanismo descritto è tecnicamente corretto, ma la precondizione **non esiste più** nel
> codice attuale: il difetto è già stato corretto nel commit `88f34ea`»
> — «rilievo **STALE**: descrive il codice al commit `7e1266b`, non l'albero in revisione»
> — «il tetto **ESISTE**, subito sotto le righe che il rilievo cita»

Le 8 confutazioni corrispondono a **4 difetti che avevo già corretto** mentre la campagna era in
corso, ciascuno esaminato da 2 scettici contro l'albero **già sistemato**. È un artefatto del
metodo: correggere durante la campagna trasforma la confutazione da «era reale?» a «c'è ancora?».
Di riflesso però conferma in modo indipendente che le correzioni **funzionano**, perché gli
scettici sono andati a cercare il difetto e hanno trovato la guardia.

**I 2 confermati sono esattamente i due punti che avevo scelto di documentare invece di
correggere** (cattura OG sulla sola camera 1, e il formato di `/state.json` che a N=1 non è più
quello della v2.23.2). Nessun difetto nuovo da correggere.

*Lezione per la prossima campagna: congelare l'albero, oppure far verificare gli scettici contro
il commit su cui il rilievo è stato sollevato, non contro HEAD.*

## Rilievi corretti

| ID | Gravità | Dove | Descrizione | Innesco | Commit |
|---|---|---|---|---|---|
| G1 | **CRITICO** | `WebApi.cpp` `selectChamber` | **Riavvio del controllore da una richiesta non autenticata.** `UriBraces::canHandle` chiama **prima** `Uri::canHandle`, che fa un confronto di stringhe **esatto** (`_uri == requestUri`, `Uri.h:23`) e in caso di match ritorna `true` **senza popolare `pathArgs`** | Una `GET /api/ch/{}/status` (percorso **letterale**, con le graffe) combacia esattamente → `pathArgs` vuoto → `pathArg(0)` esegue `assert(0 < 0)`. `NDEBUG` non è definito in questo build: **panic e riavvio**, e un riavvio durante una fermentazione fa cadere i relè | `88f34ea` |
| G1b | **CRITICO** *(preesistente)* | `WebApi.cpp` `handleProfileById` | Stessa trappola su `/api/profiles/{}`. **Non introdotta dal refactor**: c'era già nella baseline | idem | `88f34ea` |
| G2 | **ALTO** | `main.cpp` gestore MQTT | `chambers[ch].curStatus = buildStatus(chambers[0])`: dopo un comando MQTT sulla camera 2, il suo stato pubblicato era quello della camera 1. La mia sostituzione del passo 6 cercava `chambers[0].` col punto e non ha colpito l'argomento di funzione | Comando MQTT su una camera ≠ 1 | `88f34ea` |
| G3 | **ALTO** | `main.cpp` `buildStatus`/`controlTick` | Cinque letture di `pstate` cablate sulla camera 0 **dentro funzioni per camera**: `batchName`, `ogGravity` e l'**attenuazione apparente**. Non è solo visualizzazione: l'attenuazione fa **avanzare gli step a densità** | N=2 con profilo a densità sulla camera 2 | `88f34ea` |
| G4 | **ALTO** | `main.cpp` boot | Il ripristino del mapping relè era rimasto **fuori** dal ciclo per camera, benché il passo 5 avesse reso `/state.json` per camera: dopo un riavvio la camera 2 restava **senza uscite** | Riavvio con N=2 | `88f34ea` |
| G5 | **ALTO** | `WebApi.cpp` `handlePutConfig` | La guardia sull'unità di densità interrogava `c_()`, ma `/api/config` è di **dispositivo** | Cambio unità mentre la camera 2 esegue un profilo a densità | `88f34ea` |
| G6 | **ALTO** | `ConfigStore.cpp` | `chamberLabel` era l'unica stringa di config **senza tetto**; finisce in `/config.json` (doc da 6144 su ~17 KB), `/api/status` e nei nomi delle entità HA | Etichetta lunghissima → traboccamento | `88f34ea` |
| G7 | **ALTO** | `WebApi.cpp` `handleStatus` | I **valori** delle temperature venivano dalla camera servita, ma `label` e `configured` da `probeLabel[i]` con `i` = 0,1,2 **fissi** | N=2: le temperature della camera 2 etichettate con le sonde della camera 1 | `2a8f…` |
| G8 | **ALTO** | `StateStore` | Uno `/state.json` che descrive **più** camere di quante il firmware ne gestisce vedeva quelle in eccesso scartate **in silenzio** | Riportare indietro il firmware dopo aver girato a N=2 | `dd23b9d` |
| G13 | **ALTO** | `StateStore::load` | **La capacità di lettura era dimensionata sul FIRMWARE, non sul FILE.** `deserializeJson` materializza l'intero documento nel pool *prima* che il ciclo possa limitarsi ad `AX_N_CHAMBERS`, quindi un file con più camere falliva con `NoMemory` → `load()` false → indistinguibile da uno `/state.json` corrotto. Rendeva **irraggiungibile** la gestione di `excessChambers_` (G8), che esiste proprio per quel caso | Downgrade da N=2 a N=1 con cotte in corso. **La soglia dipende dalla lunghezza del nome della cotta**: capacità nominale 1344 B, un file a 2 camere ne costa 1293 + 1 B per carattere, quindi rompe oltre ~51 caratteri complessivi (nomi capped a 40 ciascuno). Tutte le camere ripartono Off/disarmate: step, OG, epoch della guardia e stimatori appresi persi | `(questo commit)` |
| G9 | **MEDIO** | `DataLog::cleanup` | **Il tetto di dimensione poteva disabilitarsi per sempre.** Contava *qualsiasi* `/log/d*.bin` ma cancellava ricostruendo i nomi da `AX_N_CHAMBERS`: con un file orfano i due insiemi divergono, `oldest` resta fermo, la `remove` fallisce e il ciclo esce — **a ogni invocazione successiva**. Il registro cresce fino a riempire la LittleFS, e allora falliscono anche i salvataggi di config e stato | **È il flusso imposto da questa campagna**: girare a N=2, tornare a N=1, con RTC inaffidabile (dove la retention per giorni non cancella nulla — il caso per cui il tetto esiste) | `7f0e90c` |
| G10 | **MEDIO** | `WebApi.cpp` | **Costo di heap permanente delle rotte doppie.** Ogni `server_.on(Uri,…)` alloca handler + `Uri` clonato + `String`; la lambda cattura una `std::function`. ~120-200 B × 16 rotte ≈ **2-3 KB**, l'11-17 % dell'heap libero | A N=1 comprava solo la simmetria di `/api/ch/1/…` | `f790b7f` |
| G11 | **BASSO** | `ConfigStore.cpp` | `chamberLabel` troncata per **byte** da `cap()`, mentre `probeLabel` — dieci righe sopra — arretra all'inizio del carattere UTF-8 | Etichetta con accenti → carattere invalido nel JSON | `dd23b9d` |
| G12 | **BASSO** | `WebApi.cpp` `handleProbeDiag` | Endpoint di dispositivo che pubblicava `armed` da `c_()`, cioè sempre dalla camera 1 | N=2 | `dd23b9d` |

## Decisione ribaltata

**G10 contraddice una mia scelta precedente.** In `88f34ea` avevo *respinto* il rilievo «le rotte
`/api/ch/…` sono morte a N=1», argomentando che `/api/ch/1/status` funziona comunque perché
`selectChamber` accetta `1..AX_N_CHAMBERS`. L'argomento era **corretto ma irrilevante**: il
problema non è che siano morte, è quanto **costano**. Misurato sul sorgente del framework, non
valgono 2-3 KB di heap permanente. Ora si registrano solo con `N > 1`.

## Limiti resi espliciti (non difetti)

- **La densità è ancora una risorsa di dispositivo** — un solo `GravityTracker` filtrato per nome
  — mentre `ogGravity` è già per camera. La cattura automatica dell'OG alimenta quindi solo la
  camera 1; per le altre si usa `/api/ch/<n>/gravity/og`. È il perimetro dichiarato nel report
  forense §1.2, ma senza commento nel codice si legge come una svista.
- **A N=1 il contenuto di `/state.json` non è più identico alla v2.23.2** (le chiavi sono
  annidate in `chambers[0]`). È deliberato, ma implica che **riportare indietro il firmware dopo
  l'aggiornamento fa perdere lo stato**: il vecchio codice non riconoscerebbe lo schema v2. Il
  criterio di accettazione «identico a N=1» va inteso come **identità di comportamento**, non di
  formato dei file.
- `MqttSvc` non ripulisce le config di discovery *retained* delle camere che non esistono più
  dopo una riduzione di `AX_N_CHAMBERS`: restano entità fantasma in Home Assistant.
