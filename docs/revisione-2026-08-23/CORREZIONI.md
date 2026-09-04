# Correzioni dei 6 rilievi critici — AxHTherm 2.5

Data: 2026-08-24. Applicate dopo la revisione forense (`REFERTO_FINALE.md`).
Verifica: `pio test -e native` → **64/64 test** (5 nuovi di regressione); `pio run -e d1_mini` → **build pulita** (Flash 67.8%, RAM 55.3%).

> Le correzioni sono state trattate come codice nuovo (§7 del mandato). La revisione della
> correzione ha già intercettato un difetto **nella correzione stessa** di C12 (vedi sotto):
> il primo tentativo traslava i latch ma non la guardia compressore — la stessa forma del
> difetto che C12 chiude. Risolto centralizzando la traslazione dietro un unico punto.

## C12 — Salti d'orologio non traslano i latch di isteresi/interlock
- `HysteresisController::shiftClock`, `Interlock::shiftClock` (nuovi): traslano i timestamp
  àncora dei latch, saltando lo 0 («mai commutato»), come già fanno motore e guardia.
- `ControlPipeline::shiftClock` (nuovo): **punto unico** che trasla guardia + isteresi +
  interlock. Così un salto non può dimenticare un latch e congelarlo.
- `main.cpp` loop: ora chiama solo `pipeline.shiftClock(clockJump)` (prima traslava la
  guardia a parte: la centralizzazione evita la doppia traslazione e le dimenticanze).
- Test: `test_hysteresis_shift_clock_backward`, `test_interlock_shift_clock_backward`,
  `test_pipeline_shift_clock_unfreezes_latch`.

## C31 — Salto d'orologio applicato subito ma traslato al giro dopo
- `main.cpp` loop riordinato: il blocco `consumeClockJump()` è spostato **dopo**
  `web.loop()`/`serialPoll` (che possono accodare un salto via `/api/time` o seriale `ora`)
  e **prima** di `controlTick`. Le àncore sono traslate prima che il tick le usi, nello
  stesso giro. Elimina il fast-forward del profilo / azzeramento guardia (archetipo 10).

## C14 — Step RampRate senza uscita garantita/timeout/allarme
- `ProfileEngine::stepDone` (RampRate): onora `maxDurationSec` come timeout di sicurezza
  (0 = nessuno), come per gli step Gravity → uscita garantita quando configurato.
- `ProfileEngine::tick`: nuovo flag `TickOutput::waitingRampStall`, alzato quando il
  setpoint è saturato al target ma la temperatura non entra in tolleranza, **un'ora oltre**
  la fine nominale della rampa (grazia anti-falso-positivo). Guardato da `!s_.completed`.
- `SafetyMonitor::rampStalled` (solo allarme, non blocca le uscite: la rampa continua a
  provarci), in `anyAlarm()`.
- Propagazione: `main.cpp` (`safety.rampStalled`, `st.alarmRampStall`), `AppStatus`,
  `WebApi::handleStatus` (stringa d'allarme → la UI la rende come le altre), `OledUi`
  (`! RAMPA FERMA`), `MqttSvc` (OR dell'allarme aggregato).
- `WebApi::validateSteps`: valida `maxDurationSec` anche per RampRate (≤ 90 giorni).
- Test: `test_profile_ramprate_safety_timeout`, `test_profile_ramprate_stall_flag`.

## C15 — gravityUnit «solo etichetta» ma governa la logica
- `WebApi::handlePutConfig`: rifiuta con 409 il cambio di `ispindel.unit` mentre un profilo
  è in modalità Profile (governa confronti/formule degli step a densità: cambiarla a caldo
  farebbe avanzare o bloccare uno step su una scala diversa).
- `AppConfig.h`: commento corretto (non è «solo etichetta»).

## C21 — File troncato promosso con rename (n==0 invece di n==misura)
- `ConfigStore::save` e `ProfileStore::writeFile`: confrontano i byte scritti con
  `measureJson`; su mismatch rimuovono il tmp e ritornano false **prima** del rename.
  Allineate al pattern già presente in Calib/State/Boot/Devices. Su FS pieno il file buono
  non viene più distrutto (evitava: profilo attivo troncato → cotta senza controllo al boot).

## C32 — Comando MQTT retained rieseguito a ogni riconnessione
- `MqttSvc` callback: ignora i messaggi di lunghezza 0; dopo aver eseguito un comando
  **cancella il retained** del topic (publish vuoto retained) — MQTT non espone il flag
  retained alla callback, quindi un comando ritenuto verrebbe riconsegnato a ogni SUBSCRIBE.
  Così è eseguito al più una volta, mai a ripetizione (niente OG azzerata a ogni reconnect).
  Usa `t.c_str()` (copia su heap) per non scrivere sul buffer da cui punta `topic`.

## File toccati
`lib/axcore/src/ax/`: HysteresisController.h, Interlock.h, ControlPipeline.h, ProfileEngine.h,
ProfileEngine.cpp, SafetyMonitor.h · `src/`: main.cpp, svc/AppStatus.h, svc/AppConfig.h,
svc/ConfigStore.cpp, svc/ProfileStore.cpp, svc/WebApi.cpp, svc/MqttSvc.cpp, svc/OledUi.cpp ·
`test/test_core/test_main.cpp` (5 nuovi test).

## Non incluso (per scelta)
- Un default di `maxDurationSec` per le rampe non è stato imposto: cambierebbe il
  comportamento dei profili esistenti (una rampa lenta legittima potrebbe durare giorni).
  La visibilità dello stallo protegge i profili esistenti senza alterarli; il timeout resta
  opt-in. La libreria dei 49 modelli **non** è stata modificata.

---

# Correzioni dei 14 rilievi gravi — 2026-08-24

Verifica: `pio test -e native` → **65/65** (66° test aggiunto: contratto flag di solo-allarme);
`pio run -e d1_mini` → **build pulita** (Flash 68.0%, RAM 56.2%).

## Sicurezza funzionale attuatori
- **C10 + C11** — `armOutputs` (main.cpp): dopo `allOff()` chiama
  `pipeline.guard().assumeJustStopped(now)`. La guardia impone il riposo pieno prima di
  (ri)avviare il compressore, sia dopo un test manuale (riavvio a riposo zero) sia dopo un
  riarmo a caldo (corto ciclo <1 s). Conservativo: al più ~5 min di attesa dopo l'arm,
  visibile in `coolBlockedSec`.

## Verità di ciò che è mostrato (asse G — «un numero falso è un guasto»)
- **C23** — `SafetyMonitor::storageFault` (nuovo, solo allarme); `persistNow` e `logSample`
  registrano l'esito della scrittura invece di ignorarlo; allarme propagato a
  status/OLED/MQTT/web. Una persistenza che fallisce non è più invisibile.
- **C36** — `logSample` (main.cpp): densità nel log clampata a [1, 65535]. Una densità
  °Plato negativa non trabocca più in uint16 (era -0,3 °P → 655,06 °P) e 0 non confonde
  con «mancante».
- **C37** — `index.html`: banner rosso «CONNESSIONE PERSA — dati fermi da N s» quando
  `refresh()` fallisce per più di ~8 s. Una dashboard congelata non è più indistinguibile
  da una viva.
- **C01** (residuo non-avversariale) — allarme «più densimetri attivi senza selezione»
  (`gravityDevice` vuoto e >1 densimetro visto): rende visibile l'ambiguità che alimenta la
  cotta sbagliata / avvelena l'OG. Il meccanismo di rete non-autenticato resta dentro il
  modello di minaccia LAN accettato (nessuna patch: aggiungere un debounce non ferma un
  iniettore deliberato).

## Correttezza / range
- **C16** — comando MQTT `climate_temp` (main.cpp): il payload deve essere numerico
  (`strtod` con controllo del terminatore); `"None"`/`""`/`"unknown"` non diventano più
  setpoint 0 °C.
- **C17** — floor antigelo unificato a **-1 °C** su `/api/setpoint`, `/api/thermostat` e
  `climate_temp` MQTT (erano -10, in contraddizione con l'invariante degli step: sotto -1
  il rischio è il congelamento della birra).
- **C18** — cattura OG (main.cpp): guardata anche contro la **pausa** (`!engine.paused()`)
  e con lo stato di partenza ritardata **live** (`snapshot().delayedStartEpoch==0`, non la
  cache di `lastTick` vecchia fino a 1 s). Niente più OG del sanificante.

## Persistenza / integrità
- **C22** — `ConfigStore::fromJson`: tetto a `logRetentionDays` (90 giorni), così il
  registro non può riempire la LittleFS e innescare i guasti da scrittura fallita.
- **C24** — `handlePutConfig`: pattern «copia → salva → applica solo se riuscito» con una
  `static AppConfig` (fuori dallo stack profondo del web). Un salvataggio fallito non lascia
  più RAM e flash divergenti (password nuova già attiva, utente escluso; commit permanente
  via taratura).

## Superficie esterna
- **C03** — gate OTA (WebApi): richiede una password UI **non vuota** oltre che non di
  default (una password vuota rende `authValid()` vera per tutti → OTA aperto a chiunque);
  allarme dedicato per password vuota.
- **C07** — comando MQTT `profile start` (main.cpp): non riavvia un profilo GIÀ in corso
  (azzererebbe l'OG). Si parte solo da fermo o da termostato.

## Risorse
- **C29** — export CSV (WebApi): tetto di 50000 righe con troncamento **visibile**
  (`# export troncato…`). Bound al caso patologico (campionamento fitto su lunga retention);
  lo stallo residuo del controllo è comunque limitato dal timeout lwIP (~1-2 min).

## Note e scelte deliberate
- **C01** e **C29** hanno ricevuto correzioni proporzionate (visibilità dell'ambiguità;
  bound + troncamento visibile), non una riscrittura: il primo è dentro il modello di
  minaccia accettato, il secondo è già limitato dall'infrastruttura di rete.
- I fix dello strato servizi (`src/`) non sono testabili a banco (`test_build_src=false`;
  dipendono da Arduino.h). Verificati per compilazione con `-Wall` e per lettura. È la
  lacuna di test dell'asse H (C42), non chiusa qui.

---

# Correzioni dei 20 rilievi minori — 2026-08-24

Verifica: `pio test -e native` → **68/68** (3 nuovi test: skip in partenza ritardata,
profileStartEpoch non traslato); `pio run -e d1_mini` → **build pulita** (Flash 68.1%, RAM 56.4%).

## Corretti con codice (14)
- **C02** — `onIspindel`: scrittura di `devices.json` limitata a una ogni 60 s (RAM sempre
  aggiornata) e bloccata nel factory reset: usura flash da flood LAN limitata.
- **C19** — `skip()` in attesa di partenza ritardata: azzera `delayedStartEpoch` se completa
  (niente resurrezione all'epoch) e resta in attesa se avanza a uno step successivo. + test.
- **C20** — `/api/setpoint` risponde 409 (non un falso 200) se l'override è un no-op
  (profilo completato o modalità Off).
- **C25** — `persistNow` è un no-op durante la finestra del factory reset (dichiarazione di
  `factoryResetPending` spostata in alto): nessun persist ricrea `/state.json`/`/devices.json`;
  il dispositivo «resettato» non rinasce più armato.
- **C26** — `ProfileEngine::shiftClock` NON trasla più `profileStartEpoch` (istante reale
  d'inizio, come `delayedStartEpoch`): dopo blackout con RTC morto + NTP i giorni restano
  corretti invece di finire nel ~2081. + guardia di plausibilità sui giorni (MQTT/UI). + test.
- **C27** — `ConfigStore`: buffer 4096→6144, controllo `overflowed()` (mai troncare in
  silenzio), tetti di lunghezza sui campi liberi (hostname/ssid/password/ntp/tz/mqtt…).
- **C30** — `ProfileStore::list()` elenca con un marcatore d'errore un profilo esistente ma
  illeggibile (OOM/corrotto), così il client non lo crede libero e non lo sovrascrive.
- **C33** — `/api/status` costruito fresco a ogni richiesta (non la cache di 1 s): niente più
  risposte contraddittorie subito dopo un comando.
- **C34** — flag `rebootPending` esplicito al posto della sentinella `rebootAtMs==0` (che al
  wrap di `millis()` poteva coincidere e non far mai riavviare).
- **C35** — UI: la conversione del target in °Plato si applica solo agli step «densità sotto»
  (mode 0), non alle soglie di attenuazione (%) — niente più «att. ≥ 253,8 %».
- **C38** — `testMode` in `AppStatus`: OLED mostra «TEST RELE' ATTIVO» invece di «riposo»
  mentre un relè è eccitato a mano (la pagina già lo mostrava).
- **C39** — UI: «PID N %» mostrato solo col PID attivo (duty>0); a isteresi non appare più
  «PID 0 %» col riscaldatore acceso.
- **C40** — numerazione step degli abilitati coerente ovunque: aggiunti `stepEnIdx/stepEnCount`
  (visualizzazione) mantenendo `stepIdx/stepCount` assoluti (indicizzazione lato client).
  Mode line/OLED/MQTT/riquadro/cold-crash ora mostrano lo stesso numero.
- **C41** — UI: la tile «Giorni» si calcola solo da un `profileStartEpoch` reale (>~2017);
  in termostato o su timeline fittizia mostra «--» invece di un'età falsa legata al grafico.

## Non corretti — scelte deliberate (6)
- **C04** (token deterministico/non revocabile) — scelta di progetto documentata
  (ARCHITETTURA.md §Sicurezza), dentro il modello LAN/HTTP accettato; il confronto non a tempo
  costante è ininfluente su una LAN dove il cookie è comunque sniffabile in chiaro.
- **C05** (nessun rate-limit sul path Basic auth) — **una correzione naïve peggiorerebbe le
  cose**: un `delay()` bloccante su ogni auth fallita darebbe a un attaccante un DoS del loop
  di controllo (300 ms di stallo per richiesta) peggiore del bruteforce che eviterebbe, su
  credenziali già sniffabili. Archetipo 8. Un lockout non bloccante sarebbe l'unico fix corretto,
  ma è sproporzionato per il rischio dentro il modello accettato.
- **C06** (corpo POST non limitato) — confermato dal confutatore che NON causa OOM (la realloc
  fallita libera il buffer) e lo stallo è limitato dal timeout d'inattività; il tetto va posto
  a livello di framework (ESP8266WebServer), non raggiungibile con una modifica proporzionata.
- **C08, C09** (guasti/reset del MCP23008) — **inerti su questo hardware**: `ANALISI.md:188`,
  il chip non è montato; i carichi stanno su REL1/REL3 (GPIO diretti). Reali solo per un build
  con MCP popolato; l'hardening generico (re-assert periodico di IODIR) richiederebbe test su
  hardware con MCP presente, non disponibile.
- **C42** (test di regressione ProbeBus assenti) — la correzione è estrarre la FSM dietro
  un'interfaccia di letture iniettabile: **un refactor sostanziale sul file più rischioso della
  base** (6 archetipi nati qui). Farlo in fretta violerebbe la cautela del §7. Va pianificato e
  revisionato a sé, non incluso in questo lotto.

## Nota
I fix dello strato servizi/UI non sono testabili a banco; verificati per compilazione (`-Wall`)
e per lettura. La coerenza della numerazione step (C40) tocca più punti dell'interfaccia: la
revisione ha verificato che gli indici assoluti (righe che indicizzano `RUNPROF.steps`) restino
intatti e solo la visualizzazione passi agli step abilitati.

---

# Revisione delle correzioni come codice nuovo (§7) — regressioni corrette — 2026-08-24

Un fix-review avversariale (7 agenti scettici sui cluster di fix + gap di copertura) ha trovato
**15 difetti INTRODOTTI dalle correzioni** (2 critici) e 2 minori nei gap. Tutti corretti.
Verifica: `pio test -e native` → **69/69**; `pio run -e d1_mini` → **build pulita** (Flash 68.1%).
Questo conferma la clausola §7: «il codice appena scritto per riparare è il meno affidabile».

## Regressioni critiche
- **C31** — Il riordino del loop faceva raddoppiare la traslazione di un'àncora epoch scritta
  da un handler nel giro della prima sync NTP (base_ cambiava subito, shiftClock dopo →
  àncora nel ~2080, profilo congelato per anni o compressore mai avviato, in silenzio).
  Fix: reso il salto ATOMICO — `TimeService` non applica più `base_` al momento del salto;
  lo fa `consumeClockJump()`, nello stesso blocco dello `shiftClock` (nowEpoch e àncore
  saltano insieme). La scrittura RTC del blocco NTP usa `t` (il tempo NTP), non `nowEpoch()`.
- **C27** — La password UI oltre 64 byte veniva troncata in silenzio (cap di ConfigStore),
  causando lockout dell'interfaccia (proprio l'archetipo 13). Fix: `handlePutConfig` rifiuta
  con 400 una password UI (>64) o SoftAP (>63) troppo lunga, invece di troncarla.

## Regressioni gravi
- **C23** — Un checkpoint di /state.json riuscito azzerava l'allarme di scrittura del REGISTRO
  fallita (un solo flag). Fix: due flag separati (`persistFault`, `logFault`), OR nell'allarme.
- **C17** — «termostato OFF» rifiutato con 400 se il setpoint persistito era < -1 (migrazione
  da pre-C17). Fix: il range si valida solo quando si ACCENDE; spegnere riesce sempre.
- **C41** — Tile «Giorni» negativa (orologio fittizio + inizio profilo reale). Fix: guardia
  `now > startEp` oltre alla plausibilità di startEp.
- **C26** — Riquadro di marcia a «giorno 19560»/«100%» se il profilo era partito con orologio
  non attendibile (startEpoch fittizio + now reale). Fix: `trasc` calcolato solo con timeline
  plausibile (startEpoch e now > ~2017, now ≥ startEpoch); ETA guardata allo stesso modo.
- **C15** (preesistente) — La guardia unità copriva solo il profilo in corsa: cambio a profilo
  FERMO ma caricato + riavvio valuta gli step a densità sulla scala sbagliata. Fix: blocco anche
  con un profilo a densità solo caricato (il fix completo sarebbe marcare i profili con l'unità).

## Regressioni/coerenze minori
- **C14** — Lo stallo rampa usava un rate clampato incoerente con `stepSetpoint`: per rate
  <0.01 °C/h l'allarme era rinviato di ~41 giorni. Fix: `nominalSec=0` per rate<0.01 (allarme
  dopo la sola grazia). + test `test_profile_ramprate_stall_zero_rate`.
- **C07** — Il guard MQTT `start` bloccava anche il riavvio di un profilo COMPLETATO. Fix:
  `!engine.running()` (blocca solo un profilo attivo).
- **C02** — Il throttle 60s poteva non persistere un densimetro legittimo. Fix: persistenza
  immediata in slot libero; throttle solo sulle eviction (il flood).
- **C20** — Il 409 «no-op» non copriva l'attesa di partenza ritardata. Fix: incluso.
- **C01** — L'allarme «più densimetri» non entrava nell'aggregato MQTT. Fix: aggiunto all'OR.
- **C21** — `ProfileStore::writeFile` non controllava `doc.overflowed()` come `ConfigStore::save`
  (invariante in un solo store). Fix: aggiunto.

## Gap di copertura chiusi
- **gzip_assets.py** — saltava in silenzio se `index.html` mancava (UI stantia spedita). Fix:
  ora fallisce il build con un errore esplicito. (Il resto dello script è stato verificato
  sano: ETag deterministico, encoding byte-esatto, PNG splash ben formati.)
- **COLLAUDO.md** — si aspettava MCP23008@0x20 e REL4-8 (assenti sull'esemplare). Aggiornato:
  0x20 non compare (chip non montato), i carichi sono su REL1 (riscaldatore)/REL3 (frigo).

---

# Correzioni del backlog dei giri 2-5 — 2026-08-25

Corretti 18 dei 19 rilievi del backlog (BACKLOG-giri2345.md). Verifica: `pio test -e native`
→ **69/69**; `pio run -e d1_mini` → **build pulita** (Flash 68.3%, RAM 57.4%).

## Critici / gravi
- **[critico] Freeze-guard all'ATTUAZIONE** — clamp del setpoint effettivo a [-1, 40] in
  controlTick (unico punto di strozzatura): copre ogni sorgente (termostato, override, MQTT
  climate_mode=auto grezzo, ripristino al boot di un valore pre-guardia), non solo gli ingressi.
- **[critico] Ripresa con orologio inaffidabile** — una rampa °C/h ri-basata non riparte più
  dalla temperatura d'inizio (abbandonando il freddo già raggiunto): la si congela al target.
- **[critico] Step a densità appeso per sempre** con dato fresco ma condizione mai soddisfatta:
  escalation ad `alarmGravity` dopo 21 giorni (orologio attendibile) — l'equivalente di C14.
- **[grave] Allarme di fuga/scostamento termico** (nuovo): setpoint stabile e T oltre 5 °C per
  più di 3 ore, uscite consentite → allarme su web/OLED/MQTT (l'impianto non ce la fa).
- **[grave] Guardie HTTP contro l'azzeramento silenzioso**: `/api/profile/start` non riavvia un
  profilo in corso (azzererebbe l'OG), `/api/thermostat {on:true}` non abbandona un profilo.
- **[grave] Locking ottimistico sul config** (`configRev`): un PUP con revisione stantia è
  rifiutato con 409 — una taratura concorrente non viene più cancellata in silenzio. UI adeguata.
- **[grave] isfinite** su `/api/setpoint`, `/api/thermostat` e metriche: un NaN non rende più
  illeggibile tutto `/api/status`.
- **[grave] Bit allarme del registro** completo (9/9): il post-mortem non mostra più «nessun
  allarme» durante un guasto.
- **[grave] Nome iSpindel a 32 byte** (era 16): due densimetri con nome che collide nei primi
  16 byte non si confondono più.
- **[grave] Riposo compressore >= 1 minuto** (era senza minimo): 0 non disattiva più la guardia.

## Minori
- **[minore, regr C31] Timbro ora del registro avvii** guardato con plausibilità (>~2017): non
  prende più un epoch ~1970 nella finestra fra trusted e salto NTP applicato.
- **[minore, regr C14] Timeout rampa °C/h** ora impostabile dall'editor (era nascosto per ramp_rate).
- **[minore] GET /api/profiles/{id}** distingue 404 (assente) da 500 (esiste ma illeggibile).
- **[minore] Conferma factory reset** accurata (elenca libreria, calibrazione, registro densimetri).
- **[minore] Nome cotta** troncato su confine UTF-8 (non spezza più una sequenza multibyte).
- **[minore] Discovery MQTT climate** con controllo `overflowed()` + buffer ampliati (config non
  più troncata verso Home Assistant con base topic lungo).
- **[minore] dtSec reale** (era 1.0 fisso): PID e stimatore predittivo sul tempo davvero trascorso.
- **[minore] Priorità allarmi OLED** riordinata: i guasti di cotta precedono «orologio inaffidabile».
- **[minore] Whitelist gravityUnit** ("sg"/"plato"): una variante non cade più in silenzio su SG.
- **[minore] RUNPROF**: le risposte fuori ordine non lasciano più a schermo il piano vecchio.

## Non corretto (limitazione documentata)
- **[minore] Il registro binario non porta l'unità di densità**: cambiare SG↔Plato (ora bloccato
  con un profilo a densità caricato, C15) reinterpreta la cronologia. Un fix pulito richiede una
  versione di formato o un tag di unità per file — cambio del formato del registro, rimandato.

## Nota §7
Questi ~20 fix sono codice nuovo e vanno rivisti come tali. È in corso un fix-review avversariale.

---

# §7 — Revisione dei fix del backlog: regressioni corrette — 2026-08-25

Un secondo fix-review avversariale (4 agenti) ha trovato 9 difetti nei fix del backlog (di cui
~5 regressioni). Corretti. Verifica: `pio test` → **69/69**; `pio run` → **build pulita** (Flash 68.3%).
(Terzo giro di review che trova regressioni nei fix: la clausola §7 si conferma a ogni passata.)

## Regressioni gravi corrette
- **Allarme fuga termica — falso positivo** su pull-down/cold-crash legittimi (escludeva solo le
  rampe, non gli step Fixed né il termostato: un raffreddamento di 4-6 h scattava a 3 h). Fix:
  gate «banda raggiunta almeno una volta» — l'allarme scatta solo se, DOPO essere entrata in banda,
  la T diverge; e uno step con allow* che vieta l'attuatore non raggiunge mai la banda → niente falso allarme.
- **gravityStuck a 21 giorni — falso positivo** su lagering/fermentazioni lunghe (il dominio dichiara
  cotte fino a 6 mesi). Fix: **rimosso** (il tempo fisso è l'euristica sbagliata; il rilevamento vero
  richiede il tracciamento del progresso della densità — lavoro futuro). Visibilità via profileNoLimit.
- **Ripresa RampTime**: il fix precedente congelava al target solo i RampRate; i RampTime-fromCurrent
  ripartivano dall'origine (setpoint che risale, freddo abbandonato). Fix: esteso ai RampTime-fromCurrent.
- **Locking UI**: l'interfaccia non aggiornava CFG.configRev dopo un salvataggio → il secondo salvataggio
  falliva con un 409 falso. Fix: il PUT restituisce la nuova revisione, l'interfaccia la aggiorna.
- **Bit allarme del registro**: avevo dimenticato alarmThermal → fuga termica invisibile nel post-mortem.
  Fix: aggiunto (ora 10/10 allarmi nel bit del log).

## Regressioni/coerenze minori corrette
- Nome iSpindel: il cap a 32 byte spezzava UTF-8 (a differenza del batchName). Fix: trim su confine.
- CalibStore save/load e handleCalibGet: buffer a 3072 + controllo overflowed() (l'ampliamento nomi
  a 32 byte poteva far promuovere/servire un calib.json troncato o un GET con punti persi in silenzio).

## Residuo documentato
- Un PUT /api/config **senza** campo configRev bypassa il locking (leniency voluta per gli script;
  l'interfaccia lo manda sempre). Chiuderlo del tutto renderebbe obbligatoria la revisione anche per
  gli script — scelta lasciata aperta.

---

# §7 — Terzo giro di review: regressioni corrette — 2026-08-25

Un terzo fix-review (3 agenti) ha trovato 3 regressioni gravi, tutte nell'area ripresa/allarmi.
Corrette con una riprogettazione coerente (non toppe). `pio test` → **71/71**; build pulita (Flash 68.3%).
Andamento dei giri di review: 15 → 9 → 3 regressioni: convergenza in vista.

- **Falso «RAMPA FERMA» dopo la ripresa**: congelare la rampa al target (fix precedente) azzerava
  lo `span` del rilevatore di stallo C14 → allarme a 1 h durante un pull-down legittimo.
- **Allarme fuga termica: falso NEGATIVO**: il gate «banda raggiunta» (aggiunto per uccidere i
  falsi positivi sui pull-down) sopprimeva l'allarme per sempre se l'impianto non arrivava MAI
  (frigo morto dall'inizio dello step → birra calda per settimane, muta).
- **Ripresa RampTime-fromCurrent=false** non coperta (setpoint torna a startC, freddo abbandonato).

## Riprogettazione (unica, coerente)
- **`ProfileEngine::reanchorCurrentRamp(now, tempC)`** (nuovo): alla ripresa con orologio
  inaffidabile la rampa continua dalla temperatura REALE corrente (letta al primo tick valido dopo
  il boot), non dall'origine né dal target. Copre RampRate e RampTime (per fromCurrent=false
  aggiorna startC nella copia RAM). Lo `span` dello stallo torna significativo. main.cpp rinvia il
  ri-ancoraggio al primo tick valido (rebaseRampPending). + test `test_reanchor_ramprate_*` e `_ramptime_*`.
- **Allarme termico basato sul PROGRESSO** (sostituisce il gate binario): con setpoint stabile e
  uscite consentite, allarme se lo scarto |T-setpoint| è oltre 5 °C e non migliora di 0,5 °C da più
  di 3 ore. Copre l'impianto che non arriva mai (scarto fermo) E il guasto sopraggiunto (scarto che
  ricresce); un pull-down/cold-crash legittimo, anche lento, migliora e non fa scattare nulla.

---

# §7 — Quarto giro di review: regressioni corrette (riprogettazione ri-ancoraggio) — 2026-08-25

Il quarto fix-review (2 agenti, sui due meccanismi nuovi) ha trovato 3 regressioni, TUTTE nel
meccanismo reanchorCurrentRamp introdotto al terzo giro. Riprogettato in modo pulito.
`pio test` → **72/72**; build pulita (Flash 68.4%). Andamento review: 15 → 9 → 3 → 3.

## Regressioni (tutte nel ri-ancoraggio del terzo giro)
- **Profilo COMPLETATO su rampa**: il ri-ancoraggio si applicava anche ai completati, rompendo
  l'hold al target (la birra risaliva). Ora: un completato su rampa è congelato al target.
- **Step avanzato a non-rampa** durante la finestra di boot (sonda assente → gli step a tempo
  scadono): il ri-ancoraggio azzerava lo stepStartEpoch di uno step non-rampa (Fixed riavviato,
  finestra Gravity resettata). Ora: reanchorCurrentRamp ri-verifica il tipo al momento del consumo.
- **Mutazione di steps_[].startC**: rompeva l'invariante «copia RAM == profilo persistito»
  (/api/profile mostrava un startC di boot, un read-modify-write lo persisteva). Ora: l'origine
  ri-ancorata vive in `EngineSnapshot::rampReanchored` (usata da stepSetpoint), steps_ intatto.

## Riprogettazione
- `EngineSnapshot::rampReanchored` (nuovo, persistito): quando true, l'origine della rampa è
  stepStartTemp invece di startC. Azzerato da enterStep (nuovo step) e startProfile.
- `reanchorCurrentRamp`: bail se completato o se lo step corrente non è più una rampa; non tocca
  steps_. + test `test_reanchor_bails_on_completed_and_nonramp`.
- Blocco di ripresa (main.cpp): completato su rampa → congelato al target; rampa attiva →
  ri-ancoraggio al primo tick valido.

---

# §7 — Quinto giro (convergenza) — 2026-08-25

Quinto fix-review: 1 agente sul ri-ancoraggio riprogettato + 1 SWEEP CRITICO dell'intera
codebase corretta. Lo sweep ha trovato **un solo grave** (nel codice appena aggiunto) e
**nulla nel nucleo di controllo** — segnale di convergenza. `pio test` → **72/72**; build pulita.
Andamento dei giri di review: 15 → 9 → 3 → 3 → 1 grave + 2 minori.

## Grave corretto (dallo sweep)
- **Allarme fuga termica su step con attuatore disabilitato di proposito**: un free-rise
  (allowHeat=false) o crash naturale (allowCool=false) tiene la temperatura fuori banda nella
  direzione dell'attuatore vietato → falso allarme a 3 h. Fix: l'allarme scatta solo se
  l'attuatore che servirebbe a chiudere lo scarto è CONSENTITO dallo step (allowHeat/allowCool).

## Minori corretti (ri-ancoraggio)
- Ri-ancoraggio durante l'attesa di partenza ritardata (stepEndsAt cosmetico su step non
  iniziato): guardia `delayedStartEpoch != 0` in reanchorCurrentRamp.
- Stato ri-ancorato reso durevole subito: `persistNow()` dopo reanchorCurrentRamp (riduce la
  finestra in cui un reboot lo perderebbe). Residuo (sonda guasta attraverso boot→NTP→reboot):
  finestra strettissima e comunque in stato di allarme controlProbeFault — documentata.

## Convergenza
Lo sweep critico complessivo non ha trovato difetti nel nucleo (relè, guardia compressore,
interlock, isteresi, motore profilo): i residui dei giri 4-5 sono tutti nei meccanismi di
allarme/ripresa aggiunti di recente, ora corretti. Criterio d'arresto (§7): tetto dei giri di
review raggiunto (5) + rendimento decrescente nel nucleo. L'ultima correzione (gate attuatore
sull'allarme termico) è stata verificata per lettura del revisore, non da un ulteriore giro di
agenti — caveat onesto.

---

# Difetto da campo — "salvataggio fallito" / profili "{}" — 2026-08-25

Segnalato dall'utente sul dispositivo reale (v2.22.0): "Usa modello" a volte dà **"salvataggio
fallito"**, e in passato profili salvati comparivano in libreria **senza nome ("Profilo 3",
"Profilo 4")** pur essendo il modello originale non modificato.

## Ground truth (non dedotto)
Dump della LittleFS via USB (esptool read_flash 0x200000 +0x1FA000, unpack mklittlefs):
- FS **quasi vuota** (~51 KB usati su ~2 MB): NON è disco pieno.
- `profiles/p3.json` e `p4.json` = **`{}`** (2 byte): JSON valido, senza `name` né `steps` →
  `list()` li rende come "Profilo N". Sono i profili "senza nome".
- Heap libero via /api/status: **~17,6 KB**.

## Causa (verificata sul dispositivo)
Il percorso di salvataggio teneva **due `DynamicJsonDocument(6144)` vivi insieme**: quello di
parsing nel gestore HTTP + quello ri-costruito in `ProfileStore::writeFile`, più la copia del
body (~2 KB). Su heap ESP8266 frammentato (~17,6 KB) la **seconda** allocazione da 6 KB può
fallire → doc a capacità 0:
- firmware pre-revisione: `name`/`steps` scartati in silenzio → serializza **`{}`** → scritto
  "con successo" → profilo senza nome (p3, p4);
- firmware v2.22.0 (guardia `doc.overflowed()` aggiunta in revisione): stessa allocazione
  fallita → `writeFile` ritorna false → **"salvataggio fallito"** (errore onesto, niente
  corruzione). I due sintomi sono lo **stesso bug** in due stadi.
Intermittenza = firma della frammentazione (i miei PUT di prova riuscivano quando l'heap era
meno frammentato).

## Correzione (causa radice)
- `ProfileStore::writeFile` riscritto in **streaming**: serializza **uno step alla volta** con
  un doc minimo (`JSON_OBJECT_SIZE(14)+64`) più un doc da 256 B per l'escaping del nome; picco
  di heap < 1 KB invece di 6 KB. Rilevamento di troncamento preservato confrontando byte attesi
  (`measureJson` per pezzo) vs scritti; scrittura ancora atomica (tmp+rename). Estratto
  `stepToJson` come unico punto di verità dei 14 campi (usato anche da `stepsToJson` per la GET).
- `handleProfileById` (PUT) e `handlePutProfile`: doc di parsing racchiuso in un blocco così da
  **liberarlo prima** di `save()`/`saveActive()` → mai due grossi buffer JSON vivi insieme.

## Verifica (sul dispositivo, v2.22.1)
- Build pulita, **72/72** native.
- Round-trip **12 step** (caso peggiore) con nome insidioso `«tëst» \ "x" °C`: salvato ok,
  riletto con **nome identico** e **0 discrepanze** su 14 campi × 12 step → serializzatore
  streaming corretto, escaping incluso.
- Modello Nottingham (8 step) salvato **5×** di fila: tutti ok, rilettura integra.
- Bonifica: cancellati i `{}` (p3, p4) e gli artefatti di test. Libreria finale pulita:
  "British Ale" + "Ale pulita fresca (Nottingham)". Heap ~17,7 KB.

Caveat (§7): correzione verificata da round-trip reale e per lettura del revisore, non da un
ulteriore giro di agenti avversari. Non ho potuto forzare la frammentazione a comando: la prova
è che il picco di heap del salvataggio è ora strutturalmente ~6× più basso.

---

# Giro "cosa manca" — verifica sul campo + rastrellamento heap — 2026-08-26

Giro avversario (11 agenti) su: rastrellamento heap + le 3 correzioni recenti (allarme termico,
ri-ancoraggio, salvataggio profili). 6 rilievi → 3 seri → 2 sopravvissuti a doppia confutazione,
+2 escalation del critico. Poi 2 giri di fix-review sulle MIE correzioni (§7): il primo ha trovato
**5 regressioni nelle correzioni**, il secondo un **grave falso-negativo** nel mio secondo
tentativo. Andamento coerente con "le correzioni sono il codice meno affidabile".

## Corretto e verificato
- **[grave] handlePutConfig: stesso doppio doc(6144) dei profili.** Il doc di parsing restava vivo
  mentre ConfigStore::save ne allocava un secondo → salvataggi di CONFIGURAZIONE a rischio di
  fallimento/`{}` intermittente su heap frammentato. Fix: doc di parsing confinato in un blocco,
  liberato prima di save(). VERIFICATO sul dispositivo (v2.22.2): 3× round-trip config OK,
  configRev 1→4, ri-login con password OK (segreti preservati).

## Allarme termico — 3 tentativi prima di azzeccarlo (storia §7, istruttiva)
- Tentativo A (copertura RampTime + ri-arma su scarto d'ingresso): il fix-review ha bocciato con
  DUE falsi positivi — copertura RampTime che allarma su rampa a tempo aggressiva-ma-sana
  (scarto dal setpoint in MOVIMENTO confonde "guasto" con "rampa troppo veloce"); e ri-arma
  all'ingresso inefficace su attraversamenti graduali dei 5 °C.
- Tentativo B (riferimento = PICCO di scarto, devWorstGap): la ricerca mirata ha trovato un
  **grave falso NEGATIVO** — dopo una discesa (cold-crash 18→7,3) e un assestamento appena sopra
  5 °C (impianto sottodimensionato), il picco congelato fa sembrare ogni tick un progresso →
  l'allarme NON scatta mai. È il caso "sottodimensionato" che l'allarme deve proprio prendere.
- Tentativo C (ADOTTATO): riferimento = MINIMO di scarto DA QUANDO la finestra si è aperta
  (ri-armato allo scarto d'ingresso a ogni rientro sopra 5 °C). Durante una discesa reale tocca
  nuovi minimi e il timer si azzera (niente allarme); se poi si pianta sopra 5 °C smette di
  toccare minimi → allarme dopo 3 h. Le rampe (RampTime E RampRate) restano ESCLUSE come
  nell'originale. Recheck finale mirato (2 agenti, cacciatore-FP + cacciatore-FN): **0 rilievi**.
  I "falsi positivi" segnalati sui tentativi precedenti erano casi NET-peggiori-in-3h (allarme
  legittimo), non progresso reale: giudizio del revisore, non accettazione cieca del rilievo.

## Ri-ancoraggio rampa in pausa — regressione mia, annullata
- Avevo aggiunto `if (s_.paused) return;` a reanchorCurrentRamp. Il fix-review l'ha bocciato come
  **grave**: reanchor è chiamata SOLO al boot, dove pausedAtEpoch è già ri-basato insieme a
  stepStartEpoch (niente desync), e saltare il ri-ancoraggio lascia la rampa a ripartire
  dall'origine STANTIA → alla ripresa RISCALDA la birra al vecchio start. ANNULLATO. Il test è
  stato riscritto per bloccare il comportamento CORRETTO (reanchor funziona anche in pausa).

## Residui documentati (NON corretti in questo giro, con motivazione)
- **Rampe a TEMPO senza rilevamento di fuga termica**: un fix corretto richiede un rilevatore sul
  TREND della temperatura della birra (non sullo scarto dal setpoint in movimento). Rimandato:
  cambio più grosso, non testabile a banco, verifica su campo impraticabile (finestra 3 h).
- **close()/flush che scarta l'esito di scrittura** in tutti e 6 gli store (ProfileStore, Config,
  Calib, State, Devices, Boot): probabilità bassissima (FS pieno all'ultimo flush) su FS quasi
  vuota; irrobustimento coerente (verifica size dopo close) rimandato a un giro dedicato+testato.
- **Gestore comandi MQTT (main.cpp ~791-830)** non auditato a fondo: superficie di mutazione
  parallela agli handler HTTP; da coprire in un giro dedicato.
- **RampRate finale con maxDurationSec scaduto sotto target a profilo COMPLETATO**: niche
  (maxDurationSec solo via import JSON), pre-esistente, non coperto da alcun allarme.

## Stato
Build pulita, **73/73** native (+1 test ri-ancoraggio riscritto). Firmware **v2.22.3** sul
dispositivo, TUTTE le correzioni attive. Config-fix verificato sul campo; allarme termico
verificato per lettura + recheck avversario (0 rilievi) + trace manuale del revisore.

---

# Campagna "production-ready" — Round 1 (nucleo sicurezza) + Round 2 parziale (I/O) — 2026-08-26

Su richiesta: cicli di revisione fino a production-ready dell'INTERO progetto. NIENTE flash
(cotta Kveik in corso); solo build+test, flash rimandato all'approvazione dell'utente.

## R1 — nucleo sicurezza (control pipeline, PID/predittivo, HAL/sonde, main.cpp)
4 finder + doppia confutazione + critico. 1 grave sopravvissuto + 2 gravi dal critico. Corretti,
poi fix-review (§7) che ha trovato **2 REGRESSIONI GRAVI nelle mie correzioni** — annullate/rifatte:

- **[grave] Sonda di controllo congelabile da polling diagnostico.** GET /api/diag/onewire ripetuto
  annullava la conversione in corso e rinnovava la grazia → sonda di controllo valid=true con valore
  fermo, nessun fault, nessun allarme → perdita SILENZIOSA del controllo. FIX (finale): **rate-limit
  a 2 s** dell'endpoint (scan()/tempById() al più ogni 2 s, cache dell'elenco; le letture live sempre
  fresche). — Il mio primo tentativo (tetto assoluto d'anzianità in ProbeBus) è stato BOCCIATO dal
  fix-review: faceva falsi controlProbeFault su ogni stallo legittimo del loop >60 s (OTA, handler
  lungo) — proprio ciò che la grazia di stallo esiste per tollerare. ANNULLATO; il rate-limit da solo
  basta (le letture completano, la sonda resta genuinamente fresca).
- **[grave] Allarme fuga termica azzerato da ogni fault sonda.** Una sonda di controllo marginale che
  sfarfalla (~15 s ogni paio d'ore) resettava il timer di 3 h → l'allarme non maturava mai. FIX
  (finale): timer riscritto come **durata accumulata** (devStallMs += dt sui soli tick valutabili),
  che a sonda invalida semplicemente NON accumula (congela) senza buttare il tempo, ed è immune al
  rollover di millis(). — Il mio primo tentativo (devNoProgressMs += dtMs in un ramo `valutabile &&
  !ctl.valid`) era CODICE MORTO: `valutabile` include outputsAllowed()=!controlProbeFault, quindi il
  ramo era irraggiungibile e il fault cadeva nel reset. Il fix-review l'ha colto; rifatto.
- **[minore] Età di fermentazione assurda** in /status se il profilo è avviato a orologio
  inaffidabile: aggiunta la guardia `>1.5e9` (come MqttSvc).

Residui R1 (documentati, non corretti): **sonda di controllo single-point-of-failure** (nessun
cross-check con le ausiliarie né taglio su temperatura misurata — richiede DECISIONE dell'utente:
un cross-check ha rischio di falsi scatti su una cotta viva); nessun failsafe hardware-WDT su stallo
del loop; tempo morto Interlock non persistito (minore); modello di deriva PeakEstimator (minore
auto-correggente); discontinuità fase PID al rollover (minore documentato).

## R2 — I/O (persistenza + web + mqtt) — PARZIALE (limite di sessione: stores/web-A/critico da rifare)
Completati solo i finder web-B e mqtt. 1 grave:
- **[grave] handleThermostat: setpoint validato solo con on=true ma memorizzato sempre.** POST
  /api/thermostat {on:false, setpoint:1e40} → memorizza inf → /status emette `"thermostatSetpoint":inf`
  = JSON NON valido → UI cieca, e persino persistito. FIX: spegnendo si tiene il setpoint CORRENTE
  (già valido), non quello del client; l'accensione l'ha già validato. (Gemello del guard isfinite
  di handleSetpoint.)
Minori R2 (documentati): comandi MQTT skip/climate_temp che chiamano persistNow su no-op (usura
flash; broker-authed, wear-leveled); climate_mode=auto senza range-check (attuazione COMUNQUE
clampata [-1,40] a main.cpp:429-430 → solo divergenza di display); publishDiscovery sensor/binary/
button senza guardia overflowed() (bassa prob.); sentinella owScanMs_==0 (trascurabile).

## Stato
Build pulita, **73/73** native. NON flashato. DA FARE dopo il reset di sessione: rifare i finder R2
mancanti (stores incl. residuo close()/flush; web-A; critico I/O); fix-review del NUOVO codice
termico ad accumulo + del fix handleThermostat; R3 (servizi/frontend/residuo termico RampTime).

---

# Campagna production-ready — R2-completion (persistenza/boot) + refinements termostato — 2026-08-26

R2-completion (finder stores + web-A + critico I/O, rieseguiti dopo il reset del limite): 4
grave/critico sopravvissuti + 3 grave dal critico. Un cluster attorno alla robustezza della
persistenza. Corretti (build pulita, 74/74 native; +1 test clamp termostato):

## Corretto
- **[critico] Flush finale non verificato in TUTTI e 6 gli store** (Config/State/Calib/Profile/
  Devices/Boot). `n != want` non copre l'esito di File::close()->lfs_file_sync (scartato): un tmp
  troncato dall'ultimo flush poteva essere promosso da rename → al boot load fallisce → stato/config/
  profilo persi in silenzio. FIX: helper `fsCommitAtomic` (src/svc/FsCommit.h) — ri-apre il tmp,
  verifica size==want committata su flash, poi rename; usato da tutti e 6 gli store. Aggiunte anche
  le guardie `doc.overflowed()` mancanti a State/Devices/Boot.
- **[grave] state.json / profile.json corrotti al boot abbandonano la cotta in SILENZIO.** Il load
  ne ignorava l'esito e non distingueva assente da corrotto (a differenza della config). FIX:
  `bootStorageFault` alzato se il file ESISTE ma non si legge (per il profilo, solo se lo stato dice
  mode==Profile) → confluisce in `safety.storageFault` (allarme su tutti i canali).
- **[grave] Il DataLog può riempire la LittleFS** — sia per campionamento fitto x retention alta
  (il commento "90 giorni sotto il limite" era falso), sia con orologio inaffidabile (campioni in
  /log/d0,d1,… e retention-per-giorni inefficace, crescita illimitata). FIX: tetto ASSOLUTO
  `kMaxTotalBytes=1,4 MB` in cleanup() — cancella il giorno più vecchio finché il totale rientra,
  indipendente da giorni e orologio → il log non riempie mai la FS (che affamerebbe i save).
- **[grave] DataLog::append ingoiava l'esito del commit**: a FS piena ritornava true (byte
  bufferizzati) → logFault falso → l'allarme "controlla lo spazio" non scattava mai. FIX: append
  ri-apre e verifica che la dimensione sia cresciuta del record.
- **[grave] handleThermostat non protegge l'OFF a profilo in corso.** setThermostat azzera lo
  snapshot per ON *e* OFF: un POST {on:false} durante una cotta la abbandonava (step/OG/batch persi),
  persistito. FIX: la guardia 409 ora blocca ENTRAMBI i versi a profilo in corso (per fermare si usa
  lo stop del profilo).
- **[minore, dal fix-review del batch precedente] setThermostat non clampava**: aggiunto clamp
  [-1,40] in ProfileEngine::setThermostat (difesa in profondità per MQTT auto/migrazione; chiude
  anche il grave "off con inf" senza rompere "off + nuovo setpoint") + test. E cap dell'accumulatore
  devStallMs alla soglia (niente wrap a 49,7 giorni).

## Residui documentati (R2/critico, non ancora corretti)
- Dedup di StateStore vanificata da lastSeenEpoch (usura flash su no-op, wear-leveled) — minore.
- Schema non versionato negli store (minore, latente).
- handleIspindel senza auth se token vuoto (minore); OTA non forza outputs.allOff() prima
  dell'update (minore) — da valutare in un giro dedicato.
- Sonda di controllo single-point-of-failure (R1) — richiede DECISIONE utente.

Stato: build pulita, 74/74. NON flashato (cotta in corso). In corso: fix-review del batch
persistenza + R3 (servizi + frontend).

---

# Campagna production-ready — Fix-review persistenza + R3 (servizi/frontend) — 2026-08-26

## Fix-review del batch persistenza: 6 regressioni (2 gravi), corrette
- **[grave] fsCommitAtomic distruggeva un save valido su OOM.** La ri-apertura del tmp alloca heap;
  su OOM (~17KB) il ramo !v cancellava il tmp GIA' scritto e ritornava false → save perso. FIX: su
  ri-apertura fallita NON si cancella e si promuove comunque (la verifica è best-effort; scarta solo
  un tmp DIMOSTRATAMENTE troncato).
- **[grave] DataLog::append verify → falso allarme storage su OOM.** Stessa trappola (3ª apertura).
  FIX: append tornato alla forma semplice (return n==sizeof). Residuo (deferred-flush) documentato,
  mitigato dal tetto di dimensione + dagli allarmi dei save config/stato.
- **[minore] Tetto DataLog gated solo a cambio giorno** → con orologio fermo (day=0) non girava mai.
  FIX: cleanup ora anche a trigger ORARIO (millis). + setThermostat clamp NaN-safe (test in forma di
  appartenenza) + cap accumulatore devStallMs (già).

## R3 — servizi + frontend: 1 survivor grave + 1 grave (meccanismo) + critico
- **[grave, SAFETY] Correzione NTP piccola (2..60 s) bypassava lo shiftClock delle àncore** → riposo
  FANTASMA nella guardia compressore/isteresi → corto ciclo del compressore col riposo minimo vicino
  al limite. FIX: i drift piccoli passano ora per pendingJumpSec_ (come i grandi), così base_ e le
  àncore saltano insieme. Stessa cosa in setManual. (Confermato dal confutatore tecnico; demolito
  sull'impatto solo per dipendenza dalla config, ma il meccanismo è reale → corretto.)
- **[grave, frontend] configRev stantio dopo la taratura sonde** → falso 409 al salvataggio
  successivo → loadConfig() cancellava le modifiche nel form. FIX: la risposta di /probe/calib ora
  include configRev, e il frontend allinea CFG.configRev.
- **[minore] gravityStaleMin=0** (campo svuotato) → densità sempre stantia → alarmGravity permanente.
  FIX: clamp >=1. **[minore] OledUi**: new senza null-check → deref di nullptr su OOM. FIX: guardia.

## Residui documentati (da valutare / decisione utente)
- Sonda di controllo single-point-of-failure (R1) — DECISIONE utente (cross-check ha rischio falsi
  scatti su cotta viva).
- Libreria profili SENZA locking ottimistico (la config ce l'ha) → due client si sovrascrivono
  (device tipicamente mono-utente) — da valutare.
- Nessun timeout/AbortController sul fetch del frontend → banner "connessione persa" può non comparire
  su connessione half-open — minore.
- softApAfterMin=0 (campo svuotato) → AP subito; frontend che manda 0 per campi numerici vuoti
  (causa comune) — da irrobustire lato UI.
- Dedup StateStore vanificata da lastSeenEpoch (usura no-op, wear-leveled); schema non versionato;
  handleIspindel senza auth se token vuoto; OTA non forza outputs.allOff() — minori.

Stato: build pulita, 74/74. NON flashato (cotta in corso). In corso: fix-review di questo batch.

---

# Fix-review batch persistenza+R3 — 2 regressioni, risolte — 2026-08-26

- **[grave] fsCommitAtomic — tensione fra due fix-review.** v1 (fallire su OOM) poteva perdere un
  save valido; v2 (promuovere su OOM) poteva promuovere un tmp TRONCATO in silenzio (OOM + FS piena).
  RISOLTO con scelta FAIL-SAFE: su ri-apertura fallita NON si promuove e si ritorna false (allarme,
  ritentabile); mai promuovere non verificato. Il falso fallimento è quasi impossibile (il chiamante
  ha appena allocato un doc da KB → la ri-apertura da centinaia di byte ha spazio). Giudizio del
  revisore fra due rilievi in conflitto: la corruzione silenziosa è il male peggiore su cotta non
  presidiata.
- **[minore, ACCETTATO] I drift NTP piccoli ora fanno persistNow+shiftClock a ogni correzione.** In
  pratica ~1 volta/giorno (deriva quarzo <2 s/ora): usura trascurabile. La correttezza (àncore
  traslate) prevale sul churn teorico di una sorgente NTP patologica. Documentato, non modificato.

Convergenza: le ultime correzioni chiudono guasti sempre più di bordo; il nucleo di controllo e la
persistenza sono hardened. Avvio un ultimo giro di completezza (auth/OTA/sicurezza + math di
controllo) per confermare la convergenza, poi sintesi.

---

# Giro di CONVERGENZA (R4) — auth/OTA/sicurezza + math di controllo — 2026-08-26

Ultimo giro: 2 finder (sicurezza/OTA poco coperti + passata PROFONDA sulla matematica di controllo)
+ critico a livello di campagna. La passata sul math di controllo NON ha trovato nulla di serio =
segnale di convergenza. Il critico ha dichiarato **"convergenza raggiunta"** su nucleo controllo/
sicurezza/persistenza/web, con un solo residuo grave (sotto).

## Corretto
- **[grave] Tempi di controllo senza tetto superiore.** Un PUT /api/config (script non soggetto ai
  limiti UI) con minStateSec/minSwitchSec/compressorMinOnSec enorme (fino a 65535) inchioderebbe un
  relè per ore: il latch d'isteresi/interlock/guardia non torna indietro nemmeno oltre il setpoint →
  frigo o riscaldatore bloccato ON → birra congelata/cotta, e nessun percorso taglia l'uscita
  (thermalDeviation è solo allarme). È "step senza uscita garantita" in forma di config. FIX: clamp a
  600 s (ampio per anti-chatter, limita l'overshoot a ~10 min). Coerente col clamp compressorRestMin>=1.
- **[grave→demolito sull'impatto, ma corretto come igiene] OTA non metteva in sicurezza le uscite.**
  Durante il flash il loop è congelato: i relè restavano latchati (es. riscaldatore ON) senza
  supervisione per upload+reboot. Impatto reale modesto (massa termica di ore, azione rara/autenticata),
  ma evitabile: FIX `r_.outputs->allOff()` prima di Update.begin (coerente con l'interlock "disarma
  prima" di test relè/taratura).
- **[minore/latente] Choke point del setpoint non NaN-safe.** Il clamp `<-1 / >40` non normalizzava
  NaN (che latcha i relè). Oggi non raggiungibile (tutte le sorgenti rifiutano NaN), ma il choke point
  ora è autosufficiente: `if (!(spAtt >= -1)) spAtt = -1;`.

## Residui minori documentati (non corretti)
- factoryResetPending copre persistNow/devices ma non i save dai gestori web nella finestra di ~1 s
  prima del reboot (un PUT concomitante potrebbe ricreare un file) — minore, finestra strettissima.
- Log-CSV sincrono congela il loop come l'OTA ma è read-only e delimitato — minore.

## Verdetto di convergenza (dal critico di campagna)
"NON ho trovato un difetto critico/grave NUOVO che morda in operatività normale. I percorsi centrali
sono convergenti e coerentemente difesi." Nucleo controllo/sicurezza/persistenza/web: **convergenza
raggiunta**. Restano le 2 decisioni di progetto (sonda-SPOF, locking libreria profili) e i minori
documentati. Build pulita, 74/74. NON flashato (cotta in corso).

# Fix-review R4 — 2 regressioni, risolte — 2026-08-26
- **[grave] Il mio allOff() dell'OTA desincronizzava la guardia compressore** → su OTA ABORTITA
  (nessun reboot) la guardia con on_ stantio rieccitava subito il compressore → corto ciclo. Ho
  ANNULLATO l'allOff(): il freeze OTA (residuo minore, demolito sull'impatto) è meno grave del corto
  ciclo. La guardia resta coerente.
- **[minore] Il tetto a 600 s troncava config legittime** (anche la UI non ha min/max) e mancava il
  tetto su compressorRestMin. FIX: tetto alzato a 1800 s (30 min, copre glicole ecc.) + tetto
  compressorRestMin a 60 min.

CONVERGENZA CONFERMATA. Fine campagna di revisione del codice: nucleo controllo/sicurezza/persistenza/
web/servizi/frontend passati a fondo con giri avversari + fix-review a decrescere. Residui: 2 decisioni
di progetto (sonda-SPOF, locking libreria profili) + minori documentati. Build pulita, 74/74. Firmware
rivisto NON ancora flashato (cotta in corso).

---

# Decisioni utente + FLASH — 2026-08-26

Decisioni dell'utente sui 2 residui di progetto:
1. **Sonda SPOF → cross-check SOLO-ALLARME (scelto).** Implementato: se la sonda di controllo è
   valida ma diverge dall'ausiliaria PIÙ VICINA di >15 °C per 30 min accumulati → allarme
   `controlProbeSuspect` (NON taglia le uscite). Il "da tutte le ausiliarie" (minDiff) evita falsi
   scatti quando una sola ausiliaria diverge (pull-down camera). Wiring: SafetyMonitor/AppStatus/
   buildStatus/bit-log/messaggio-status; via anyAlarm arriva a OLED/MQTT.
2. **Flash → SUBITO (scelto).** La cotta Kveik NEL FRATTEMPO era stata FERMATA dall'utente
   (mode=0, running=False): nessuna fermentazione da disturbare. Flashato **v2.23.0** (l'intera
   campagna di revisione). Verificato sul dispositivo: fw=2.23.0, heap ~17,8 KB, 3 sonde valide,
   **0 allarmi** (cross-check non scatta, sonde entro 1 °C).

Locking libreria profili: lasciato come limite documentato (device mono-utente; la config ha già il
locking per il caso taratura-race).

Firmware rivisto ORA SUL DISPOSITIVO (v2.23.0). In corso: fix-review del cross-check (ultimo pezzo
nuovo; alarm-only → re-flash a basso costo se serve, nessuna cotta in corso).

# Fix-review del cross-check — 7 rilievi, corretti — 2026-08-26
Il fix-review del cross-check (ultimo pezzo nuovo) ha trovato 7 problemi, a conferma che ANCHE una
feature semplice richiede la verifica avversaria:
- **[grave] Falso positivo con UNA sola ausiliaria** ("da tutte" degenerava in "dall'unica"). FIX:
  richiede nAux>=2 (con <2 il cross-check si disattiva, degrada in sicurezza).
- **[grave] Falso negativo sul caso bersaglio**: se la sonda di controllo scivola in camera e legge
  ~la sonda camera, minDiff resta piccolo → non scatta. È un LIMITE FONDAMENTALE del confronto fra
  sonde: documentato nel codice; rimedio reale = fissare la sonda del mosto nel pozzetto.
- **[grave] Wiring MQTT + OLED**: avevo assunto "arriva via anyAlarm()", ma anyAlarm() non è chiamato
  da nessuno. FIX: alarmProbeSuspect aggiunto al doc["alarm"] MQTT e alla cascata OLED.
- **[minori] Accumulatore senza cap** (wrap a 49,7 giorni) + **reset invece di congelamento** sulla
  sonda invalida. FIX: cap alla soglia + congelamento (come l'allarme termico).

Flashato **v2.23.1** (cross-check corretto). Device verificato: fw=2.23.1, 0 allarmi. In corso:
re-check di conferma delle correzioni del cross-check.

# Re-check cross-check + chiusura — 2026-08-26
Re-check delle correzioni del cross-check: 7/7 risolti, 1 solo MINORE nuovo (il ramo nAux<2 azzerava
invece di congelare). CORRETTO: ora congela anche con <2 ausiliarie (coerente col congelamento su
sonda invalida). Flashato **v2.23.2** (FINALE). Device verificato: fw=2.23.2, heap ~17,6 KB, 3 sonde
valide, 0 allarmi.

## CHIUSURA CAMPAGNA
Revisione del codice CONVERGUTA e firmware rivisto SUL DISPOSITIVO (v2.23.2). 74/74 native. Restano:
i minori documentati (locking libreria profili multi-client, timeout fetch frontend, versioning
schema, ecc.) e la VALIDAZIONE SUL CAMPO (osservare una cotta reale col firmware nuovo).
