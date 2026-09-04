# FASE 4 — Verifica avversariale dei passi 1-4

Campagna del 2026-08-31 sul diff da `ad1b50d` (baseline v2.23.2) ai passi
`pid-phase`, `chamber-ctx`, `probe-roles`, `outputs-per-chamber`.

**Metodo:** 5 lenti avversariali indipendenti (regressione a N=1, elettrico/attuatori, modi di
guasto e isolamento fra camere, indici/limiti/memoria, stato-boot-persistenza) → **45 rilievi
grezzi** → confutazione con 2 scettici indipendenti per rilievo.

## Nota sulla copertura — risolta

Nella prima esecuzione **44 dei 95 agenti di confutazione sono falliti per un limite di
sessione**, lasciando 23 rilievi (indici 20, 23-44) senza verdetto. Registrarli come "scartati"
sarebbe stato un falso negativo travestito da rigore.

**I controlli interrotti sono stati completati** (commit `3f9edc4`): dei 23 rilievi, **16 erano
duplicati** di difetti già corretti — le 5 lenti avevano trovato gli stessi problemi da angoli
diversi. I 7 distinti sono stati verificati uno per uno leggendo il codice: **4 reali** (F13-F16
sotto), 3 non difetti (sezione finale). Il conteggio grezzo di 45 corrisponde a **~20 difetti
distinti**.

## Rilievi corretti

| ID | Gravità | File:riga | Descrizione | Scenario di innesco | Fix | Commit |
|---|---|---|---|---|---|---|
| F1 | **CRITICO** | `main.cpp:1011` → `WebApi.cpp:1529` | Il gate del test manuale relè via **web** consultava `isArmed()`, legata alla sola camera 0. La CLI seriale era già stata irrobustita con `anyChamberArmed()`; il percorso web no | N=2, camera 2 armata e camera 1 disarmata: `POST /api/diag/relay` non vede l'armamento, entra in test mode → `applyAutomation` diventa no-op per **tutte** le camere fino a `test off` o al timeout di 10 min, e i relè si comandano a mano | `Refs::anyArmed`, semantica esplicitamente distinta da `isArmed` (dispositivo vs camera servita) | `bd5e73b` |
| F2 | **ALTO** | `main.cpp:1057` | Il salto d'orologio era propagato solo a `chambers[0]`: le altre restavano con guardia compressore, latch d'isteresi e tempo morto interlock ancorati alla **vecchia timeline** | Sync NTP dopo boot con RTC inaffidabile, N=2 | Ciclo su tutte le camere. È la forma di difetto che `ControlPipeline::shiftClock` centralizza per evitare | `bd5e73b` |
| F3 | **ALTO** | `main.cpp:430` | `rebaseRampPending` era rimasta una `static` **di file**. `ChamberCtx` aveva raccolto le `static` **locali di funzione**: questa è sopravvissuta | Riavvio con orologio inaffidabile mentre la camera 1 è in rampa; al primo tick la sonda della camera 1 non è ancora valida (85 °C di power-on) ma quella della camera 2 sì → la camera 2 consuma il flag → **la rampa della camera 1 non viene mai ri-ancorata** | Spostata in `ChamberCtx` | `bd5e73b` |
| F4 | **ALTO** | `main.cpp:1017` | `POST /api/outputs/disarm` disarmava solo la camera 0 rispondendo 200 OK | N=2, l'utente preme "Disarma" e la camera 2 resta armata | L'endpoint non porta indice: disarma **tutte**. Per un'azione di sicurezza fare di più è corretto | `bd5e73b` |
| F5 | **ALTO** | `WebApi.cpp:1234` | `handleProbeCalib` usava il letterale `wanted[0]` come sinonimo di "sonda di controllo" — l'assunzione indice⇒ruolo rimossa da `b9e506d` | N=2: si tara la sonda di controllo della camera 2 mentre pilota i relè | Usa `*r_.controlProbe` | `bd5e73b` |
| F6 | **ALTO** | `CompressorGuard` / `ControlPipeline.h:55` | Lo sfasamento copriva solo il lato **caldo** (finestra PID). I riposi del compressore si **sincronizzano**: dopo un blackout tutte le camere chiamano `assumeJustStopped()` con lo stesso epoch, quindi i riposi scadono nello stesso secondo | Blackout con entrambe le camere sopra setpoint → i due compressori partono insieme, sommando spunti di 5-8× la corrente di marcia | `setStartStagger()`: **allunga** il riposo di `idx * 30 s`, mai lo accorcia (blindato da test) | `691db85` |
| F7 | **ALTO** | `main.cpp:890` | Le camere senza stato persistito ripartivano con guardia **vergine** (`lastOffEpoch == 0` = "nessun vincolo") | Riavvio a compressore in marcia su una camera ≥2 → riaccensione immediata, cioè il corto ciclo che la guardia esiste per impedire | Assunte "appena fermate" al boot → riposo pieno | `691db85` |
| F8 | **MEDIO** | `main.cpp:929` | Il mapping persistito veniva applicato con `setMapping` alla cieca, scavalcando le invarianti centralizzate in `armOutputs` | `/state.json` corrotto o scritto a mano con freddo e caldo sullo **stesso relè** → compressore e riscaldante comandati insieme dallo stesso pin, aggirando l'interlock | Rivalidazione al boot; mapping non valido → uscite non assegnate, camera disarmata, `bootStorageFault` (visibile, non silenzioso) | *(questo commit)* |
| F9 | **BASSO** | `OutputBus.h` | `writeRel(0)` avrebbe scritto `states_[-1]` | Non raggiungibile oggi (i chiamanti filtrano lo 0), ma è il punto unico di scrittura dei relè | Guardia `rel` in 1..8 | `bd5e73b` |
| F10 | **BASSO** | `ax_dimensions.h` | `kMaxProbes` con cast a `uint8_t` troncherebbe in silenzio oltre 85 camere | Teorico su ESP8266, ma è un indice che pilota attuatori | `static_assert(1..8)` | `bd5e73b` |
| F11 | **BASSO** *(regressione N=1)* | `main.cpp:744` | Il nuovo parser di `arma` leggeva 3 numeri come `<camera> <cool> <heat>` anche a camera singola; la v2.23.2 leggeva sempre due campi (`arma 2 3 4` → freddo=REL2, caldo=REL3) | `arma 2 3 4` a N=1 | La forma a 3 token esiste solo se `AX_N_CHAMBERS > 1` | `bd5e73b` |
| F12 | **BASSO** | `PidController.h` | La soglia documentata del limite dello sfasamento era sbagliata: non `duty >= 99` ma la guardia sul minimo OFF (`windowMs - onMs < 1000`), cioè già da duty > ~96,7 % col default | — | Commento corretto | `bd5e73b` |

### Rilievi dei controlli completati

| ID | Gravità | File:riga | Descrizione | Scenario di innesco | Fix | Commit |
|---|---|---|---|---|---|---|
| F13 | **MEDIO** *(regressione)* | `main.cpp:176` | `disarma` non spegneva più tutti i canali. Nel commit `77ddb71` avevo affermato che sostituire `allOff()` con `chamberOff()` fosse "osservabilmente identico a N=1 perché i relè non mappati sono già spenti". **Era sbagliato: avevo dimenticato la modalità test** | In modalità test `rele 5 1` eccita REL5 (`testRelay` accetta 1..8). Né `disarma` né `POST /api/outputs/disarm` hanno un gate sulla modalità test → REL5 **resta eccitato** dopo il disarmo. "Disarma" è l'azione usata come fermo d'emergenza | `disarmAllOutputs()`: disarma ogni camera, poi `allOff()` come rete finale. Il disarmo **per camera** resta `chamberOff()` | `3f9edc4` |
| F14 | **MEDIO** | `WebApi.cpp` `handlePutConfig` | Nessun controllo di unicità sugli **ID ROM** delle sonde, in nessun punto (né `fromJson`, né `configureProbe`, né `handlePutConfig`) | N=2: la stessa DS18B20 come sonda di controllo di due camere → due frigoriferi regolati sulla stessa temperatura, in silenzio. È lo specchio del conflitto relè, già rifiutato da `armOutputs` | Rifiuto con 400 | `3f9edc4` |
| F15 | **MEDIO** | `WebApi.cpp` `handleProbeCalib` | Risposta costruita in un doc da 384 B **senza guardia `overflowed()`** — unica eccezione fra 10 punti del firmware che la applicano | Con più sonde l'array `applied` cresce e la risposta si tronca in silenzio, rompendo il pannello | Guardia aggiunta | `3f9edc4` |
| F16 | **BASSO** | `WebApi.cpp:1116`, `:1296` | Documenti JSON dimensionati su 3 sonde fisse | A N=2 iterano 6 sonde | Proporzionali a `ProbeBus::kProbes`, con coefficienti scelti perché a `kProbes=3` diano **esattamente** i valori storici (1024×3=3072, 128×3=384) | `3f9edc4` |

### Verificati e NON difetti

| Rilievo | Esito |
|---|---|
| Wrap di `(nowMs + offset)` a 32 bit nella fase PID | **Benigno.** La discontinuità al wrap di `millis()` esiste già nella baseline (`2^32 mod windowMs` = 17.296 ms col default) e l'offset non la crea. Soprattutto: lo sfasamento **relativo** fra camere è **invariante al wrap** — misurato, scostamento **0 ms** — quindi la desincronizzazione non si perde mai |
| `handleProbeDiag` troncamento silenzioso | **Già protetto** da `doc.overflowed()` con 500 esplicito: la campagna di hardening precedente lo copriva |
| Misure di RAM riportate da 2 lenti | Coincidono con le mie |
| `factoryResetPending` non copre `handlePutConfig` | **Preesistente**, non introdotto dal refactor. Una `PUT /api/config` nella finestra di 1 s ricreerebbe `/config.json`, ma richiede un client autenticato che vinca una corsa di un secondo. Registrato, fuori scope |

## Residui aperti, da chiudere nei passi successivi

| ID | Gravità | Dove | Perché resta aperto |
|---|---|---|---|
| R-A | **ALTO** | `main.cpp:87` `persistNow()` | **Confermato da 2 scettici.** Salva solo `chambers[0]`: con N=2 lo stato delle altre camere si perde a ogni riavvio (profilo, guardia compressore, stimatori). È il perimetro dichiarato del passo **`persistence-v2`**, non un difetto trascurato — ma finché quel passo non c'è, **N=2 non va messo in produzione** |
| ~~R-B~~ | — | — | ✅ **Chiuso** da F14 |
| ~~R-C~~ | — | — | ✅ **Chiuso** da F15/F16 |
| R-D | BASSO | `WebApi.cpp:236` | `/api/status` mescola due convenzioni: i **valori** vengono da `cam.probeControl/probeAux`, **etichette e flag** da `cfg->probeLabel[i]`. Da unificare in **`api-v2`** |
| R-H | BASSO | `ProfileStore.cpp:84` | `readFile` alloca ancora 6144 in un colpo (la scrittura è già in streaming dal fix v2.22.1). **Verificato che NON coesiste** con un altro documento grande: ogni chiamante libera questo doc prima di allocare il proprio, e al boot le camere si leggono in sequenza — il picco resta di *un* solo documento, come nella v2.23.2. Ridurre la capacità a stima renderebbe illeggibili profili validi (`NoMemory`): va fatto misurando il caso peggiore a 12 step **sul dispositivo** |
| R-E | BASSO | `Mcp23008.h:28` | `recover()` riparte da `OLAT=0`, spegnendo tutti i REL4-8 comprese le uscite di un'altra camera; `applyAutomation` le ripristina entro 1 s. **Preesistente**, non introdotto dal refactor |
| R-F | BASSO | `main.cpp:172` | `disarmOutputs` non azzera `map_[ch]`, quindi una camera disarmata continua a "prenotare" i suoi relè verso `relayTakenByOther`. **Scelta deliberata**: il mapping è configurazione, non stato di marcia — non si vuole che l'altra camera rubi un relè durante un disarmo temporaneo |
| R-G | BASSO | `ProbeBus.h:163` | La rotazione dell'ordine di lettura avanza su `kProbes` invece che sulle sonde effettivamente configurate: con slot dichiarati e non usati la rotazione perde un po' di efficacia. Cosmetico |
