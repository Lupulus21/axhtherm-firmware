# Verdette indipendenti del revisore (da confrontare coi confutatori)

Formate leggendo il codice riga per riga, prima di vedere i verdetti della Fase 3.
Servono a cogliere disaccordi coi confutatori (punto cieco in entrambe le direzioni).

## Critici

- **C01 (/ispindel injection)** — CONFERMO. `handleIspindel` (WebApi.cpp:713) non chiama `auth()`.
  Default `ispindelToken=""`, `gravityDevice=""`. Un POST con gravity plausibile entra in
  `gravity.update`; al tick `gravityConditionMet` Below avanza lo step. Reale, critico.
  Nota impatto: il modello di minaccia LAN/HTTP è "accettato", ma il difetto scatta anche con
  un SECONDO iSpindel legittimo mal filtrato (gravityDevice vuoto = accetta il primo). Regge.

- **C08 / C09 / C13 (cluster MCP23008)** — ATTESO DISCORDE. Tecnicamente reali (nessun readback
  di IODIR/OLAT; failsafe non riapre un relè latchato). MA ARCHITETTURA.md §9 dice esplicitamente
  che i carichi reali (frigo+riscaldatore) stanno su REL1-3 (GPIO diretti), non sul MCP. Su GPIO
  `writeRel` usa `digitalWrite`, che non "fallisce" come l'I2C. Il difetto morde SOLO se l'utente
  mappa i 230 V su REL4-8, mapping che la doc sconsiglia. → confutatore tecnico: reale;
  confutatore d'impatto: probabile "non reale/ridimensionato". Da leggere a mano.

- **C12 (salti orologio non traslano latch isteresi/interlock)** — CONFERMO. main.cpp:710-713
  trasla engine/guard/gravity ma NON `HysteresisController` né `Interlock` (niente shiftClock,
  la pipeline non li espone). `evaluate`/`arbitrate` ricevono `nowEpoch`. Salto all'indietro di Δ:
  `elapsed(now, coolChangedAt_)` = -Δ < minStateSec → il latch non può cambiare per ~Δ. Frigo
  congelato acceso oltre setpoint per l'ampiezza del salto. Reale, grave→critico. 3 assi.

- **C14 (RampRate senza uscita)** — CONFERMO. `stepDone` RampRate richiede sp==target E tempC in
  target±tol; nessun `maxDurationSec` (solo Gravity), nessun allarme (buildStatus noLimit/gravity
  solo per Gravity). La libreria stock ha decine di ramp_rate. Il test 317-321 sancisce l'attesa
  infinita come atteso. Reale, critico. 3 assi.

- **C15 (gravityUnit "solo etichetta" governa la logica)** — CONFERMO. Usata in main.cpp:328/368,
  WebApi.cpp:739-741, coldcrash. Cambio sg↔plato a cotta in corso: la lettura cambia scala mentre
  gravityTarget resta nella vecchia → avanza all'istante o si blocca. Reale, critico.

- **C21 (file troncato promosso)** — CONFERMO. ConfigStore.cpp:24 e ProfileStore writeFile:98
  controllano `n==0`, non `n==misura`. Contrasto: StateStore/CalibStore/BootLog/DeviceRegistry
  controllano `written!=want`. Profilo attivo troncato → al boot loadActive fallisce → engine
  senza step → mode Off → cotta senza controllo, in silenzio. Reale. Severità sfumata (config
  troncata è fail-safe: valid=false→uscite off+allarme; profilo troncato è peggio).

- **C29 (export log stallo controllo)** — CONFERMO. handleLogCsv/Json in web.loop() prima di
  controlTick; forEach yield SOLO per file-giorno, non per record; sendContent blocca su
  backpressure TCP. Client che smette di leggere → controllo fermo per minuti, relè congelati.
  Reale, grave→critico. 2 assi.

- **C31 (salto orologio stesso giro)** — CONFERMO (archetipo 10). setManual sposta base_ subito
  ma consumeClockJump (loop:704) è GIÀ passato prima di web.loop (719). controlTick (723) nello
  stesso giro gira con nowEpoch nuovo e ancore vecchie → il while(stepDone) fast-forwarda il
  profilo a COMPLETED (o la guardia vede elapsed enorme). Persistito. Poi shiftClock corregge
  ancore già bruciate. Reale, critico.

- **C32 (comando MQTT retained rieseguito)** — CONFERMO (da verificare PubSubClient). La callback
  non riceve il flag retain (firma topic/payload/len); risottoscrizione a cmd/+ a ogni reconnect
  → il broker riconsegna il retained → riesecuzione (start→azzera OG) a ogni riconnessione WiFi.
  Reale; richiede che l'utente pubblichi un comando retained (errore plausibile).

## Gravi (verdetto rapido; delego il dettaglio ai confutatori)
- C02 usura devices.json da LAN: reale (auth assente, save per ID nuovo). Impatto: usura lenta +
  sfratto densimetri. Regge, forse ridimensionabile a minore su "usura" ma non su "sfratto".
- C16 climate_temp toFloat()→0: reale. C17 freeze-guard -1 vs -10: reale (coldcrash floora a -1,
  setpoint/thermostat/MQTT a -10). C24 config mutata prima del save senza rollback: reale.
- C22 retention senza tetto, C23 esiti scrittura ignorati, C28 StepDef[12] su stack,
  C30 profilo=ASSENTE su OOM, C35 soglie attenuazione convertite in °P, C36 densità °P negativa
  in uint16, C37 dashboard congelata: reali dalla lettura; confidenza sui dettagli ai confutatori.
- C18 OG in pausa: reale (engine.running() vero anche in pausa). C42 test regressione sonde: reale.

## Minori: prevalentemente reali ma recuperabili; C33/C34/C40/C41/C43 attesi confermati minori,
## C26 (profileStartEpoch→2081) reale ma cosmetico, C25 finestra factory-reset reale ma raro.
