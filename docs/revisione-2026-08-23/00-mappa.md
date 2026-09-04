# MAPPA DI RICOGNIZIONE — AxHTherm 2.5 (Fase 1)

Copertura: letti PER INTERO `platformio.ini`, `src/main.cpp` (751), `docs/ARCHITETTURA.md`, tutti i 12 file di `lib/axcore/src/ax/`, `include/hardware_map.h`, tutti gli header di `src/hal/` e `src/svc/` elencati nel mandato, più `WebApi.cpp` (registrazione rotte, auth, handler principali: righe 1-260, 311-1069, 1160-1434), `StateStore.cpp`, `TimeService.cpp`, `DataLog.cpp/.h`, `BootLog.h`, `CalibStore.h`, `DeviceRegistry.h`, `NetworkManager.h`, `OledUi.h`. Letti solo in parte: `WebApi.cpp` (righe 260-310 e 1070-1160: corpo di handleStatus/handleProbeCalib), `ConfigStore.cpp`, `ProfileStore.cpp`, `MqttSvc.cpp`, `NetworkManager.cpp`, `OledUi.cpp` (solo grep mirati). NON letti: `data-src/index.html`, `test/test_main.cpp`, `tools/gzip_assets.py`, librerie in `.pio/libdeps`.

---

## 1. Architettura delle chiamate

### setup() — main.cpp:522-691, ordine esatto
1. `outputs.safeInitEarly()` (523) — GPIO relè 1-3 LOW **prima di tutto**
2. Serial, `Wire.begin` @100 kHz (529-530)
3. `mcp.begin(0x20)` → `outputs.attachMcp` → `outputs.allOff()` (532-535)
4. `LittleFS.begin()` → `fsOk` (537)
5. `bootLog.load()` + `record(resetReason, 0, 0)` (543-545) — epoch=0, timbrato dopo
6. `cfgStore.load(&cfg)`: assente→crea default; corrotta→default+`valid=false` (551-561)
7. `probes.begin()` (563), `rtc.begin()` (565)
8. `stateStore.load(&pstate)` (568) → `timeSvc.begin(rtc, pstate.lastSeenEpoch, …)` (569) — verifica monotonia RTC vs epoch persistito
9. `applyConfigLive()` (573) — configura probes, pipeline, engine, gravity/gravityAny
10. `profStore.loadActive()` → `engine.setSteps` → clamp `stepIdx`; profilo sparito → `mode=Off` (578-583) → `engine.restore(pstate.engine)` (584)
11. `pipeline.guard().restore(pstate.guard, false)` (587: "al boot i relè sono comunque caduti") + `setEstimators`; `coolWasOn` → riposo pieno (589-594); orologio non trusted o `!epochsTrusted` → `assumeJustStopped` + ri-base timeline profilo + azzera finestra stabilità (595-609)
12. `outputs.setMapping(pstate.coolRel, pstate.heatRel)` (613) — il mapping torna, ma l'**armed** vive solo in `pstate.armed`/`safety.notArmed`
13. `dataLog.begin()`, `calibStore.load()`, `devices.load()` (618-620)
14. `net.begin(...)` (622), `oled.begin` (625), `mqtt.begin` + `setCommandHandler` (628-657: comandi climate_temp/climate_mode/profile, poi `curStatus = buildStatus()`)
15. `web.begin(refs)` (659-687) — 25 puntatori/lambda in `WebApi::Refs`

### loop() — main.cpp:693-751, ordine e cadenze
| # | Chiamata | Cadenza effettiva |
|---|---|---|
| 1 | `probes.loop(nowMs)` (696) | FSM: request ogni 5 s, lettura 1 s dopo (kConvMs=1000, margine 250 ms su 750); ordine sonde **a rotazione** (ProbeBus.h:155-201); stallo >15 s → grazia 20 s per sonda |
| 2 | `timeSvc.loop(staUp)` (697) | check SNTP ogni 10 s; scrittura RTC max ogni 6 h |
| 3 | `bootLog.stampEpoch` (700-703) | una volta, appena trusted |
| 4 | `consumeClockJump()` → `engine/guard/gravity/gravityAny.shiftClock` + `persistNow` (704-717) | su salto >60 s |
| 5 | `net.loop`, `web.loop` (**handler HTTP girano qui**), `outputs.loop` (timeout test 10 min, retry MCP 5 s), `serialPoll` (718-721) | ogni giro |
| 6 | `controlTick(nowMs)` + `curStatus = buildStatus()` (723-727) | ogni 1000 ms |
| 7 | `oled.loop` (729) | draw max 1/s |
| 8 | `mqtt.loop` (730) | reconnect 30 s, publish 15 s |
| 9 | checkpoint `persistNow()` (732-735) | 300 s |
| 10 | `logSample()` (736-739) | `cfg.logSampleMin` min (default 5); cleanup al cambio giorno |
| 11 | log heap (740-744) | 60 s |
| 12 | reboot schedulato: `persistNow()` (salvo factory reset) + `ESP.restart()` (745-748) | quando scade |
| 13 | `delay(2)` (750) | nutre il soft-WDT |

### controlTick (1 s) — main.cpp:307-350
`timeSvc.nowEpoch()` → `probes.control()` → aggiorna i 5 flag di `safety` → costruisce `TickInput` (con attenuazione calcolata QUI, non nel motore, 324-335) → `engine.tick(in)` → `safety.gravityStale = lastTick.waitingGravity` → `pipeline.tick(now, nowMs, …, safety.outputsAllowed(), **1.0f**, allowHeat, allowCool)` (339-341) → `outputs.applyAutomation(heat, cool)` (342) → su `stepChanged || profileCompleted || cool!=prevCool` → `persistNow()` (344-349).

Dentro `pipeline.tick` (ControlPipeline.h:65-144): fail-safe (tutto spento se !allowed/!setpoint/!tempValid, con `guard.update(now,false)` e reset stimatori) → PeakEstimator su stato fisico precedente → isteresi (+PID caldo) → stop predittivo via `forceCoolOff/forceHeatOff` → politiche allowHeat/allowCool → priorità freddo → `Interlock.arbitrate` → `CompressorGuard` (mustStayOn/canStart) → `guard.update`/`lock.update` → cancelObservation incrociate.

Catena comandi: HTTP (`WebApi` handler, dentro `web.loop()`) e MQTT (lambda in setup) e seriale (`handleSerialLine`) convergono tutti su `engine.*`, `startProfileCmd`, `armOutputs/disarmOutputs`, `persistNow` — **tra un controlTick e l'altro**, mai in mezzo (cooperativo, single-thread).

---

## 2. Dove vive lo stato

### Globali in main.cpp (righe 31-66)
| Oggetto | Riga | Scritto da | Letto da |
|---|---|---|---|
| `mcp, outputs, probes, rtc` | 31-34 | setup, controlTick, handler diag/relay, seriale | pipeline (via applyAutomation), buildStatus |
| `timeSvc` | 35 | loop (NTP), handleTime, seriale `ora` | ovunque (nowEpoch) |
| `bootLog`, `bootStamped` | 36-37 | setup, loop | handleProbeDiag |
| `net` | 38 | setup, loop | buildStatus, mqtt.loop, timeSvc.loop |
| `cfgStore`, `cfg` | 39-40 | setup, **handlePutConfig (muta cfg in place, 328-336)** | applyConfigLive, handler, buildStatus, onIspindel |
| `stateStore`, `pstate` | 41-42 | `persistNow` (70-81, fotografia engine+guard+outputs), armOutputs/disarmOutputs (armed), startProfileCmd (batch, og), onIspindel (og), setOgGravity | setup (ripresa), buildStatus, `refs.isArmed` |
| `profStore, calibStore, devices, dataLog` | 43-46 | handler web, onIspindel (devices.save) | handler, logSample |
| `oled, mqtt, web` | 47-49 | setup | loop |
| `engine` | 50 | comandi (start/stop/pause/resume/skip/override/setThermostat), setup restore, shiftClock | controlTick, buildStatus, handler |
| `pipeline` | 51 | applyConfigLive, setup (guard restore, estimators), shiftClock | controlTick, buildStatus |
| `gravity, gravityAny` | 52-53 | onIspindel, shiftClock | controlTick, buildStatus, handleCalib* |
| `safety` | 54 | controlTick (5 flag), armOutputs/disarmOutputs (notArmed) | pipeline (outputsAllowed), buildStatus |
| `curStatus` | 56 | loop (1/s, dopo controlTick) e handler MQTT (656) | handleStatus, oled, mqtt, logSample, seriale `stato` — **cache vecchia fino a 1 s** (WebApi.h:41) |
| `activeProfileName, lastGravityDevice` | 57-58 | startProfileCmd, handlePutProfile, coldcrash, onIspindel | buildStatus, handler |
| `lastTick, lastPipe, prevCool` | 59-61 | controlTick | buildStatus, onIspindel (waitingDelayedStart) |
| `batteryLow, fsOk, rebootAtMs, lastCtlMs/CkptMs/LogMs/HeapMs, lastCleanupDay, factoryResetPending` | 62-66, 202 | vari | loop |
| `static ax::StepDef steps[]` in startProfileCmd | 134 | **static locale**: "niente array sullo stack… nessun problema di rientranza" | startProfileCmd |

### Stato interno rilevante nelle classi
- `WebApi`: `otaAuthorized_`, `sessionToken_` (MD5(pw+chipId), deterministico, sopravvive ai riavvii), `tokenPass_` (WebApi.h:110-113).
- `OutputBus`: `states_[8]` (specchio SW dei relè), `testMode_`, mapping (OutputBus.h:100-106). `Mcp23008::olat_` = specchio del latch I²C.
- `ProbeBus`: `st_[3]`, `stat_[3]`, `graziaFinoMs_[3]` (per sonda), `primaSonda_` (rotazione), finestra grezzi 12×5 s.
- `ControlPipeline`: latch isteresi (`heatChangedAt_/coolChangedAt_` **su epoch**), `Interlock` (`heatOffAt_/coolOffAt_` **su epoch**), `CompressorGuard::Persist` (persistito), 2 `PeakEstimator` (coefficienti persistiti, ciclo volatile), `prevCool_/prevHeat_`.
- `ProfileEngine::s_` (EngineSnapshot) + `steps_[12]` — persistiti rispettivamente in /state.json e /profile.json.
- `TimeService`: `base_` (default 100 = timeline fittizia), `totalMs_`, `trusted_`, `pendingJumpSec_`.
- `StateStore::lastSaved_` (String dell'ultimo JSON: dedup scritture).

---

## 3. Invarianti dichiarate (testuali)

**Le 9 di ARCHITETTURA.md §1 (righe 25-48)**: uscite spente all'avvio e in errore; mai caldo+freddo insieme; mai blink su D6/D7/D8; disarmate alla nascita; il loop di controllo non si blocca mai; controllo senza WiFi; persistenza solo su transizioni+checkpoint, atomica, saltata se invariata; salti d'orologio traslati; guasto MCP → allarme+consenso revocato.

Nel codice (citazioni chiave):
1. **"Prima la copia attiva su flash, poi il motore"** — main.cpp:143-145 ("se la scrittura fallisce non si parte con una ripresa post-blackout che userebbe gli step vecchi"); WebApi.cpp:461-463 ("Il motore si aggiorna SOLO dopo che la scrittura su flash è riuscita"); WebApi.cpp:583-585 ("È la stessa invariante di handlePutProfile e di startProfileCmd"). ← bersaglio archetipi 5/6: verificarla su **ogni** punto che tocca engine+flash (coldcrash 587-600, handleThermostat/handleSetpoint non toccano flash-profilo, MQTT `profile start` passa da startProfileCmd(0,…) **senza** risalvare).
2. **"Validazione PRIMA di toccare qualsiasi cosa"** — main.cpp:138-141.
3. **Unico punto che decide i relè** — ControlPipeline.h:12-14 ("È L'UNICO punto… Qualunque condizione dubbia produce TUTTO SPENTO"); OutputBus.h:6 ("Unico punto di contatto con i relè"); SafetyMonitor.h:7 ("qualunque dubbio -> uscite spente").
4. **"un remap a caldo non deve lasciare eccitato il vecchio relè"** — main.cpp:113 (`allOff()` prima di `setMapping`). Ma setup():613 fa `setMapping` senza allOff (uscite già spente da safeInitEarly).
5. **"il persist pre-riavvio NON deve ricreare lo stato"** — main.cpp:214 (`factoryResetPending`).
6. **Predittivo: "può solo ANTICIPARE uno spegnimento… Mai tenere acceso più a lungo, mai accendere"** — PeakEstimator.h:21-24; "un ciclo = al più una lezione" (h:13-19).
7. **"Mai avanzare su dato assente o stantio"** (gravity) — ProfileEngine.cpp:182; "Timeout di sicurezza PRIMA di tutto" — cpp:178-180.
8. **shiftClock: "delayedStartEpoch NON si trasla: è un istante assoluto scelto dall'utente"** — ProfileEngine.h:61-63.
9. **Guardia compressore conservativa**: "orologio all'indietro → avvio resta bloccato: comportamento conservativo voluto" — CompressorGuard.h:8-9; "Al boot i relè sono comunque caduti" — main.cpp:587; "riposo pieno prima di ripartire" se `coolWasOn` — main.cpp:589-594.
10. **ProbeBus**: "Si riparte da capo SOLO se cambia la sonda" (h:98-103, anti-archetipo 7); "il rilevamento [stallo] deve stare QUI… a valle significherebbe spingere i timestamp nel futuro" (h:130-135, anti-archetipo 10); "l'ordine di lettura ruota" (h:155-157, anti-archetipo 2); grazia "solo per QUESTA sonda" (h:184-186, anti-archetipo 11); `invalida()` "unico punto in cui una sonda perde validità" (h:217-220).
11. **OG**: "NON in termostato… NON durante l'attesa di partenza ritardata: quell'OG avvelenerebbe l'attenuazione" — main.cpp:183-188; coldcrash "senza passare dal comando di avvio profilo: quello azzererebbe l'OG" — WebApi.cpp:594-595.
12. **"RAM e disco non devono divergere"** — WebApi.cpp:929 (calib add rollback), 958 (delete reload).
13. **OTA**: "autorizzazione ricalcolata a ogni upload e verificata di nuovo alla richiesta finale" — ARCHITETTURA.md:14-15, WebApi.cpp:1391-1400; "OTA vietato finché la password UI è quella di default" — WebApi.cpp:1412-1415.
14. **Numeri non fabbricati**: main.cpp:434-437 ("stamparlo come ultimo valore noto sarebbe un numero fabbricato"); WebApi.cpp:1020 ("Senza letture non esiste una percentuale"); WebApi.cpp:1029 ("meglio un errore esplicito che un JSON troncato").
15. **Allarme densimetro** — main.cpp:275-289: allarme solo se `densimetroVisto || senzaLimite` (hasValue è volatile, l'OG persistita fa da memoria).
16. **Freeze-guard**: setpoint step ≥ -1 °C — WebApi.cpp:16-20; coldcrash "scende sempre" — 528-535.
17. **handlePutProfile solo a profilo fermo** — WebApi.cpp:438-442 (anche a COMPLETED).
18. **Heap non stack** per JSON in fondo alla catena web — StateStore.cpp:8-9, WebApi.cpp:617; array step `static` — main.cpp:132-134.

---

## 4. Inventario endpoint HTTP (WebApi.cpp:51-131)

**Senza autenticazione**: `GET /` (root UI, 51), `POST /api/login` (52; delay 300 ms su errore — blocca il loop), `POST /api/logout` (53; nessun check), `GET /icon.png` (58), `GET` splash iOS ×N (64-70), **`POST /ispindel`** (97; token opzionale + clamp plausibilità, 713-757).

**Con `auth()`** (cookie `axs` == MD5(pw+chipId) oppure Basic `admin:pw`; password vuota = tutto aperto, WebApi.cpp:152):
`GET /api/status` · `GET|PUT /api/config` · `GET|PUT /api/profile` · `GET /api/profiles` · `GET|PUT|DELETE /api/profiles/{id}` · `POST /api/profile/{start,stop,pause,resume,skip}` · `POST /api/profile/coldcrash` · `POST /api/setpoint` · `POST /api/thermostat` · `POST /api/gravity/og` · `POST /api/outputs/{arm,disarm}` · `GET|POST|DELETE /api/calib` · `GET /api/log.csv` · `GET /api/log` · `GET /api/wifi/scan` (**GET con effetto collaterale**: `enableSTA`+scan async, 1254-1255) · `GET /api/diag/i2c` (sonda il bus I²C condiviso) · `GET /api/diag/onewire` (**perturba il bus**: `scan`→`annullaConversione`) · `GET /api/probes` · `POST /api/probes/{calibrate,reset}` · `POST /api/diag/relay` (rifiuta se armato, entra in test mode) · `POST /api/diag/testmode` (esce dal test) · `POST /api/time` · `POST /api/reboot` · `POST /api/factory_reset`.

**`POST /api/update`** (130-131): upload gate = `otaEnabled && authValid() && !usingDefaultUiPassword()` (1414-1415); richiesta finale rivalida `otaAuthorized_ && authValid()` (1395); abort azzera il flag (1431). Nessuna verifica di firma/integrità del binario.

---

## 5. Persistenza (LittleFS)

| File | Chi scrive | Quando | Atomica |
|---|---|---|---|
| `/config.json` | ConfigStore | PUT /api/config; primo avvio (default) | sì (tmp+rename, ConfigStore.cpp:21-26) |
| `/state.json` | StateStore via `persistNow()` | transizioni step/completamento/cambio relè freddo (main.cpp:344-349); checkpoint 5 min; ogni comando (start/stop/pause/resume/skip/setpoint/thermostat/arm/disarm/og/time); salto orologio; **onIspindel** (cattura OG, 188-191); pre-reboot | sì; **dedup**: no-op se JSON identico (StateStore.cpp:76) |
| `/profile.json` (attivo) | ProfileStore.saveActive | PUT /api/profile; start con profileId>0; coldcrash | sì (ProfileStore.cpp:94-99) |
| `/profiles/p1..p20.json` | ProfileStore.save/remove | PUT/DELETE /api/profiles/{id} | sì |
| `/log/d<epochday>.bin` | DataLog.append | 1 record da 16 B ogni `logSampleMin` (default 5 min); cleanup al cambio giorno (retention 14 gg) | append-only |
| `/boot.json` | BootLog | 1 record per accensione + stampEpoch quando l'ora diventa trusted | sì (BootLog.cpp:53-62) |
| `/calib.json` | CalibStore | add/delete punto (con rollback RAM su save fallita) | sì |
| `/devices.json` | DeviceRegistry | **da onIspindel** quando arriva un densimetro nuovo (main.cpp:165-168) | sì |

Factory reset (main.cpp:204-216): rimuove config/state/profile/calib/devices + libreria profili — **non** `/boot.json` né `/log/*`.

---

## 6. Tempo

- **`millis()`** (uint32, wrap ~49,7 gg): tutte le cadenze del loop (`nowMs - lastX >= K`, wrap-safe); FSM sonde e grazia (con confronto `(int32_t)` a ProbeBus.h:207); timeout test mode e retry MCP (OutputBus.h:61-62); `rebootAtMs` (main.cpp:745, sentinella 0 + confronto con segno); finestra PID (`nowMs % windowMs`, PidController.h:44); finestre statistiche sonde.
- **`TimeService.nowEpoch()`** = `base_ + millis accumulati` (TimeService.h:16-18). `base_` da RTC validato al boot (monotonia vs `pstate.lastSeenEpoch` con margine 120 s, TimeService.cpp:25); fallback `base_=100` → **timeline fittizia** (epoch piccoli), `trusted=false`, `pstate.epochsTrusted` la marca su flash.
- **RTC DS1307**: letto solo al boot (3 tentativi); scritto da NTP (max 1/6 h, se scarto >5 s) e da `setManual`.
- **SNTP**: partenza alla prima STA up; check ogni 10 s; scarto >2 s corregge `base_`; scarto >60 s → `pendingJumpSec_` → in loop `consumeClockJump()` trasla **engine, guard compressore, gravity, gravityAny** e persiste (main.cpp:704-717).
- **Chi usa gli epoch**: ProfileEngine (tutti gli àncora, aritmetica firmata `sdiff`), CompressorGuard (persistiti; indietro = bloccato, conservativo), GravityTracker (età firmata disponibile: `signedAgeSec`, GravityTracker.h:53-58), DataLog (chiave dei campioni e dei file giornalieri), calibrazione (età lettura), **HysteresisController e Interlock** (latch `*ChangedAt_`/`*OffAt_` su nowEpoch, sentinella 0 = nessun vincolo).
- **Punto notevole**: `shiftClock` copre engine/guard/gravity ma **non** i latch di isteresi e interlock (che pure vivono su epoch) — vedi §7.

---

## 7. Sorprese per le fasi successive

1. **`dtSec` cablato a 1.0f** — main.cpp:340: `pipeline.tick(…, 1.0f, …)` mentre il tick reale è "≥1000 ms" e può dilatarsi (handler lenti, CSV, OTA). Integrale PID e contatori dei PeakEstimator (`runSec_ += dtSec`) contano i tick, non il tempo.
2. **Latch isteresi/interlock non traslati né persistiti** — un salto d'orologio all'indietro >60 s rende negativi gli `elapsed` di `heatChangedAt_`/`coolOffAt_` (blocco fino al recupero); `pipeline.guard().shiftClock` trasla solo la guardia (main.cpp:711). Dopo un riavvio i latch ripartono da 0 = "nessun vincolo" (minSwitchSec non vale attraverso i riavvii; la guardia sì).
3. **`/ispindel` non autenticato scrive su flash**: densimetro nuovo → `devices.save()` (main.cpp:165-168); con registro pieno (4) ogni ID nuovo **sostituisce l'LRU e riscrive** → un attore LAN senza token può forzare scritture ripetute e svuotare l'elenco; inoltre la prima lettura valida a profilo avviato scrive l'OG (`persistNow`, 188-191).
4. **Token di sessione deterministico e senza scadenza server-side**: MD5(password+chipId) (WebApi.cpp:142-149); il logout cancella solo il cookie del client; chi ha visto il cookie una volta (HTTP in chiaro) è dentro finché non cambia la password. Confronti non a tempo costante.
5. **GET con effetti collaterali** (archetipo 19 dichiarato altrove risolto): `/api/wifi/scan` abilita la STA e avvia scansione; `/api/diag/onewire` e il comando seriale `sonde` abortiscono la conversione in corso (mitigato da `annullaConversione`+grazia); `/api/diag/i2c` scandaglia il bus condiviso con MCP/RTC/OLED mentre l'automazione gira.
6. **`handlePutConfig` muta `cfg` in place prima del save** (WebApi.cpp:335-337): se `save` fallisce risponde 500 ma la config in RAM è già cambiata e `valid=true` — RAM/flash divergenti fino al riavvio (forma dell'archetipo 6, da verificare in fase 2/3).
7. **MQTT `profile start` → `startProfileCmd(0, batch, 0)`** (main.cpp:648): riparte con gli step attivi correnti azzerando `ogGravity` — un comando HA involontario a fermentazione in corso azzera l'OG e riavvia il profilo dallo step 1 senza conferma. `climate_mode off` (641) ferma tutto.
8. **Cache di stato 1 s**: gli handler HTTP leggono `curStatus` vecchio fino a 1 s (esiste `refs.isArmed` live proprio per questo, WebApi.h:41); i comandi HTTP **non** ricostruiscono `curStatus` (quello MQTT sì, main.cpp:656) — una GET /api/status subito dopo un comando può mostrare lo stato precedente.
9. **`delay(300)` sul login errato** (WebApi.cpp:186) e la CLI `scan i2c` bloccano il loop dentro un giro; il CSV lungo è chunked ma sincrono (il controlTick slitta; la grazia sonde copre >15 s di stallo, il resto no).
10. **`stepEndsAtEpoch()` ritorna 0 per RampRate/Gravity** (ProfileEngine.cpp:196-203): l'unica uscita garantita di RampRate è "setpoint saturo **e** temperatura in tolleranza" — senza timeout (archetipo 14 dichiarato in §4: verificare se la UI/allarmi lo raccontano; `profileNoLimit` copre solo Gravity).
11. **`GravityMode::Below` confronta `in.gravity <= target` senza controllo di unità** (ProfileEngine.cpp:145-146): il clamp d'ingresso di /ispindel dipende da `cfg.gravityUnit`; un cambio unità a profilo attivo lascia target e letture su scale diverse (archetipo 12, da inseguire).
12. **Persist su cambio del solo relè freddo** (main.cpp:344): il caldo che commuta ogni finestra PID non persiste (bene per la flash), ma `lastPipe.heat` di un attimo prima del blackout non è ricostruibile — verificare irrilevanza.
13. **`logSample` con unità Plato**: `gravity` in `uint16_t` = `lroundf(plato*100)` — una densità Plato negativa ammessa dal clamp (-1) andrebbe in underflow (main.cpp:367-369, WebApi.cpp:740).
14. **Factory reset non cancella log e boot history** (main.cpp:204-213) — dato residuo dopo "reset di fabbrica".
15. **`armOutputs` da seriale esce dal test mode automaticamente, quello HTTP no** (main.cpp:485 vs WebApi.cpp:705-707): asimmetria voluta? il 409 HTTP è documentato.
16. **`buildStatus` copia `AppStatus` con 3 String per ritorno di lambda** (`refs.status`) a ogni /api/status, OLED e MQTT publish — pressione heap da quantificare (asse E).
17. **kMaxSteps=12 ma il commento di ProfileEngine.h:7 dice ancora "fino a 5 step"**; ARCHITETTURA.md §3 dice "Max 12 step (era 8)" e §2 elenca moduli con nomi vecchi (core/, IspindelIngest, OtaUpdate, RTClib) che non corrispondono ai file reali — la doc è parzialmente stantia: non usarla come oracolo.
18. **Delayed start non traslato dai salti d'orologio** (scelta dichiarata, ProfileEngine.h:61-63): con boot su timeline fittizia (base=100) un `delayedStartAt` reale è "fra 50 anni" finché NTP non sincronizza — il ri-base di setup (600-609) tocca stepStart/pausedAt ma **non** `delayedStartEpoch`.