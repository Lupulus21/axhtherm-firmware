# Referto forense — AxHTherm 2.5

**Data**: 2026-08-23/24 · **Mandato**: `docs/REVISIONE_FORENSE.md` · **Materiali di lavoro**: `docs/revisione-2026-08-23/`

## Sommario esecutivo

Revisione condotta con agenti in parallelo secondo l'architettura del §7. **Fase 1** (ricognizione) → mappa della codebase. **Giro 1** (ricerca per asse, 8 agenti sugli assi A–H del §6) → 57 rilievi grezzi. Deduplicazione contro tutti i rilievi → **43 rilievi canonici**. **Fase 3** (confutazione avversariale, 2 agenti indipendenti per rilievo: confutatore tecnico + confutatore d'impatto, 86 agenti) → verdetti. Adjudicazione del revisore sui voti discordi.

**Esito della confutazione**: 16 confermati (entrambi i confutatori), 24 a voti discordi (letti a mano, non scartati), 3 respinti (entrambi negano).

**Severità dopo adjudicazione**: **6 critici, 14 gravi, 20 minori, 3 respinti.**

> **Nota di metodo — la fase avversariale ha cambiato il quadro.** Il confutatore d'impatto ha letto `docs/ANALISI.md` (collaudo al banco del 15/08/2026), che né la ricognizione né gli agenti di ricerca avevano aperto. Fatto decisivo (`ANALISI.md:186-188`): **il chip MCP23008 non è montato sull'esemplare reale**; i due carichi a 230 V stanno su **REL1 (cavo riscaldante)** e **REL3 (frigo)**, GPIO diretti. Questo ha declassato da critico a minore l'intero cluster di guasti sul MCP (C08, C09) e ha contribuito a respingere C13: sono difetti reali nel firmware generico ma **inerti su questo hardware**. È esattamente il tipo di errore — «due spiegazioni compatibili, una sola verificata» (archetipo 1) — che la seconda squadra esiste per intercettare.

### I sei rilievi critici (gatekeeper del giudizio di produzione)

| id | titolo | archetipo |
|---|---|---|
| **C12** | Salti d'orologio non traslano i latch di isteresi/interlock: relè congelato per l'ampiezza del salto | 5 |
| **C14** | Step RampRate senza condizione di uscita garantita, senza timeout, senza allarme | 14 |
| **C15** | `gravityUnit` dichiarata «solo etichetta» ma governa confronti e formule: cambio a caldo fa avanzare o bloccare gli step | 12 |
| **C21** | ConfigStore/ProfileStore promuovono con `rename` un file **troncato** (controllo `n==0`, non `n==misura`) | 5 |
| **C31** | Salto d'orologio da set manuale applicato subito ma traslato solo al giro dopo: `controlTick` corre su àncore vecchie | 10 |
| **C32** | Comando MQTT **retained** rieseguito a ogni riconnessione (start → azzera OG) | nuovo |

Tre di questi (C12, C14, C31) sono ricorrenze dirette di archetipi già pagati (5, 14, 10). C15 e C21 sono varianti degli archetipi 12 e 5 in punti nuovi. **Nessuno produce un allarme**: si manifestano in silenzio su un dispositivo non sorvegliato — la clausola più importante del §1.

---

## Rilievi critici
### C12 — Salti d'orologio non traslano i latch di isteresi e interlock: relè congelato (acceso o spento) per l'ampiezza del salto
- **SEVERITÀ**: critico  (rilievo originale: critico)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: B,C,H  ·  rilievi grezzi: G1-B-5,G1-C-3,G1-H-1
- **ARCHETIPO (§4)**: 5
- **FILE**: src/main.cpp:710-713 ; lib/axcore/src/ax/HysteresisController.h:29-32 ; lib/axcore/src/ax/HysteresisController.h:57-60 ; lib/axcore/src/ax/Interlock.h:35-39 ; src/svc/TimeService.cpp:76
- **CODICE**:
```
main.cpp:710-713: engine.shiftClock(clockJump); pipeline.guard().shiftClock(clockJump); gravity.shiftClock(clockJump); gravityAny.shiftClock(clockJump);  [hyst_ e lock_ esclusi: shiftClock non esiste proprio in quelle classi — verificato con grep]
HysteresisController.h:29-31: if (wantCool != coolOn_ && elapsed(nowEpoch, coolChangedAt_) >= minStateSec_) { coolOn_ = wantCool; coolChangedAt_ = nowEpoch; }
HysteresisController.h:59: return (int64_t)now - (int64_t)since;
TimeService.cpp:76: if (jump > 60 || jump < -60) pendingJumpSec_ += jump;
```
- **COSA ACCADE**: Stato iniziale: fermentazione attiva, frigo ON con latch coolOn da 2 min (coolChangedAt_=T sulla vecchia timeline). Sequenza: l'orologio salta all'indietro di Δ>60 s — set manuale via /api/time o seriale `ora` (correzione di un'ora impostata male), oppure NTP che corregge un RTC avanti dopo giorni di WiFi assente. consumeClockJump trasla motore, guardia e densimetro ma coolChangedAt_ resta nel futuro: elapsed diventa negativo e resta < minStateSec finché now non supera T+minStateSec, cioè per Δ+minStateSec. In quell'intervallo il latch non può spegnersi (evaluate rifiuta il cambio, forceCoolOff del predittivo pure): la temperatura scende sotto il setpoint e il frigo resta in marcia continua — con Δ=1 h, un'ora di sovraraffreddamento del mosto. Simmetrico sul caldo solo con usePid=false (col PID il caldo passa da windowOutput su millis, immune). Effetto speculare del salto in avanti: elapsed gonfiato annulla in un colpo minStateSec e il tempo morto minSwitchSec dell'interlock (il riposo compressore invece resta protetto perché la guardia È traslata). Conseguenza osservabile: UI con cool attivo e temperatura sotto il setpoint per decine di minuti.
- **PERCHÉ ORA**: L'invariante "salti d'orologio traslati" è dichiarata (ARCHITETTURA §1) e applicata a motore, guardia e densimetro: questi due componenti, che pure vivono sugli stessi epoch, sono l'unico punto scoperto. Il trigger è un'operazione ordinaria: correzione manuale dell'ora o prima sincronizzazione NTP dopo un lungo distacco di rete.
- **CORREZIONE**: Aggiungere shiftClock(deltaSec) a HysteresisController (heatChangedAt_/coolChangedAt_) e a Interlock (heatOffAt_/coolOffAt_, con la stessa cura per la sentinella 0 usata da CompressorGuard::shiftClock) e chiamarli in consumeClockJump accanto agli altri quattro.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi. Salti orologio non traslano i latch isteresi/interlock (niente shiftClock) → relè congelato per l'ampiezza del salto. 3 assi. Critico.

### C14 — Step RampRate senza condizione di uscita garantita, senza timeout e senza allarme di stallo
- **SEVERITÀ**: critico  (rilievo originale: critico)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: C,G,H  ·  rilievi grezzi: G1-C-1,G1-G-1,G1-H-2
- **ARCHETIPO (§4)**: 14
- **FILE**: lib/axcore/src/ax/ProfileEngine.cpp:172-175 ; lib/axcore/src/ax/ProfileEngine.cpp:196-202 ; lib/axcore/src/ax/Types.h:27-28 ; src/main.cpp:280-288 ; data-src/index.html:1179 ; data-src/index.html:880
- **CODICE**:
```
case StepType::RampRate: {
  bool spAtTarget = (sp == st.targetC);
  return spAtTarget && in.tempValid &&
         (in.tempC >= st.targetC - tol_ && in.tempC <= st.targetC + tol_);
}
--- Types.h:27: uint32_t maxDurationSec = 0;  // Gravity: timeout di sicurezza (0 = nessuno)
--- main.cpp:282-284: const bool senzaLimite = passi && iPasso < ax::kMaxSteps &&
                         passi[iPasso].type == ax::StepType::Gravity &&
                         passi[iPasso].maxDurationSec == 0;
--- index.html:1179: {type:"ramp_rate",target:10,degPerHour:0.3,allowHeat:false}
--- index.html:880: d.querySelector(".f_mx").style.display=ty==="gravity"?"":"none"
```
- **COSA ACCADE**: Stato iniziale: profilo lager della libreria stock in esecuzione, step ramp_rate 'scendi a 10 °C a 0,3 °C/h, allowHeat=false' (index.html:1179; la libreria ne contiene decine, anche in salita a 20-22 °C). Sequenza: il setpoint satura al target dopo poche ore, ma il frigo non arriva a 10 °C (compressore esausto, garage d'estate, porta socchiusa — o, per le rampe in salita, riscaldatore non mappato: l'arm accetta heat=0 'nessuno'). stepDone richiede tempC dentro target±tol: resta false a ogni tick, per sempre. Conseguenza osservabile: il profilo resta su quello step per settimane in silenzio: nessun flag in anyAlarm(), stepEndsAtEpoch()=0 quindi la UI non mostra nemmeno una fine prevista, il campo timeout (maxDurationSec) esiste ma il motore lo consulta solo per Gravity e l'editor lo nasconde per ramp_rate; l'avviso 'questo step non avanzerà mai' della pagina esiste solo per gli step a densità. La cotta procede alla temperatura sbagliata finché qualcuno non se ne accorge da solo.
- **PERCHÉ ORA**: Basta un profilo della libreria integrata più un impianto che non raggiunge il target: frigo sottodimensionato d'estate per un lager a 10 °C, o rampa in salita in cantina fredda senza cavo riscaldante collegato. Il dispositivo lavora non sorvegliato per settimane: è esattamente il guasto invisibile del §0.2.
- **CORREZIONE**: Estendere maxDurationSec (già serializzato per tutti gli step) anche a RampRate nel motore e mostrarne il campo nell'editor; in alternativa (o in aggiunta) un allarme 'step in ritardo' quando il setpoint è saturo da oltre N ore e la temperatura resta fuori tolleranza. La prima strada dà un'uscita garantita, la seconda dà visibilità senza cambiare la semantica dei profili esistenti.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi. Step RampRate senza uscita garantita/timeout/allarme; decine di modelli stock lo usano; il test sancisce l'attesa infinita. 3 assi. Critico.

### C15 — gravityUnit dichiarata 'solo etichetta' ma governa confronti/clamp/formule: cambio a caldo fa avanzare o bloccare gli step
- **SEVERITÀ**: critico  (rilievo originale: critico)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: C,H  ·  rilievi grezzi: G1-C-2,G1-H-5
- **ARCHETIPO (§4)**: 12
- **FILE**: src/svc/AppConfig.h:49 ; lib/axcore/src/ax/ProfileEngine.cpp:144-146 ; src/main.cpp:326-335 ; src/svc/WebApi.cpp:335-348 ; src/svc/WebApi.cpp:739-744 ; src/svc/WebApi.cpp:32-35
- **CODICE**:
```
AppConfig.h:49: String gravityUnit = "sg";  // D11: "sg" o "plato" (solo etichetta/visualizzazione)
--- ProfileEngine.cpp:145: case GravityMode::Below:
      return in.gravity <= st.gravityTarget;
--- main.cpp:328: if (cfg.gravityUnit == "plato") {
      if (og > 0.01f) { in.attenuationPct = (og - sg) / og * 100.0f; ... }
--- WebApi.cpp:739-740: bool plato = r_.cfg->gravityUnit == "plato";
    float gmin = plato ? -1.0f : 0.90f, gmax = plato ? 45.0f : 1.30f;
--- WebApi.cpp:335-337: r_.cfgStore->fromJson(&c, doc);
    c.valid = true;
    if (!r_.cfgStore->save(c)) { ... }  // nessuna guardia su unit con profilo attivo
--- WebApi.cpp:32-34: } else if (steps[i].gravityMode == ax::GravityMode::Below &&
               (steps[i].gravityTarget < 0 || steps[i].gravityTarget > 35)) {  // range che copre ENTRAMBE le unità
```
- **COSA ACCADE**: Stato iniziale: fermentazione attiva con unità °Plato, step Gravity 'Below 3.0 °P' seguito dal cold crash; OG catturata 12.5. Sequenza: l'utente passa l'unità a SG in Impostazioni (es. per allinearsi al nuovo firmware dello iSpindel) — handlePutConfig accetta senza avvisi anche a profilo in corsa e applyConfigLive rende attivo il cambio; la lettura successiva arriva in SG (1.045, accettata dal clamp SG 0.90-1.30, fresca e valida). gravityConditionMet valuta 1.045 <= 3.0 → vero → lo step 'fermentazione' si chiude all'istante e il crash parte con giorni di anticipo su birra non attenuata. Nella direzione opposta (sg→plato) lo stesso step non si chiude mai: 6.2 °P <= 1.012 è sempre falso, e con maxDurationSec=0 (default) lo stallo è eterno e muto. Anche l'attenuazione si avvelena: OG 12.5 rimasta in °P con letture SG e formula SG dà (12.5-1.020)/(12.5-1)*100 ≈ 99.8% → uno step Attenuation scatta subito. validateSteps non può accorgersene: il range del target Below (0..35) copre entrambe le scale.
- **PERCHÉ ORA**: Il cambio unità è un'operazione di configurazione ordinaria (nuovo densimetro, preferenza dell'utente) raggiungibile da PUT /api/config in qualunque momento; nessun punto del firmware converte i target degli step, l'OG persistita o la correction, né blocca il cambio a profilo attivo. Il commento 'solo etichetta/visualizzazione' documenta un'invariante che il codice non rispetta in almeno cinque punti.
- **CORREZIONE**: Due strade: (a) rifiutare il cambio di gravityUnit mentre un profilo è attivo o un'OG è catturata (fail-closed, semplice, il messaggio spiega perché); (b) convertire coerentemente target degli step, OG, correction e soglie al cambio unità (completa ma delicata: ogni conversione parziale riproduce l'archetipo). In ogni caso validare che unit sia solo 'sg' o 'plato' (oggi accetta qualunque stringa, e 'Plato' con la maiuscola ricade in silenzio su SG) e correggere il commento di AppConfig.h.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi. gravityUnit governa confronti/formule pur dichiarata 'solo etichetta'; cambio a caldo fa avanzare o bloccare gli step. Critico.

### C21 — ConfigStore e ProfileStore promuovono con rename un file troncato: il controllo è n==0, non n==misura documento
- **SEVERITÀ**: critico  (rilievo originale: critico)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: incompleto  ·  trovato dagli assi: D  ·  rilievi grezzi: G1-D-1
- **ARCHETIPO (§4)**: 5
- **FILE**: src/svc/ConfigStore.cpp:21-26 ; src/svc/ProfileStore.cpp:94-99 ; contrasto (guardia corretta): src/svc/CalibStore.cpp:44-55, src/svc/DeviceRegistry.cpp:34-43, src/svc/BootLog.cpp:53-62, src/svc/StateStore.cpp:78-85 ; conseguenza al boot: src/main.cpp:551-561 e 578-583
- **CODICE**:
```
ConfigStore.cpp:21-26:
  File f = LittleFS.open("/config.tmp", "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  if (n == 0) return false;
  return LittleFS.rename("/config.tmp", kPath);  // sovrascrittura atomica

ProfileStore.cpp:94-99 (writeFile, usato da saveActive/save):
  File f = LittleFS.open(tmpPath, "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  if (n == 0) return false;
  return LittleFS.rename(tmpPath, path);  // sovrascrittura atomica

CalibStore.cpp:46-54 (l'invariante dichiarata altrove):
  const size_t want = measureJson(doc);
  const size_t written = serializeJson(doc, f);
  f.close();
  // Con filesystem pieno serializeJson scrive solo i primi chunk: promuovere un
  // file troncato distruggerebbe i punti buoni (al riavvio load() fallirebbe).
  if (written != want) {
    LittleFS.remove("/calib.tmp");
    return false;
  }
```
- **COSA ACCADE**: Stato iniziale: filesystem quasi pieno (vedi rilievo sulla retention senza tetto) e fermentazione in corso. Sequenza: l'utente avvia un profilo dalla libreria (startProfileCmd -> profStore.saveActive, main.cpp:145), o un cold crash (WebApi.cpp:587), o salva la config (PUT /api/config). serializeJson scrive i primi chunk sul tmp, la write LittleFS fallisce a metà: n>0 ma n<misura. Il controllo n==0 passa e rename sovrascrive il file buono con il troncato; la funzione ritorna true, il motore parte, l'utente vede 200/profilo avviato. Conseguenza osservabile: al primo riavvio o blackout deserializeJson fallisce (IncompleteInput). Per /profile.json: loadActive false -> stepCount 0 -> mode forzato Off (main.cpp:582-583) -> la cotta resta senza controllo termico, in silenzio, su dispositivo non sorvegliato. Per /config.json: default + valid=false -> uscite disabilitate e credenziali WiFi perse (dispositivo raggiungibile solo via SoftAP).
- **PERCHÉ ORA**: L'intera invariante 'prima la flash, poi il motore' (main.cpp:143-145, WebApi.cpp:461-464 e 583-590) si fonda sul valore di ritorno di saveActive/save, che in questo caso mente: il 500 promesso non arriva mai. Il modo di guasto è riconosciuto dal progetto stesso (commento in CalibStore.cpp:49-50) e la guardia c'è in 4 store su 6: mancano esattamente config e profili, i due file che l'utente non può ricostruire e da cui dipende la ripresa. Semantica verificata nella libreria: ArduinoJson 6.21 conta i byte accettati dal Print (.pio/libdeps/d1_mini/ArduinoJson/src/ArduinoJson/Serialization/CountingDecorator.hpp: count_ += writer_.write(...)), non la misura del documento.
- **CORREZIONE**: Applicare a ConfigStore::save e ProfileStore::writeFile la stessa guardia degli altri quattro store: confrontare serializeJson(doc,f) con measureJson(doc), rimuovere il tmp e ritornare false sul mismatch, prima del rename. In alternativa (più costosa): rileggere e parsare il tmp prima di promuoverlo.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi (residuo impatto arrivato). ProfileStore/ConfigStore promuovono file troncato (n==0 non n==misura), a differenza degli altri 4 store che confrontano con measureJson. Profilo attivo troncato → al boot loadActive fallisce → mode Off → cotta senza controllo dopo blackout, in silenzio. Correzione a costo minimo (allineare al pattern già presente). Critico.

### C31 — Salto d'orologio da set manuale applicato subito ma traslato solo al giro dopo: controlTick nello stesso giro su ancore non traslate
- **SEVERITÀ**: critico  (rilievo originale: critico)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: F  ·  rilievi grezzi: G1-F-1
- **ARCHETIPO (§4)**: 10
- **FILE**: src/svc/TimeService.cpp:72-81 ; src/main.cpp:704-727 ; src/svc/WebApi.cpp:1376-1389 ; src/main.cpp:491-496 ; lib/axcore/src/ax/ProfileEngine.cpp:243-268 ; lib/axcore/src/ax/CompressorGuard.h:30-44
- **CODICE**:
```
TimeService.cpp:75-77: int64_t jump = (int64_t)epoch - (int64_t)nowEpoch();
  if (jump > 60 || jump < -60) pendingJumpSec_ += jump;
  base_ = (uint64_t)epoch - totalMs_ / 1000ULL;
main.cpp loop(): 704 int64_t clockJump = timeSvc.consumeClockJump(); ... 719 web.loop(); ... 721 serialPoll(); 723 if (nowMs - lastCtlMs >= 1000) { lastCtlMs = nowMs; controlTick(nowMs); }
ProfileEngine.cpp:244-246: uint8_t guard = 0;
  while (guard++ <= kMaxSteps && stepDone(steps_[s_.stepIdx], elapsed, in, sp, &waiting)) {
```
- **COSA ACCADE**: Stato iniziale: profilo in marcia; orologio partito senza RTC/NTP (base_=100, timeline fittizia — è il caso in cui /api/status mostra l'allarme «orologio inaffidabile: impostare data/ora»). L'utente obbedisce: POST /api/time (o `ora` da seriale). setManual() sposta base_ IMMEDIATAMENTE e accoda pendingJumpSec_; ma consumeClockJump() è già passato in questo giro di loop (posizione 704, prima di web.loop a 719). Se nello stesso giro scatta il bordo dei 1000 ms (riga 723), controlTick gira con nowEpoch sulla timeline nuova e stepStartEpoch/lastOffEpoch ancora su quella vecchia: il while di ProfileEngine::tick fa scadere TUTTI gli step a tempo in un colpo (profilo completato e persistito su flash), e CompressorGuard::canStart vede un riposo di decenni trascorso e può eccitare il compressore ignorando il riposo minimo. Al giro successivo shiftClock trasla di +salto anche le ancore appena SCRITTE durante il tick sbagliato: lastOnEpoch finisce nel futuro di ~56 anni, quindi mustStayOn resta vero per sempre (compressore inchiodato ACCESO finché non si riavvia); dopo il riavvio quegli epoch futuri persistiti bloccano canStart (frigo mai più avviabile senza intervento). Con salti più piccoli (correzione di ore) l'effetto è proporzionale: step saltati e step successivo allungato del salto, in silenzio. Lo stesso vale per mqtt.loop (730) e per il checkpoint (732) che agiscono nella finestra.
- **PERCHÉ ORA**: Il percorso è l'azione raccomandata dal firmware stesso (l'allarme in /api/status chiede di impostare l'ora) e ogni set manuale con scarto >60 s apre la finestra; la coincidenza col bordo del tick vale ~(durata del giro)/1000 per ogni set, con conseguenze catastrofiche e persistite. Il percorso NTP è invece sicuro perché rilevamento (697) e consumo (704) stanno nello stesso giro: l'asimmetria è solo dei comandi che girano dopo la riga 704.
- **CORREZIONE**: Consumare il salto pendente PRIMA di ogni uso delle ancore, non a un punto fisso del loop: o si esegue la traslazione dentro setManual (il TimeService riceve i riferimenti/una callback di shift), o controlTick/mqtt-handler ricontrollano consumeClockJump() come prima istruzione. In alternativa setManual può limitarsi ad accodare il salto senza toccare base_ finché il loop non lo consuma (tempo e traslazione diventano atomici), al costo di rispondere alla POST con l'ora ancora vecchia.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi. Salto orologio manuale applicato subito ma traslato al giro dopo → controlTick nello stesso giro fast-forwarda il profilo. Archetipo 10. Critico.

### C32 — Comando MQTT retained rieseguito a ogni riconnessione: stop del controllo o riavvio profilo con azzeramento OG
- **SEVERITÀ**: critico  (rilievo originale: critico)
- **CONFIDENZA**: probabile
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: F  ·  rilievi grezzi: G1-F-3
- **ARCHETIPO (§4)**: nuovo
- **FILE**: src/svc/MqttSvc.cpp:29-40 ; src/svc/MqttSvc.cpp:45-70 ; src/main.cpp:630-657 ; .pio/libdeps/d1_mini/PubSubClient/src/PubSubClient.h:81-83
- **CODICE**:
```
MqttSvc.cpp:70: cli_.subscribe((base_ + "/cmd/+").c_str());   // a OGNI riconnessione
MqttSvc.cpp:29: cli_.setCallback([this](char* topic, uint8_t* payload, unsigned int len) { ... onCmd_(cmd, String(buf)); ... });
PubSubClient.h:81: #define MQTT_CALLBACK_SIGNATURE std::function<void(char*, uint8_t*, unsigned int)> callback   // nessun flag retained
main.cpp:639-641: if (payload == "off") { engine.stop(); persistNow(); }
main.cpp:648: if (payload == "start") startProfileCmd(0, pstate.batchName, 0);  // pstate.ogGravity = 0 dentro
```
- **COSA ACCADE**: Stato iniziale: fermentazione di settimane in corso, Home Assistant integrato via MQTT. Sequenza: l'utente (o una sua automazione) pubblica una volta un comando su axhtherm/cmd/… con retain=true — scelta naturale proprio «perché il comando arrivi anche se il dispositivo è offline» (opzione esposta dal servizio mqtt.publish di HA), o un test con mosquitto_pub -r. Il broker conserva il messaggio; MqttSvc si risottoscrive a cmd/+ a ogni riconnessione (retry ogni 30 s dopo ogni caduta WiFi o riavvio del broker) e per specifica MQTT il retained viene riconsegnato a ogni SUBSCRIBE; il callback di PubSubClient non espone il flag retained, quindi il firmware lo riesegue come un comando fresco. Con «climate_mode off» retained: a ogni riconnessione engine.stop() — il controllo di temperatura si spegne da solo e la cotta deriva verso l'ambiente, su un dispositivo non sorvegliato. Con «profile start» retained: il profilo riparte dallo step 1 e pstate.ogGravity viene azzerata a ogni riconnessione — la fermentazione non avanza mai oltre le prime ore e attenuazione/ABV sono persi.
- **PERCHÉ ORA**: Riconnessioni MQTT sono eventi ordinari (riavvio del broker/di HA, cali WiFi); basta un solo publish retained, una tantum, per armare la riesecuzione perpetua. Il meccanismo (retained su subscribe, callback senza flag) è da specifica e dal codice della libreria; resta «probabile» solo perché richiede che l'utente abbia pubblicato con retain.
- **CORREZIONE**: Due direzioni: (a) all'atto della connessione, ignorare i comandi ricevuti in una breve finestra dopo la subscribe (scarta i retained ma anche un comando vivo sfortunato in quei ms); (b) ripubblicare su ogni cmd/… un payload vuoto retained dopo l'esecuzione (cancella il retained dal broker, costo: publish aggiuntivi e dipendenza dai permessi ACL). Documentare comunque che i comandi non vanno pubblicati retained.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi. Comando MQTT retained rieseguito a ogni riconnessione (start azzera OG). Critico (richiede comando retained pubblicato dall'utente).

---

## Rilievi gravi
### C01 — /ispindel non autenticato inietta densità: avanza step Gravity o avvelena OG, rovina la cotta in silenzio
- **SEVERITÀ**: grave  (rilievo originale: critico)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: A  ·  rilievi grezzi: G1-A-1
- **ARCHETIPO (§4)**: nuovo (superficie esterna non autenticata che pilota decisioni critiche di cotta; affine 15/19)
- **FILE**: src/svc/WebApi.cpp:97 ; src/svc/WebApi.cpp:713-758 ; src/svc/AppConfig.h:50 ; src/svc/AppConfig.h:53 ; src/main.cpp:170-192 ; lib/axcore/src/ax/ProfileEngine.cpp:143-146
- **CODICE**:
```
WebApi.cpp:97  server_.on("/ispindel", HTTP_POST, [this]() { handleIspindel(); });
WebApi.cpp:721  if (r_.cfg->ispindelToken.length()) { ... }   // token OPZIONALE
AppConfig.h:50  String ispindelToken = "";   // default: nessun token
AppConfig.h:53  String gravityDevice = ""; // default: accetta il primo che trasmette
main.cpp:175  if (cfg.gravityDevice.length() && devName.length() && !cfg.gravityDevice.equalsIgnoreCase(devName)) { ... return; }
main.cpp:181  gravity.update(g, batt, t, timeSvc.nowEpoch(), angle, rssi, devId);
main.cpp:188  if (pstate.ogGravity <= 0.0f && engine.running() && !lastTick.waitingDelayedStart) { pstate.ogGravity = gravity.gravity(); persistNow(); }
ProfileEngine.cpp:145  case GravityMode::Below: return in.gravity <= st.gravityTarget;
```
- **COSA ACCADE**: Configurazione di default (ispindelToken="", gravityDevice=""). È in corso un profilo con uno step Gravity (es. "fermenta a 20 °C finché SG <= 1.010", pattern comune nei 49 modelli). Un qualunque host sulla LAN invia POST /ispindel con corpo {"gravity":1.008,"temperature":20} (valore dentro il clamp di plausibilità). handleIspindel non chiama auth(), il filtro densimetro è vuoto quindi la lettura entra in gravity.update; al tick successivo in.gravity=1.008 <= target 1.010 -> gravityConditionMet vera -> lo step avanza. Se lo step successivo è un cold crash o è l'ultimo, la fermentazione viene troncata a metà: lievito ancora attivo fatto cadere, birra dolce/stucco -> cotta persa. La lettura appare valida e fresca, quindi NESSUN allarme si accende. In parallelo, se è la prima lettura dopo l'avvio, quel valore diventa l'OG persistita (main.cpp:189): attenuazione e ABV mostrati diventano falsi per il resto della cotta.
- **PERCHÉ ORA**: Raggiungibile con la configurazione di fabbrica (token e densimetro attivo entrambi vuoti) da chiunque abbia accesso IP al dispositivo; basta un solo pacchetto ben temporizzato, nessun bruteforce, nessuna sessione.
- **CORREZIONE**: Due strade, non esclusive: (a) rendere obbligatorio ispindelToken quando esiste un profilo attivo o quando l'ingest alimenta l'engine, rifiutando /ispindel senza token; (b) rifiutare per default le letture da un densimetro il cui deviceId/nome non è quello configurato in gravityDevice quando un profilo è in marcia (whitelist esplicita invece di 'accetta il primo'). In entrambi i casi rendere visibile in UI che l'ingest è aperto.
- **ADJUDICAZIONE DEL REVISORE**: Reale nel default (auth assente, clamp non filtra valori plausibili). Il danno da attacco deliberato è dentro il modello LAN accettato, MA resta un residuo NON avversariale: un secondo iSpindel legittimo con gravityDevice='' alimenta la cotta sbagliata / avvelena l'OG. Da critico a grave.
- **VOTO TECNICO** (reale=True): Non riesco a demolirlo: ogni riferimento file:riga è corretto e il percorso è raggiungibile con la configurazione di fabbrica. Verifica riga per riga:

1) Endpoint non autenticato. WebApi.cpp:97 registra `/ispindel` e handleIspindel (WebApi.cpp:713-758) NON chiama mai auth(): a differenza di tutti gli altri handler (es. disarm a riga 92 `if (!auth()) return;`), l'unica difesa è il token OPZIONALE a WebApi.cpp:721 `if (r_.cfg->ispindelToken.length()) {...}`. Verificato leggendo l'intero handler.…
- **VOTO IMPATTO** (reale=False, sev=minore): La meccanica tecnica regge (WebApi.cpp:713-757 non autentica, token opzionale a :721, clamp a :739-744 filtra solo valori fuori range non un 1.008 plausibile; ProfileEngine.cpp:180-190 con gravityValid/Fresh veri e :145-146 fa avanzare lo step; main.cpp:188-190 persiste l'OG). Ma l'IMPATTO non è concreto per un percorso plausibile. Il danno richiede un'iniezione deliberata da un host LAN, cioe' un attore ostile. Sotto il modello di minaccia ESPLICITAMENTE accettato (ARCHITETTURA.md:7-18: 'LAN do…

### C03 — OTA senza firma né MD5; password UI vuota apre l'OTA senza allarme (RCE, max guadagno)
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: A  ·  rilievi grezzi: G1-A-3
- **ARCHETIPO (§4)**: nuovo (max-gain: RCE via OTA; affine 15 default pericoloso e 8/17 per invisibilità)
- **FILE**: src/svc/WebApi.cpp:1409-1421 ; src/svc/WebApi.cpp:1425-1429 ; src/svc/WebApi.cpp:152 ; src/svc/AppConfig.h:12 ; src/svc/WebApi.cpp:297-298
- **CODICE**:
```
WebApi.cpp:1414  otaAuthorized_ = r_.cfg->otaEnabled && authValid() && !r_.cfg->usingDefaultUiPassword();
WebApi.cpp:1421  if (!Update.begin(maxSpace)) Update.printError(Serial);   // nessun setMD5()/installSignature()
WebApi.cpp:1427  if (Update.end(true)) ...   // con _target_md5 vuoto e _verify nullo: nessun controllo di integrità
WebApi.cpp:152  if (r_.cfg->uiPassword.length() == 0) return true;   // password vuota = auth sempre superata
AppConfig.h:12  bool usingDefaultUiPassword() const { return uiPassword == kDefaultUiPassword; }
```
- **COSA ACCADE**: Due difetti compongono il rischio a massimo guadagno. (1) Nessuna provenienza: il firmware non chiama mai Update.setMD5() né Update.installSignature(), quindi Update.end(true) accetta qualunque binario della lunghezza giusta con header di boot valido — nessuna firma, nessun MD5. L'unico gate è authValid(). (2) usingDefaultUiPassword() confronta solo con "axhtherm": se l'utente imposta la password UI a stringa vuota (raggiungibile via PUT /api/config con system.uiPassword=""), authValid() ritorna true per tutti (riga 152) e usingDefaultUiPassword() è false, quindi otaAuthorized_ diventa true SENZA autenticazione. Inoltre l'allarme 'PASSWORD DI DEFAULT' (riga 297) non scatta per la password vuota: il dispositivo è completamente aperto, OTA incluso, e non lo dice. Chiunque sulla LAN può caricare firmware arbitrario = esecuzione di codice sul controllore dei 230 V.
- **PERCHÉ ORA**: Con password di default 'axhtherm' l'OTA è correttamente bloccato; ma appena la password diventa vuota (scelta plausibile di chi non vuole password, oltre che via API grezza) tutte le difese cadono in silenzio. Anche con password impostata, l'assenza di firma rende l'OTA una compromissione totale per chiunque possieda/catturi la credenziale.
- **CORREZIONE**: Trattare la password vuota come 'nessuna auth' esplicita: bloccare OTA e alzare un allarme dedicato quando uiPassword è vuota (non solo quando è il default). Per l'integrità: adottare la verifica firma dell'Updater (Update.installSignature con chiave pubblica compilata nel firmware) oppure almeno Update.setMD5() con l'hash atteso trasmesso e verificato; senza provenienza crittografica, dichiarare esplicitamente il limite.
- **ADJUDICAZIONE DEL REVISORE**: Componente MITM dentro il modello accettato. Residuo REALE e indipendente: password UI vuota (accettata da PUT /api/config) apre l'OTA a chiunque senza allarme (usingDefaultUiPassword confronta solo con 'axhtherm'). Difesa in profondità mancante.
- **VOTO TECNICO** (reale=True): Ho letto riga per riga i file citati e ho cercato la guardia a monte che impedirebbe il difetto: non esiste. Tutti i riferimenti del rilievo corrispondono al codice reale, e la catena regge su due gambe indipendenti, entrambe verificate.

GAMBA 1 — assenza di integrità/provenienza (verificata). In tutto src/ non compare mai setMD5() né installSignature(): `grep -rn "setMD5|installSignature"` su src/ non ha esiti oltre alle chiamate Update.begin/write/end. WebApi.cpp:1421 `if (!Update.begin(maxSp…
- **VOTO IMPATTO** (reale=False, sev=minore): Due componenti, entrambi senza conseguenza concreta plausibile per il proprietario.

COMPONENTE A (OTA senza firma/MD5). Vero che il firmware non chiama setMD5()/installSignature() (src/svc/WebApi.cpp:1421,1427), ma il modello di minaccia e' esplicitamente "LAN domestica, HTTP in chiaro — modello accettato" (docs/ARCHITETTURA.md:7). La firma del firmware difende soprattutto da MITM/supply-chain, classe gia' fuori portata perche' il modello accetta HTTP in chiaro. Il gate OTA e' otaEnabled && aut…

### C07 — Comandi MQTT senza autorizzazione per-comando: chiunque sul broker ferma il controllo o riavvia il profilo
- **SEVERITÀ**: grave  (rilievo originale: minore)
- **CONFIDENZA**: probabile
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: A  ·  rilievi grezzi: G1-A-7
- **ARCHETIPO (§4)**: nuovo (superficie di comando esterna senza autorizzazione per-azione; affine §7.7 della mappa)
- **FILE**: src/svc/MqttSvc.cpp:29-40 ; src/svc/MqttSvc.cpp:70 ; src/main.cpp:646-653 ; src/main.cpp:152
- **CODICE**:
```
MqttSvc.cpp:32  if (!t.startsWith(prefix) || !onCmd_) return;   // nessun controllo su chi pubblica
MqttSvc.cpp:70  cli_.subscribe((base_ + "/cmd/+").c_str());
main.cpp:648  if (payload == "start") startProfileCmd(0, pstate.batchName, 0);
main.cpp:639  if (payload == "off") { engine.stop(); persistNow(); }
main.cpp:152  pstate.ogGravity = 0;  // startProfileCmd: nuova cotta, OG ricatturata
```
- **COSA ACCADE**: Con MQTT abilitato, il dispositivo si sottoscrive a <base>/cmd/+ e accetta qualunque messaggio che arrivi su quei topic: non c'è autorizzazione a livello di comando, dipende interamente dagli ACL del broker. Un publisher malevolo (o un'automazione HA errata) invia <base>/cmd/profile con payload 'start' -> startProfileCmd(0,...) riavvia il profilo dallo step 1 e azzera pstate.ogGravity: attenuazione e ABV della cotta in corso vengono persi e la fermentazione riparte dall'inizio, senza alcuna conferma. 'climate_mode off' ferma del tutto il controllo (frigo/riscaldatore inattivi) lasciando la cotta senza termostato. Nessuna di queste azioni richiede la password UI.
- **PERCHÉ ORA**: Richiede MQTT abilitato (opt-in, spento di default) e un broker che consenta la pubblicazione all'attaccante; su broker domestici con ACL laschi o anonimi è concreto.
- **CORREZIONE**: Non far dipendere azioni distruttive solo dagli ACL del broker: richiedere che 'profile start' via MQTT non azzeri l'OG se un profilo è già in marcia (o ignorare 'start' a profilo attivo), e documentare che i topic /cmd/ vanno protetti con ACL dedicati. In alternativa, un flag di configurazione che disabilita i comandi MQTT di scrittura lasciando solo il monitoraggio.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi; il confutatore tecnico eleva a grave: comandi MQTT senza autorizzazione per-comando (start azzera OG, climate_mode off ferma il controllo). Dipende dagli ACL del broker.

### C10 — Riarmo/rimappa a sistema armato: compressore in marcia aperto e richiuso entro ~1s, guardia non registra
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: B  ·  rilievi grezzi: G1-B-3
- **ARCHETIPO (§4)**: nuovo
- **FILE**: src/main.cpp:111-119 ; src/svc/WebApi.cpp:692-711 ; lib/axcore/src/ax/ControlPipeline.h:124-127
- **CODICE**:
```
main.cpp:111-114: static bool armOutputs(uint8_t coolRel, uint8_t heatRel) { if (outputs.testMode()) return false; outputs.allOff();  // un remap a caldo non deve lasciare eccitato il vecchio relè
  outputs.setMapping(coolRel, heatRel);
ControlPipeline.h:124-125: if (guard_.isOn()) { coolFinal = lr.out.cool || guard_.mustStayOn(nowEpoch);
[handleArm (WebApi.cpp:692-711) non contiene alcun controllo sullo stato già armato: rifiuta solo test mode]
```
- **COSA ACCADE**: Stato iniziale: armato, compressore in marcia da 10 min (guard.isOn()==true, latch d'isteresi coolOn). Sequenza: l'utente riconferma o rimappa le uscite (es. cambia solo il relè del caldo) via POST /api/outputs/arm o seriale `arma`: armOutputs esegue allOff() — il relè del compressore si apre fisicamente — poi setMapping. Né allOff né il riarmo passano dalla CompressorGuard: guard_.update viene chiamata solo dalla pipeline con lo stato deciso. Al controlTick successivo (≤1 s) il latch è ancora coolOn e guard_.isOn() è ancora true → coolFinal=true → il relè viene richiuso. Conseguenza osservabile: il compressore subisce un'interruzione di ~0,2-1 s e una richiusura col motore in rotazione e la pressione di mandata piena — esattamente il colpo che restSec esiste per impedire; ogni ripetizione del comando ripete il colpo. Variante equivalente: disarm+arm in rapida successione dentro lo stesso intervallo fra due controlTick (nessun tick in failsafe in mezzo → la guardia non registra mai l'off).
- **PERCHÉ ORA**: Il remap a caldo è un flusso previsto (il commento a main.cpp:113 lo dichiara); basta un click ripetuto sulla conferma del mapping in UI o uno script di provisioning. Nessuna conferma lato API lo impedisce.
- **CORREZIONE**: Due strade: (a) in armOutputs, se già armato e il relè freddo non cambia, non aprirlo affatto (allOff selettivo sugli altri relè); (b) se il relè freddo cambia o si arriva da disarm, notificare la guardia dell'off fisico (guard.update(now,false) o assumeJustStopped) così che la richiusura attenda restSec. La (b) da sola è fail-closed: blocca il freddo per restSec a ogni riconferma del mapping — accettabile (5 min) ma va reso visibile in UI (coolBlockedSec già esiste); la (a) evita il problema alla radice per il caso più frequente.
- **ADJUDICAZIONE DEL REVISORE**: Riarmo a caldo apre e richiude il compressore <1s bypassando la guardia. Su REL3 (GPIO) è reale. Richiede azione utente (riarmo durante marcia). Grave (corto ciclo del compressore, archetipo già pagato 2 volte).
- **VOTO TECNICO** (reale=True): Ho tracciato il percorso riga per riga e i tre riferimenti sono corretti. armOutputs (main.cpp:111-119): se non in test mode esegue outputs.allOff() (riga 113) poi setMapping (114); allOff() (OutputBus.h:40-43) chiama writeRel(r,false) per r=1..8 e writeRel (OutputBus.h:86-98) fa digitalWrite/mcp->setPin senza alcun debounce o min-switch: il relè del compressore, che era HIGH (cool_==true), viene fisicamente aperto subito. Né armOutputs né allOff notificano la CompressorGuard: guard_.update è in…
- **VOTO IMPATTO** (reale=False, sev=minore): Il meccanismo è verificato nel codice: `src/main.cpp:113-114` armOutputs chiama allOff() (che in `src/hal/OutputBus.h:40-43` porta fisicamente LOW il relè del compressore) e setMapping senza notificare la guardia; `lib/axcore/src/ax/ControlPipeline.h:69-72` mostra che l'unico guard_.update(now,false) sta nel ramo failsafe (raggiunto solo se outputsAllowed()==false), ma in un re-arm a caldo notArmed resta false (`src/main.cpp:116`, `lib/axcore/src/ax/SafetyMonitor.h:16-18`) quindi guard_.on_ rest…

### C11 — Commutazioni in modalità test non aggiornano CompressorGuard: dopo il collaudo riavvio senza riposo
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: B  ·  rilievi grezzi: G1-B-4
- **ARCHETIPO (§4)**: nuovo
- **FILE**: src/hal/OutputBus.h:75-79 ; src/main.cpp:467-473 ; src/svc/WebApi.cpp:1347-1374 ; lib/axcore/src/ax/CompressorGuard.h:68-71
- **CODICE**:
```
OutputBus.h:75-79: bool testRelay(uint8_t rel, bool on) { if (!testMode_ || rel < 1 || rel > 8) return false; testModeSinceMs_ = millis(); return writeRel(rel, on); }  [scrittura diretta, nessun aggancio alla guardia]
CompressorGuard.h:69: if (since == 0) return INT64_MAX;  // mai avvenuto: nessun vincolo
main.cpp:471: if (!outputs.testMode()) outputs.enterTestMode(pstate.armed);
```
- **COSA ACCADE**: Stato iniziale: collaudo iniziale — sistema mai armato (epoch della guardia a 0 = nessun vincolo) o disarmato da ore. Sequenza: l'utente identifica il relè del frigo dalla pagina diagnostica o dalla seriale: REL4 ON → il compressore parte davvero; dopo 1 min REL4 OFF. Esce dal test e arma (`arma 4 2` o POST /api/outputs/arm). La temperatura è sopra setpoint+tol → al primo controlTick canStart interroga lastOffEpoch/lastOnEpoch che valgono 0 (o un istante di ore prima): elapsed=INT64_MAX ≥ restSec → il compressore riparte pochi secondi dopo lo stop manuale, a caldo, contro la pressione di mandata. La guardia non ha mai visto le commutazioni del test: durante il test il controlTick gira in failsafe (notArmed) e guard_.update(now,false) è un no-op perché on_ era già false. Conseguenza osservabile: hot-restart del compressore proprio nella sequenza più tipica del primo giorno di vita del dispositivo.
- **PERCHÉ ORA**: È il flusso di messa in servizio descritto dallo stesso hardware_map.h ("Mapping configurato da UI dopo il test manuale relè"): test → arma → avvia. Il mandato segnala che l'aggiramento del riposo compressore è già accaduto due volte in questo progetto.
- **CORREZIONE**: Registrare nella guardia le transizioni fisiche fatte in test quando il relè toccato è (o diventa all'arm) quello mappato freddo; oppure, più conservativo, chiamare assumeJustStopped all'uscita dal test o all'atto dell'arm se un relè è stato commutato durante il test. Il compromesso della via conservativa: restSec di attesa anche quando si è testato solo il caldo — mitigabile tracciando QUALE relè è stato commutato.
- **ADJUDICAZIONE DEL REVISORE**: Commutazioni test non aggiornano CompressorGuard → dopo il collaudo il compressore riparte a caldo senza riposo. Momento sorvegliato ma il danno (compressore avviato contro pressione) è fisico. Grave.
- **VOTO TECNICO** (reale=True): Ho verificato ogni riferimento riga per riga e ho cercato una guardia a monte che impedisca la sequenza: non l'ho trovata. Il difetto regge.

RIFERIMENTI CITATI, TUTTI ESATTI:
- OutputBus.h:75-79 `testRelay`: `if (!testMode_ || rel < 1 || rel > 8) return false; testModeSinceMs_ = millis(); return writeRel(rel, on);`. Scrittura diretta al relè fisico (writeRel, righe 86-98) su qualsiasi rel 1..8, indipendente dal mapping cool/heat e senza alcun aggancio a CompressorGuard. Confermato.
- Compressor…
- **VOTO IMPATTO** (reale=False, sev=minore): Catena tecnica confermata riga per riga: in test `testRelay` scrive diretto senza toccare la guardia (src/hal/OutputBus.h:75-79); durante il test il pipeline gira in failsafe e `guard_.update(now,false)` è un no-op perché on_ era già false (lib/axcore/src/ax/ControlPipeline.h:71); all'uscita/arm si fa allOff (src/hal/OutputBus.h:52-57, src/main.cpp:486-487, src/main.cpp:111-113) ma gli epoch della guardia restano 0, quindi elapsed=INT64_MAX (lib/axcore/src/ax/CompressorGuard.h:69) e canStart pas…

### C16 — MQTT climate_temp con payload non numerico → setpoint 0°C (toFloat()=0 accettato dal range)
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: C  ·  rilievi grezzi: G1-C-4
- **ARCHETIPO (§4)**: nuovo
- **FILE**: src/main.cpp:632-637 ; src/svc/MqttSvc.cpp:29-38 ; ~/.platformio/packages/framework-arduinoespressif8266/cores/esp8266/WString.cpp (toFloat)
- **CODICE**:
```
main.cpp:632-637: if (cmd == "climate_temp") {
      float v = payload.toFloat();
      if (v >= -10 && v <= 40) {
        engine.overrideSetpoint(v);
        persistNow();
      }
--- WString.cpp: float String::toFloat(void) const {
    if (buffer())
        return atof(buffer());
    return 0.0F;
}
```
- **COSA ACCADE**: Stato iniziale: profilo in corso a 19 °C, integrazione Home Assistant collegata via MQTT (topic .../cmd/climate_temp annunciato dalla discovery). Sequenza: un'automazione con un template rotto (o un'entità 'unavailable') pubblica 'None', 'unknown' o stringa vuota sul topic; MqttSvc passa il payload grezzo, toFloat()/atof restituisce 0.0, che supera il controllo v >= -10 && v <= 40 → engine.overrideSetpoint(0.0). Conseguenza osservabile: il setpoint del profilo diventa 0 °C in silenzio; l'override sopravvive fino al prossimo cambio step — su uno step Gravity può significare settimane — e viene pure persistito, quindi sopravvive ai riavvii. Il frigo porta 23 litri verso 0 °C: fermentazione stallata o birra congelata. Lo status mostra setpoint 0.00, ma il dispositivo è non sorvegliato e nessun allarme scatta.
- **PERCHÉ ORA**: Il topic è pubblicato dalla discovery HA proprio per essere automatizzato; un template errato o un'entità sorgente 'unavailable' che si serializza come stringa è il guasto più comune delle automazioni HA. Il broker è in LAN e il comando non richiede alcuna conferma.
- **CORREZIONE**: Parsare il payload con strtod verificando il puntatore di fine (o rifiutare se la stringa non è interamente numerica) prima del range check; considerare inoltre se 0 °C — sotto il freeze-floor di -1? no, sopra — comunque un setpoint valido debba essere accettato da MQTT senza distinguerlo da un errore di parsing: il punto è non fabbricare mai un numero da un payload che non lo contiene.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO. climate_temp toFloat()→0 accettato → setpoint 0°C su profilo in corso. Grave.

### C17 — Freeze-guard -1°C solo per gli step: /api/setpoint, /api/thermostat e MQTT accettano fino a -10°C (rischio congelamento)
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: C,H  ·  rilievi grezzi: G1-C-5,G1-H-4
- **ARCHETIPO (§4)**: 5
- **FILE**: src/svc/WebApi.cpp:16-20 ; src/svc/WebApi.cpp:664-668 ; src/svc/WebApi.cpp:682-686 ; src/main.cpp:633-634 ; src/svc/WebApi.cpp:508-510
- **CODICE**:
```
WebApi.cpp:16-18: // Freeze-guard: sotto -1 °C l'overshoot del frigo può portare la birra verso il
// punto di congelamento (~-3 °C a 5% ABV): il floor del setpoint è -1.
if (steps[i].targetC < -1 || steps[i].targetC > 40 ...
--- WebApi.cpp:665-668: if (v < -10 || v > 40) {
      sendErr(400, "setpoint fuori range (-10..40)");
      return;
    }
    r_.engine->overrideSetpoint(v);
--- WebApi.cpp:683-684: if (sp < -10 || sp > 40) {
      sendErr(400, "setpoint fuori range (-10..40)");
--- main.cpp:633-634: float v = payload.toFloat();
      if (v >= -10 && v <= 40) {
```
- **COSA ACCADE**: Stato iniziale: modalità termostato su una cotta finita, birra a 2 °C. Sequenza: l'utente vuole scrivere -1.0 e digita '-10' (uno slittamento del separatore), o un cursore HA impostato al minimo manda -10 → handleThermostat/handleSetpoint accettano (il range dichiarato è -10..40) → l'isteresi tiene il freddo in marcia finché la camera non arriva a -10+tol, cioè al minimo fisico del vano — in un congelatore a pozzetto usato da keezer ben sotto lo zero. Conseguenza osservabile: la birra congela (espansione, fermentatore o bottiglie danneggiate, cotta persa), esattamente il rischio che il commento di validateSteps dichiara di voler impedire con il floor a -1. Nota per la confutazione: le due letture sono entrambe compatibili col codice — (a) i -10 di termostato/override sono una flessibilità deliberata per usi non-birra; (b) il floor è stato aggiunto agli step e al cold crash (-1..12) e queste tre porte sono state dimenticate. Il codice non contiene alcun commento che giustifichi la (a), mentre l'invariante scritta esiste solo per la (b).
- **PERCHÉ ORA**: Sono le tre vie di comando manuali più usate (pagina, API, HA); il valore è a un errore di battitura di distanza e nessuna conferma è richiesta sotto 0 °C.
- **CORREZIONE**: Allineare il floor a -1 °C su handleSetpoint, handleThermostat e sul comando MQTT (una costante condivisa col validateSteps); se il caso d'uso keezer è voluto, farne un'opzione esplicita di configurazione che alza il limite, non il default.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi. Freeze-guard -1°C solo per step/coldcrash; setpoint/thermostat/MQTT accettano -10°C → rischio congelamento. Grave. 2 assi.

### C18 — Cattura automatica OG protetta contro partenza ritardata ma non contro la pausa (né contro lastTick vecchio 1s)
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: C,F  ·  rilievi grezzi: G1-C-6,G1-F-6
- **ARCHETIPO (§4)**: 5
- **FILE**: src/main.cpp:183-192 ; lib/axcore/src/ax/ProfileEngine.h:76
- **CODICE**:
```
main.cpp:186-191: // NON durante l'attesa di partenza ritardata: lo iSpindel potrebbe essere ancora
  // nel secchio di sanificante, e quell'OG avvelenerebbe l'attenuazione della cotta.
  if (pstate.ogGravity <= 0.0f && engine.running() && !lastTick.waitingDelayedStart) {
    pstate.ogGravity = gravity.gravity();
    persistNow();
--- ProfileEngine.h:76: bool running() const { return s_.mode == RunMode::Profile && !s_.completed; }  // paused incluso
```
- **COSA ACCADE**: Stato iniziale: l'utente avvia il profilo la sera e lo mette subito in pausa (pitch del lievito rimandato a domattina) — l'alternativa manuale alla partenza ritardata; lo iSpindel è ancora nel secchio di sanificante e trasmette. Sequenza: onIspindel riceve la prima lettura valida (es. 1.001); pstate.ogGravity è 0, engine.running() è vero anche da paused, waitingDelayedStart è falso → OG=1.001 catturata e persistita. Conseguenza osservabile: per tutta la cotta attenuazione e ABV in /api/status, OLED e MQTT sono calcolati da un'OG falsa (con OG≈1.0 il ramo SG dà attenuationValid solo marginalmente e percentuali senza senso clampate a 0); uno step GravityMode::Attenuation non raggiunge mai il target e avanza solo all'eventuale timeout. Il valore è correggibile a mano via POST /api/gravity/og, ma solo se l'utente si accorge che il numero mostrato è falso — il commento del codice dimostra che questo esatto avvelenamento era considerato reale, e la guardia copre una sola delle due vie con cui si aspetta prima di pitchare.
- **PERCHÉ ORA**: La sequenza avvio+pausa è un pattern d'uso naturale per chi non conosce (o non trova) la partenza ritardata; basta una singola trasmissione dello iSpindel nella finestra di pausa.
- **CORREZIONE**: Escludere la pausa dalla cattura (condizione && !engine.paused()), che rende la guardia coerente con la sua motivazione dichiarata; da valutare se la cattura debba anche attendere la prima lettura DOPO l'uscita dalla pausa iniziale.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO (impatto lo pone minore). Cattura OG in pausa/finestra 1s registra la densità del sanificante → attenuazione/ABV falsi per l'intera cotta. Tengo grave (numero falso persistente su decisione di imbottigliamento).

### C22 — Nessun limite superiore a logRetentionDays né controllo spazio: il registro può riempire la LittleFS
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: probabile
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: D  ·  rilievi grezzi: G1-D-2
- **ARCHETIPO (§4)**: 15
- **FILE**: src/svc/ConfigStore.cpp:190-193 ; src/svc/AppConfig.h:62-63 ; src/svc/DataLog.cpp:10-16 ; assenza di qualunque uso di FSInfo/usedBytes in src/ (verificato con grep)
- **CODICE**:
```
ConfigStore.cpp:190-193 (fromJson):
    c->logSampleMin = lg["sampleMin"] | c->logSampleMin;
    if (c->logSampleMin < 1) c->logSampleMin = 1;
    c->logRetentionDays = lg["retentionDays"] | c->logRetentionDays;
    if (c->logRetentionDays < 1) c->logRetentionDays = 1;

DataLog.cpp:10-15 (append, nessun controllo di spazio):
  File f = LittleFS.open(dayPath(s.epoch / 86400), "a");
  if (!f) return false;
  size_t n = f.write((const uint8_t*)&s, sizeof(s));
```
- **COSA ACCADE**: Stato iniziale: utente imposta retentionDays=365 ('voglio un anno di storico') via PUT /api/config; il campo è uint16 e il clamp esiste solo verso il basso. Sequenza: ogni file giornaliero occupa almeno 1-2 blocchi da 4 KB su LittleFS (4,6 KB/giorno a campionamento default 5 min; 23 KB/giorno a sampleMin=1). Con ~2 MB di partizione (platformio.ini:2, layout 4m2m) 365 file da 2+ blocchi superano i ~500 blocchi disponibili: il filesystem si riempie dopo alcuni mesi di funzionamento normale, cioè dentro la durata di una lager. Conseguenza osservabile: da quel momento ogni stateStore.save fallisce in silenzio (nessun controllo, vedi rilievo dedicato), ogni save di config/profilo può promuovere un file troncato (rilievo precedente), dataLog.append fallisce e il registro si buca. Nessun allarme in nessun punto: la UI mostra 'tutto a posto'.
- **PERCHÉ ORA**: Il valore è accettato da PUT /api/config senza tetto e il consumo cresce lentamente e in silenzio per mesi: esattamente il profilo temporale del dispositivo non sorvegliato descritto in §0. Non esiste alcuna misura dello spazio libero in tutta la codebase (grep su usedBytes/totalBytes/FSInfo: zero risultati).
- **CORREZIONE**: Clampare logRetentionDays a un massimo compatibile con la partizione (es. 60-90 giorni) come già si fa verso il basso; in aggiunta, misurare periodicamente lo spazio libero (FSInfo) e sotto una soglia forzare la cleanup e accendere un allarme di storage in AppStatus. Le due strade sono complementari: il solo clamp non copre il riempimento da altre cause.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO (impatto minore: 2MB FS difficile da riempire con log da 64KB). Tengo grave→minore: il tetto mancante è reale ma il riempimento è improbabile nella durata di una cotta.

### C23 — persistNow e dataLog.append ignorano l'esito della scrittura: persistenza fallisce per settimane senza segnale
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: D  ·  rilievi grezzi: G1-D-3
- **ARCHETIPO (§4)**: 16
- **FILE**: src/main.cpp:70-81 ; src/main.cpp:371 ; src/svc/AppStatus.h:30-31 ; esempio di comando che finge successo: src/svc/WebApi.cpp:687-689
- **CODICE**:
```
main.cpp:80:
  if (fsOk) stateStore.save(pstate);
(valore di ritorno mai controllato; fsOk è deciso una volta sola al boot, main.cpp:537)

main.cpp:371:
  dataLog.append(s);
(idem)

AppStatus.h:30-31 (tutti gli allarmi esistenti — nessuno per la persistenza):
  bool alarmProbe = false, alarmConfig = false, alarmRtc = false, alarmGravity = false,
       alarmBattery = false, alarmOutputs = false;

WebApi.cpp:687-689 (handleThermostat):
  r_.engine->setThermostat(on, sp);
  r_.persistNow();
  sendOk();
```
- **COSA ACCADE**: Stato iniziale: filesystem pieno (o LittleFS che inizia a fallire per usura/settore difettoso) durante una fermentazione; ultima save riuscita al tempo T0. Sequenza: l'utente porta il termostato da 18 a 10 °C per un lagering (POST /api/thermostat): setThermostat viene applicato in RAM, persistNow fallisce in silenzio, la risposta è comunque {ok:true}. Il dispositivo lavora correttamente a 10 °C per giorni. Poi un blackout: al riavvio stateStore.load ripristina lo stato di T0 -> setpoint 18 °C. Conseguenza osservabile: la birra viene tenuta a 18 °C invece che a 10 per giorni, senza alcun allarme; nel registro non c'è traccia del periodo (append falliti anch'essi in silenzio), quindi il guasto non è nemmeno ricostruibile a posteriori. Lo stesso vale per arm/disarm, OG, pause/skip: ogni comando 'confermato' può regredire al riavvio.
- **PERCHÉ ORA**: fsOk fotografa solo il mount al boot: qualunque fallimento successivo (FS pieno, errore di scrittura) non ha un percorso per diventare visibile — non esiste un campo di allarme storage in AppStatus, niente su OLED, MQTT o /api/status. È la clausola centrale della missione (§1): un guasto che il dispositivo non sa raccontare. Il trigger non richiede condizioni esotiche: basta il rilievo sulla retention, o l'invecchiamento della flash.
- **CORREZIONE**: Propagare l'esito: un contatore/flag di 'ultima persistenza fallita' alimentato da stateStore.save e dataLog.append, esposto come allarme in AppStatus (UI/OLED/MQTT) e nel log seriale; i comandi HTTP che chiamano persistNow dovrebbero rispondere 500 (o ok con warning esplicito) quando la save fallisce, come già fanno i percorsi di calibrazione.
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi. persistNow/append ignorano l'esito; nessun allarme storage; al blackout lo stato regredisce in silenzio. Trigger realistico = usura flash a fine vita. Grave.

### C24 — handlePutConfig muta cfg in RAM prima del save e non fa rollback: sul 500 password e mezza config cambiano comunque
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: D,F  ·  rilievi grezzi: G1-D-4,G1-F-4
- **ARCHETIPO (§4)**: 6
- **FILE**: src/svc/WebApi.cpp:328-340 ; uso live dei campi mutati: src/svc/WebApi.cpp:151-153 e 169 (uiPassword in auth), src/main.cpp:328 (gravityUnit in controlTick), src/main.cpp:175-182 (gravityDevice/batteryAlertV), src/main.cpp:736 (logSampleMin) ; contrasto (rollback corretto): src/svc/WebApi.cpp:1138-1147
- **CODICE**:
```
WebApi.cpp:328-340:
  AppConfig& c = *r_.cfg;
  ...
  r_.cfgStore->fromJson(&c, doc);
  c.valid = true;
  if (!r_.cfgStore->save(c)) {
    sendErr(500, "salvataggio fallito");
    return;
  }
(nessun rollback; applyConfigLive viene chiamata solo dopo il save riuscito, riga 348)

WebApi.cpp:153 (auth legge la password live):
  if (tokenPass_ != r_.cfg->uiPassword) computeToken();
```
- **COSA ACCADE**: Stato iniziale: FS pieno o scrittura fallita. Sequenza: l'utente cambia password UI e unità di densità (sg->plato) in un solo PUT /api/config; save fallisce; risposta 500 'salvataggio fallito'. Ma fromJson ha già mutato cfg: (1) il login e ogni auth() richiedono da subito la password NUOVA che l'utente crede non salvata; (2) controlTick calcola da subito l'attenuazione con l'unità nuova su OG catturata nell'unità vecchia, e il clamp di /ispindel passa alla scala nuova, mentre pipeline/sonde restano sulla config vecchia (applyConfigLive non chiamata): stato misto mai esistito come configurazione intera. Conseguenza osservabile: al riavvio tutto regredisce alla config su flash — la password torna la vecchia e i cancelli a densità tornano coerenti — senza alcun avviso: RAM e flash sono divergenti per tutto l'uptime.
- **PERCHÉ ORA**: È la stessa forma dell'archetipo 6 già pagata da questo progetto ('stato applicato prima della validazione: una richiesta rifiutata lasciava RAM e flash divergenti'); il handler della taratura sonde (WebApi.cpp:1138-1147) fa il rollback correttamente, quindi l'invariante è nota e violata in un punto solo. Raggiungibile con lo stesso trigger dei rilievi precedenti (FS piena).
- **CORREZIONE**: Applicare fromJson a una copia di AppConfig, salvare la copia, e solo a save riuscita assegnarla a cfg (poi applyConfigLive). In alternativa, salvare lo stato precedente e ripristinarlo sul fallimento, come fa handleProbeCalib per gli offset.
- **ADJUDICAZIONE DEL REVISORE**: Tecnico airtight, impatto lo pone minore (self-heal al reboot, no rischio fisico). Tengo grave: lockout password + commit permanente via calib + stato misto. Richiede save fallito (raro).
- **VOTO TECNICO** (reale=True): Tutti i riferimenti verificati e corretti. WebApi.cpp:328 'AppConfig& c = *r_.cfg;' e' un riferimento, e refs.cfg=&cfg (main.cpp:660) con AppConfig cfg globale (main.cpp:40): fromJson muta direttamente la config viva. WebApi.cpp:335-340: fromJson->c.valid=true->save; su save false, sendErr(500) e return SENZA rollback e SENZA applyConfigLive (che e' a riga 348). ConfigStore::fromJson (ConfigStore.cpp:115,176,175,178,190) sovrascrive in-place i campi presenti (uiPassword via setSecret, gravityUni…
- **VOTO IMPATTO** (reale=False, sev=minore): Difetto di codice confermato: WebApi.cpp:328 `AppConfig& c = *r_.cfg` e' un riferimento alla config viva, fromJson (335) la muta in place, sul save fallito (337-340) si esce con 500 senza rollback ne applyConfigLive, mentre handleProbeCalib (1143-1147) fa il rollback corretto. Campi letti live (uiPassword auth WebApi.cpp:152/169, gravityUnit main.cpp:328, batteryAlertV/gravityDevice main.cpp:175-182, logSampleMin main.cpp:736) agiscono subito. Ma sul piano dell'impatto la catena si sgonfia: (1)…

### C29 — Export del registro (CSV/JSON) sincrono nel loop di controllo: un client lento congela il controllo per minuti/ore
- **SEVERITÀ**: grave  (rilievo originale: critico)
- **CONFIDENZA**: da verificare sul campo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: E,F  ·  rilievi grezzi: G1-E-3,G1-F-2
- **ARCHETIPO (§4)**: nuovo
- **FILE**: src/svc/WebApi.cpp:1166-1205 ; src/svc/WebApi.cpp:1207-1246 ; src/svc/DataLog.cpp:29-48 ; src/main.cpp:719-727
- **CODICE**:
```
handleLogCsv(): r_.log->forEach(from, now, [&](const DataLog::Sample& s){ line = String(s.epoch)+";"+r_.time->localString(s.epoch)+";"; ...; server_.sendContent(line); return true; });  // WebApi.cpp:1182-1203
forEach(): for (uint32_t day=fromDay; day<=toDay; day++) { yield(); File f=...; while (f.read(...)) { ...; cb(s); } }  // DataLog.cpp:35-45 (yield UNA volta per giorno, non per campione)
loop(): web.loop();  // main.cpp:719 (handleClient qui) ... if (nowMs-lastCtlMs>=1000) controlTick(nowMs);  // :723
```
- **COSA ACCADE**: Stato iniziale: retention di default 14 giorni, campionamento 5 min => ~4032 record; profilo in marcia con ciclo di raffreddamento attivo. Sequenza: un client (browser dell'utente, magari su WiFi debole) chiede GET /api/log.csv senza parametro hours. handleLogCsv itera tutti i giorni: per ogni campione costruisce una riga (allocazione String di localString + concatenazioni) e chiama server_.sendContent, che dentro un file-giorno NON esegue yield tranne l'attesa TCP. Tutto avviene dentro web.loop(), PRIMA di controlTick e della prossima probes.loop. Se il client rallenta o sparisce, ogni chunk attende fino a HTTP_MAX_SEND_WAIT=5000 ms l'ACK (verificato nella libreria). Conseguenza osservabile: controlTick, campionamento sonde e MQTT congelati per l'intera esportazione (secondi, fino a ~5 s aggiuntivi con client stallato); i rele' restano nell'ultimo stato comandato, e un ciclo di frigo con isteresi stretta prosegue oltre il punto di spegnimento fino a quando il loop riprende.
- **PERCHÉ ORA**: Raggiungibile con un uso normale (scaricare il grafico storico) su una connessione lenta, o con un client che apre la richiesta e non legge. La grazia sonde (kStallMs 15 s) maschera lo stallo lato sonde, quindi il congelamento NON emette alcun allarme.
- **CORREZIONE**: Rendere l'esportazione non bloccante o a passo limitato: yield/anti-starvation ogni N campioni (non solo per file-giorno), tetto esplicito al numero di record/righe per risposta, e/o eseguire controlTick con priorita' anche durante download lunghi. In alternativa limitare la finestra massima esportabile in una singola richiesta e imporre paginazione. Valutare un timeout di invio piu' corto per non trattenere il loop 5 s su un client morto.
- **ESPERIMENTO**: Con profilo attivo, avviare GET /api/log.csv su 14 giorni di dati e misurare via seriale il ritardo tra due 'controlTick' consecutivi (o l'intervallo dei log heap a 60 s); ripetere aprendo la connessione e sospendendo la lettura lato client per osservare lo stallo fino a 5 s. Verificare se in quel lasso un cambio di stato rele' atteso viene ritardato.
- **ADJUDICAZIONE DEL REVISORE**: Tecnico confermato; impatto lo pone minore (stallo limitato a ~1-2 min, WDT nutrito, relè congelati ma recuperabile). Da critico a grave: stallo del controllo reale ma limitato.
- **VOTO TECNICO** (reale=True): Ho cercato la ragione per cui il difetto NON accade e non l'ho trovata: ogni anello della catena regge alla lettura riga per riga, e i riferimenti file:riga sono esatti.

RIFERIMENTI VERIFICATI
- WebApi.cpp:1166-1205 handleLogCsv: dopo auth() (riga 1167), il from viene clampato SOLO alla retention (righe 1173-1174: `retSec = logRetentionDays*86400`; `if (now>retSec && from<now-retSec) from=now-retSec`), ma NON c'e alcun tetto al numero di record. Il callback (righe 1182-1202) costruisce una Stri…
- **VOTO IMPATTO** (reale=False, sev=minore): Meccanica confermata: handleLogCsv (WebApi.cpp:1182-1203) esaurisce l'intero forEach dentro una sola handleClient, quindi controlTick (main.cpp:723-726) non gira fino a fine export; DataLog::forEach yield-a una volta per file-giorno non per campione (DataLog.cpp:35-45); timeout invio HTTP_MAX_SEND_WAIT=5000 impostato a impl.h:344, short-send solo a debug e prosegue (impl.h:538-540). Retention default 14 gg -> ~4032 record; toleranceC=0.5 (AppConfig.h:35); grazia sonde kGraziaMs=20s (ProbeBus.h).…

### C36 — Log/CSV: densità °Plato negativa trabocca in uint16 (655°P) e 0°P legittimo registrato come 'mancante'
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: G  ·  rilievi grezzi: G1-G-3
- **ARCHETIPO (§4)**: 13
- **FILE**: src/main.cpp:367-369 ; src/svc/DataLog.h:16 ; src/svc/WebApi.cpp:740-741 ; src/svc/WebApi.cpp:1197 ; data-src/index.html:739-747
- **CODICE**:
```
main.cpp:367-369: `// SG x10000 (1.0105 -> 10105); in °Plato x100 (12.5 -> 1250): entrambi in uint16.` / `float gscale = cfg.gravityUnit == "plato" ? 100.0f : 10000.0f;` / `s.gravity = curStatus.gravityHas ? (uint16_t)lroundf(curStatus.gravity * gscale) : 0;`. DataLog.h:16: `uint16_t gravity;   // SG x10000 (o °P x100); 0 = mancante`. WebApi.cpp:740-741: `float gmin = plato ? -1.0f : 0.90f ...` — l'ingest AMMETTE °P fino a -1. WebApi.cpp:1197 (CSV): `line += s.gravity ? String(s.gravity / gdiv, plato ? 2 : 4) : "";`.
```
- **COSA ACCADE**: Stato iniziale: unità °Plato, cotta quasi attenuata (o i modelli 'Sidro secco 71B' / 'Idromele' della libreria, che finiscono sotto 1.000 SG ≈ °P negativi). Sequenza: lo iSpindel invia -0,3 °P; l'ingest lo accetta (clamp -1..45); al campione successivo `(uint16_t)lroundf(-0.3*100)` = 65506. Conseguenza osservabile: il CSV scaricato contiene '655.06' nella colonna densita; il grafico densità scala l'asse a 655 °P e schiaccia l'intera curva a una riga piatta; 'Attività punti/giorno' e 'densità stabile ✓' in Andamento, calcolati dagli stessi punti (index.html:781-792), diventano privi di senso. Caso complementare: una lettura di 0,00 °P (acqua/FG raggiunta) codifica 0 = sentinella 'mancante' → buco nel grafico e riga vuota nel CSV proprio nei giorni in cui si decide l'imbottigliamento.
- **PERCHÉ ORA**: Le densità attorno o sotto 0 °P sono l'esito normale di sidri, idromele e birre molto secche (modelli inclusi nella libreria), e un piccolo errore di taratura dello iSpindel porta sotto zero anche una birra normale a fine fermentazione. L'unica sonda del guasto è il registro storico, cioè esattamente il dato che l'utente usa per decidere.
- **CORREZIONE**: Separare la sentinella dal dominio dei valori: usare 0xFFFF come 'mancante' e codificare il °P con offset (es. (p+2)*100, clampato a [0..47]) oppure clampare a 0 in scrittura ma con sentinella distinta dal valore 0. Richiede di versionare/riconoscere i record esistenti in lettura (CSV e /api/log decodificano con la stessa regola).
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi. Densità °P negativa trabocca in uint16 (655°P) nel log/CSV; 0°P legittimo = 'mancante'. Grafico/statistiche falsati. Grave.

### C37 — Dashboard congelata indistinguibile da viva: refresh() ingoia gli errori di rete, la pagina mostra dati vecchi di ore come attuali
- **SEVERITÀ**: grave  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: G  ·  rilievi grezzi: G1-G-4
- **ARCHETIPO (§4)**: 17
- **FILE**: data-src/index.html:613 ; data-src/index.html:2261 ; data-src/index.html:2155-2160
- **CODICE**:
```
index.html:613: `}catch(e){/* rete assente: si ritenta */}}` (chiusura di refresh()). index.html:2261: `setInterval(refresh,2000);`. Contrasto con il pannello sonde, che invece segnala: index.html:2158-2159: `$("probe_err").textContent='dati non aggiornati ('+e.message+'): quelli mostrati potrebbero essere obsoleti.';`
```
- **COSA ACCADE**: Stato iniziale: dashboard aperta su un tablet (la pagina è costruita come web-app da tenere aperta: meta apple-mobile-web-app, 'resti collegato su questo dispositivo'), profilo in marcia, frigo comandato. Sequenza: alle 02:00 il firmware crasha in boot-loop, o il WiFi cade, o salta la corrente al controllore. refresh() fallisce ogni 2 s e il catch scarta l'errore senza toccare il DOM. Conseguenza osservabile: per ore o giorni la pagina mostra l'ultima temperatura come attuale, il badge ARMATO, la pill RAFFREDDA e l'età densità congelata ('12 min fa' per sempre); nessun banner, nessun timestamp di ultimo aggiornamento in tutta la dashboard. Il proprietario che 'controlla dalla pagina' vede tutto a posto mentre il controllo è morto — lo scenario che il §1 definisce peggiore del guasto stesso.
- **PERCHÉ ORA**: Qualunque caduta WiFi/crash/blackout durante i mesi di esercizio; il pattern d'uso a pagina sempre aperta è quello che la pagina stessa promuove. Il rimedio esiste già nel codice (pannello sonde) ma non è stato applicato alla vista principale.
- **CORREZIONE**: Nel catch di refresh() marcare lo stato come stantio (banner 'connessione persa — dati di HH:MM' e/o desaturare i numeri) dopo N fallimenti consecutivi; in alternativa mostrare sempre un 'aggiornato alle HH:MM:SS' accanto all'orologio, come già fa il pannello sonde. Stessa cura per loadLog().
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO da entrambi. Dashboard congelata indistinguibile da viva (refresh ingoia errori) → mostra dati vecchi di ore come attuali. Archetipo 17. Grave.

---

## Rilievi minori
### C02 — /ispindel non autenticato: scritture flash illimitate su devices.json + sfratto densimetri reali (usura pilotabile da LAN)
- **SEVERITÀ**: minore  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: A,D,E  ·  rilievi grezzi: G1-A-2,G1-D-8,G1-E-1
- **ARCHETIPO (§4)**: nuovo (scrittura flash non limitata su percorso non autenticato; affine 7 per azzeramento distruttivo su percorso frequente)
- **FILE**: src/main.cpp:165-168 ; src/svc/DeviceRegistry.h:25-49 ; src/svc/DeviceRegistry.cpp:25-44 ; src/svc/WebApi.cpp:97
- **CODICE**:
```
main.cpp:165  if (devices.seen(devName, devId, timeSvc.nowEpoch()) && fsOk) { devices.save(); ... }
DeviceRegistry.h:38  if (n_ < kMax) { devs_[n_++] = d; } else { // Registro pieno: sostituisce il meno recente ... devs_[old] = d; }
DeviceRegistry.h:48  return true;   // strumento nuovo -> il chiamante scrive su flash
DeviceRegistry.cpp:34  File f = LittleFS.open("/devices.tmp", "w"); ... LittleFS.rename("/devices.tmp", kPath);
```
- **COSA ACCADE**: kMax=4. Un attore LAN invia POST /ispindel ripetuti con gravity plausibile e, ogni volta, ID e name distinti (es. name=a1/a2/... , ID incrementale). devices.seen non trova corrispondenza -> registro pieno -> sostituisce l'LRU e ritorna true -> devices.save() apre /devices.tmp in scrittura e fa rename: una cancellazione+scrittura di settore LittleFS per ogni richiesta. A poche richieste/secondo si superano rapidamente i cicli di cancellazione del settore -> usura permanente della flash fino al guasto; nel frattempo ogni ID nuovo espelle i densimetri legittimi dall'elenco mostrato in calibrazione. Tutto senza autenticazione e senza alcun rate-limit.
- **PERCHÉ ORA**: Endpoint non autenticato per default; nessun limite di frequenza; la scrittura avviene sul thread del loop, quindi ogni richiesta consuma anche tempo di controllo.
- **CORREZIONE**: Subordinare devices.save() a un token valido; oppure limitare la frequenza di scrittura (persistere il registro al più ogni N minuti, o solo su nuovo device con debounce) e comunque non far dipendere una scrittura flash da input non autenticato. Valutare un tetto di scritture/giorno con allarme.
- **ADJUDICAZIONE DEL REVISORE**: Usura flash richiede flood deliberato e sostenuto (DoS insider LAN, fuori portata). Lo sfratto densimetri dall'elenco è il residuo reale ma recuperabile alla trasmissione successiva. Da grave a minore.
- **VOTO TECNICO** (reale=True): Ho tentato di confutare e ho verificato ogni riferimento file:riga: sono tutti corretti e la catena regge. NON ho trovato guardia, invariante, dedup o rate-limit che impedisca il difetto per l'attacco con ID variabile.

VERIFICA DEI RIFERIMENTI (tutti esatti):
- src/main.cpp:165-168 (onIspindel): `if (devices.seen(devName, devId, timeSvc.nowEpoch()) && fsOk) { devices.save(); Serial.printf(...); }`. Esatto.
- src/svc/DeviceRegistry.h:25-49 seen(): il match a monte è `if (devs_[i].id == id || (na…
- **VOTO IMPATTO** (reale=False, sev=minore): Entrambe le conseguenze richiedono un attaccante deliberato sulla LAN che confeziona ID/nome distinti a ogni richiesta e martella /ispindel: nessun iSpindel reale (ID = chip ID fisso, nome fisso, invio ogni ~15 min in deep sleep) né alcuna misconfigurazione accidentale produce questo pattern, e il dedup esplicito di seen() (DeviceRegistry.h:27, `return false` per ID o nome già noto -> nessuna save) assorbe ogni retry/duplicato. (1) Usura flash: LittleFS fa wear-leveling su ~500 settori, budget ~…

### C04 — Token di sessione deterministico, non scadibile né revocabile lato server, su HTTP in chiaro
- **SEVERITÀ**: minore  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: A  ·  rilievi grezzi: G1-A-4
- **ARCHETIPO (§4)**: nuovo (sessione senza stato lato server; affine alla nota della mappa §7.4)
- **FILE**: src/svc/WebApi.cpp:142-149 ; src/svc/WebApi.cpp:151-170 ; src/svc/WebApi.cpp:191-193 ; src/svc/WebApi.cpp:53-56
- **CODICE**:
```
WebApi.cpp:145  md5.add(r_.cfg->uiPassword + ":" + String(ESP.getChipId(), HEX) + ":axsession");
WebApi.cpp:147  sessionToken_ = md5.toString();
WebApi.cpp:164  if (v.length() && v == sessionToken_) return true;
WebApi.cpp:192  server_.sendHeader("Set-Cookie", String("axs=") + sessionToken_ + "; Max-Age=31536000; Path=/; SameSite=Lax; HttpOnly");
WebApi.cpp:54  server_.sendHeader("Set-Cookie", "axs=; Max-Age=0; Path=/");   // logout: solo lato client
```
- **COSA ACCADE**: Il token è MD5(password+chipId+"axsession"): identico per tutti i client, stabile fra i riavvii, senza scadenza lato server (Max-Age 1 anno lato client). Il logout cancella solo il cookie del browser; non esiste alcun invalidamento server-side. Il server web è HTTP in chiaro (nessun TLS nel firmware). Sequenza: un attaccante sniffa una singola richiesta autenticata sulla LAN (o riusa un browser condiviso) e ottiene il valore axs=. Da quel momento ha accesso completo a tutte le /api/* — inclusi arm/disarm dei relè, setpoint, factory reset e OTA — finché l'utente non cambia la password UI. Cambiare password è l'UNICO modo per revocare; il logout non serve a nulla.
- **PERCHÉ ORA**: Il dispositivo espone HTTP in chiaro sulla LAN domestica per settimane. La cattura passiva del cookie (o dell'header Basic) è realistica su reti condivise; il token non ruota mai e non è revocabile senza cambio password.
- **CORREZIONE**: Introdurre un segreto di sessione casuale generato al boot (o persistito) mescolato nel token, e un elenco/versione di sessioni invalidabile: il logout e un 'revoca tutte le sessioni' devono cambiare il token effettivo senza richiedere il cambio password. Confronto del token a tempo costante. Documentare che senza TLS la confidenzialità del cookie non è garantita.
- **ADJUDICAZIONE DEL REVISORE**: Token deterministico/non revocabile è scelta documentata (ARCHITETTURA.md:8), dentro il modello LAN/HTTP accettato. Hardening consigliabile (rotazione, scadenza) ma non difetto.
- **VOTO TECNICO** (reale=True): Ho letto WebApi.cpp e WebApi.h riga per riga e ho verificato la libreria/framework, non dedotto. Ogni riferimento del rilievo corrisponde al codice reale:

1. Token deterministico: WebApi.cpp:145 `md5.add(r_.cfg->uiPassword + ":" + String(ESP.getChipId(), HEX) + ":axsession")` e :147 `sessionToken_ = md5.toString()`. Il token dipende SOLO da uiPassword e ESP.getChipId(), entrambi stabili fra i riavvii e identici per ogni client. Nessun input per-sessione, nessun casuale, nessun timestamp.

2. Ne…
- **VOTO IMPATTO** (reale=False, sev=minore): Il rilievo è tecnicamente esatto nei fatti (token MD5 deterministico a WebApi.cpp:145-147; confronto non-a-tempo-costante a WebApi.cpp:164; cookie Max-Age 1 anno a WebApi.cpp:191-192; logout solo client a WebApi.cpp:53-56; unica revoca = cambio password via computeToken a WebApi.cpp:153/190), ma NON produce conseguenza concreta per il proprietario, per quattro ragioni convergenti. (1) Il modello di minaccia è esplicito: docs/ARCHITETTURA.md:7 titola la sezione 'Sicurezza (LAN domestica, HTTP in…

### C05 — Nessun rate-limit/lockout: il delay anti-bruteforce è solo su /api/login, aggirabile con Basic auth
- **SEVERITÀ**: minore  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: A  ·  rilievi grezzi: G1-A-5
- **ARCHETIPO (§4)**: nuovo (protezione presente in un punto e assente nel gemello; affine 5 'invariante rispettata ovunque tranne un punto')
- **FILE**: src/svc/WebApi.cpp:185-189 ; src/svc/WebApi.cpp:169 ; src/svc/WebApi.cpp:172-176
- **CODICE**:
```
WebApi.cpp:186  delay(300);  // rallenta la forza bruta senza bloccare a lungo il loop
WebApi.cpp:169  return server_.authenticate("admin", r_.cfg->uiPassword.c_str());   // fallback Basic: nessun delay, nessun lockout
WebApi.cpp:173  if (authValid()) return true;   // usato da tutti gli handler protetti
```
- **COSA ACCADE**: L'unico freno al bruteforce è delay(300) dentro handleLogin. Ma authValid() offre un fallback Basic auth (Authorization: Basic admin:pw) usato da OGNI endpoint protetto, senza alcun ritardo né conteggio dei tentativi né blocco temporaneo. Un attaccante prova password ripetute con GET /api/status e header Basic: ogni tentativo è un round-trip normale, limitato solo dalla rete (centinaia/migliaia al secondo sulla LAN). Non esiste lockout, non esiste backoff, i tentativi non vengono registrati. La password UI è l'unica credenziale del dispositivo; forzata, dà controllo completo (relè, OTA).
- **PERCHÉ ORA**: Il delay su /api/login dà una falsa impressione di protezione; il percorso Basic la annulla del tutto. Raggiungibile da qualunque host LAN in ogni momento.
- **CORREZIONE**: Applicare lo stesso rallentamento/limite a TUTTI i percorsi di verifica credenziale (non solo /api/login): contatore di fallimenti per IP con backoff crescente o lockout temporaneo, e log dei tentativi falliti visibile in UI. Valutare la disattivazione del fallback Basic se non necessario agli script.
- **ADJUDICAZIONE DEL REVISORE**: No lockout/rate-limit sul path Basic auth è hardening reale anche su LAN (brute force senza freno né log), indipendente dallo sniffing. Minore.
- **VOTO TECNICO** (reale=True): Non ho trovato alcuna ragione per cui il difetto non accada; anzi, la lettura riga per riga e la verifica sul framework lo confermano. Riferimenti verificati esatti: WebApi.cpp:186 'delay(300);' e' l'UNICA occorrenza di delay( in tutto il file (grep) ed e' dentro handleLogin, sul solo percorso POST /api/login (WebApi.cpp:185-189). WebApi.cpp:169 'return server_.authenticate("admin", r_.cfg->uiPassword.c_str());' e' il fallback Basic dentro authValid(), e su quel return non c'e' delay, contatore…
- **VOTO IMPATTO** (reale=False, sev=minore): Il rilievo è tecnicamente esatto: `delay(300)` esiste solo in `handleLogin` (src/svc/WebApi.cpp:186), mentre il fallback Basic auth in `authValid()` (src/svc/WebApi.cpp:169) è privo di ritardo/conteggio/lockout ed è raggiunto da `auth()` (src/svc/WebApi.cpp:172-176) che tutti gli handler protetti invocano (es. `handleStatus` src/svc/WebApi.cpp:213). Ma sul piano dell'IMPATTO concreto per il proprietario il percorso non aggiunge nulla oltre a ciò che il modello di minaccia dichiarato già concede,…

### C06 — Corpo POST non autenticato illimitato bufferizzato in heap prima dell'auth: stallo loop o OOM da LAN
- **SEVERITÀ**: minore  (rilievo originale: grave)
- **CONFIDENZA**: probabile
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: A,E  ·  rilievi grezzi: G1-A-6,G1-E-4
- **ARCHETIPO (§4)**: nuovo (DoS/stallo del loop da input non autenticato; affine ai vincoli §0 su handler lenti)
- **FILE**: src/svc/WebApi.cpp:97 ; src/svc/WebApi.cpp:52 ; src/main.cpp:719 ; framework ESP8266WebServer Parsing-impl.h:160-169
- **CODICE**:
```
Parsing-impl.h:163  (   !readBytesWithTimeout<ServerType>(client, contentLength, plainBuf, HTTP_MAX_POST_WAIT)  // HTTP_MAX_POST_WAIT = 5000 ms
Parsing-impl.h:164   || (plainBuf.length() < contentLength) )
Parsing-impl.h:187  arg.value = plainBuf;   // l'intero corpo finisce in una String heap
main.cpp:719  web.loop();   // handleClient() gira dentro il loop cooperativo single-thread
```
- **COSA ACCADE**: ESP8266WebServer legge tutto il corpo di una POST non-form in una String heap PRIMA di invocare l'handler (quindi prima di qualunque controllo auth), con timeout complessivo HTTP_MAX_POST_WAIT=5000 ms e nessun limite su Content-Length. /ispindel (riga 97) e /api/login (riga 52) sono non autenticati. Due varianti: (a) l'attaccante dichiara Content-Length grande (es. 30-40 KB, vicino ai ~19 KB liberi) e invia il corpo: la String cresce con reallocazioni finché l'allocazione fallisce -> frammentazione/OOM, con rischio di reboot; (b) slow-loris: dichiara Content-Length e invia i byte a goccia, tenendo readBytesWithTimeout bloccato fino a 5 s per richiesta. In entrambi i casi web.loop() gira dentro loop() single-thread: per la durata della lettura non vengono eseguiti controlTick, outputs.loop né il delay(2) finale — i relè restano nell'ultimo stato comandato. Richieste consecutive mantengono il controllo effettivamente congelato: il termostato smette di reagire alla temperatura mentre un'uscita può restare eccitata.
- **PERCHÉ ORA**: Endpoint non autenticati raggiungibili da qualunque host LAN; l'heap di esercizio è ~19 KB, quindi un corpo di quella taglia è realistico. La lettura bloccante è inerente al framework ma è esposta perché il web server condivide il loop con il controllo.
- **CORREZIONE**: Rifiutare presto le richieste con Content-Length oltre una soglia (es. rispondere 413 senza leggere il corpo) e, dove possibile, rigettare i corpi sugli endpoint non autenticati prima della lettura completa. In generale, imporre un tetto di dimensione corpo per tutti gli endpoint.
- **ADJUDICAZIONE DEL REVISORE**: Il confutatore ha verificato che l'OOM non avviene (realloc fallisce → CLIENT_MUST_STOP, buffer liberato); lo stallo è limitato dal timeout d'inattività. Resta un DoS di disponibilità limitato. Da grave a minore.
- **VOTO TECNICO** (reale=True): Ho letto riga per riga il codice reale e tutti i riferimenti del rilievo corrispondono. Il framework ESP8266WebServer (sincrono) legge l'INTERO corpo di una POST non-form in una String heap PRIMA di invocare l'handler, quindi prima di qualunque auth: in ESP8266WebServer-impl.h:341-346 handleClient() esegue _parseRequest() (case CLIENT_REQUEST_CAN_CONTINUE) e solo dopo _handleRequest(); dentro _parseRequest, Parsing-impl.h:151-152 fa 'contentLength = headerValue.toInt()' SENZA alcun tetto, e Pars…
- **VOTO IMPATTO** (reale=False, sev=minore): Meccanismo tecnicamente confermato ma senza conseguenza concreta per il proprietario nell'uso reale. Variante (a) OOM/reboot: smentita dalla stessa E-4 ("senza crash") e verificata sul codice — a Parsing-impl.h:160-169 plainBuf e' una String che cresce; se la realloc fallisce readBytesWithTimeout torna corta (StreamSend.cpp:127-142 ShortOperation), _parseRequest ritorna CLIENT_MUST_STOP (Parsing-impl.h:168), plainBuf viene liberata. Nessun altro allocatore gira (single-thread bloccato nel read),…

### C08 — Guasto I2C in marcia: relè MCP resta fisicamente chiuso mentre UI/MQTT/allarme dichiarano spento
- **SEVERITÀ**: minore  (rilievo originale: critico)
- **CONFIDENZA**: probabile
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: B  ·  rilievi grezzi: G1-B-1
- **ARCHETIPO (§4)**: nuovo
- **FILE**: src/hal/OutputBus.h:32-38 ; src/hal/Mcp23008.h:33-39 ; src/main.cpp:233-234 ; src/svc/WebApi.cpp:264-265 ; src/svc/WebApi.cpp:291
- **CODICE**:
```
OutputBus.h:32-37: void applyAutomation(bool heat, bool cool) { if (testMode_) return; if (heatRel_) writeRel(heatRel_, heat); if (coolRel_) writeRel(coolRel_, cool); heat_ = heat && heatRel_; cool_ = cool && coolRel_; }
Mcp23008.h:35-38: if (!recover() || !write8(REG_OLAT, v)) { present_ = false; return false; }
main.cpp:233-234: st.heat = lastPipe.heat; st.cool = lastPipe.cool;
WebApi.cpp:291: if (st.alarmOutputs) alarms.add("driver uscite (MCP23008) in errore: uscite disabilitate");
```
- **COSA ACCADE**: Stato iniziale: sistema armato, caldo=REL5 (o freddo=REL4) su MCP23008, carico eccitato (scrittura OLAT riuscita). Sequenza: il bus I2C condiviso con OLED e RTC si blocca (SDA/SCL tenuti bassi da uno slave); la scrittura di spegnimento fallisce → present_=false, il relè resta chiuso perché l'OLAT del MCP conserva il bit finché il chip è alimentato. Al tick successivo outputsFault→failsafe: la pipeline decide tutto spento, ma il relè non si apre; applyAutomation azzera comunque heat_/cool_ e buildStatus pubblica lastPipe.heat=false. Conseguenza osservabile: cavo riscaldante a 230 V (o compressore) alimentato senza controllo, mentre /api/status, MQTT/HA e OLED mostrano heat=false e l'allarme dice "uscite disabilitate" — affermazione falsa. Il retry di OutputBus::loop (5 s) risana solo se il bus si libera: il core ESP8266 sa clockare fuori uno slave che tiene SDA (core_esp8266_si2c.cpp:441-447) ma per SCL tenuto basso dichiara "no procedure available to recover"; un guasto duro lascia il relè chiuso a tempo indefinito su un dispositivo non sorvegliato. Lo stesso stato sopravvive a un riavvio ESP con bus incastrato: mcp.begin fallisce (main.cpp:532) e l'OLAT resta com'era.
- **PERCHÉ ORA**: Basta mappare uno dei due carichi su REL4-8 (opzione prevista dalla UI). Il bus I2C è condiviso con un OLED riscritto ~1 volta/s e con l'RTC: un wedge I2C è un guasto noto e la finestra di esposizione dura quanto il guasto. Nessuna lettura dello stato fisico è mai esposta: states_[]/heatActive()/coolActive() non hanno alcun lettore (verificato con grep su WebApi/main/OledUi/MqttSvc).
- **CORREZIONE**: Separare nella telemetria la decisione della pipeline dallo stato fisico noto (states_ + esito ultima scrittura) e, quando una scrittura di SPEGNIMENTO su relè mappato fallisce, alzare un allarme specifico "possibile relè bloccato chiuso" invece di "uscite disabilitate". In aggiunta: tentare un bus-clear esplicito (clock pulsing) in Mcp23008::recover. Il limite residuo (nessun attuatore di sgancio indipendente dal bus) va dichiarato nella documentazione: via software non è eliminabile, solo reso visibile.
- **ADJUDICAZIONE DEL REVISORE**: REALE nel firmware generico ma INERTE sull'esemplare: ANALISI.md:188 — MCP23008 non montato, carichi su REL1/REL3 (GPIO). Da critico a minore (rilevante solo per build con MCP).
- **VOTO TECNICO** (reale=True): Ho letto riga per riga i file citati e la libreria I2C del framework; tutti i riferimenti corrispondono al codice reale e non ho trovato alcuna guardia a monte che impedisca il difetto.

1) Relè fisicamente bloccato chiuso su fallimento di scrittura. Mcp23008.h:33-40: `if (!write8(REG_OLAT, v)) { present_=false; if (!recover() || !write8(REG_OLAT, v)) { present_=false; return false; } } olat_ = v;`. Su bus incastrato persistente sia la write sia recover() (=begin()=altra write OLAT) falliscono:…
- **VOTO IMPATTO** (reale=False, sev=minore): La catena in codice è descritta correttamente (OLAT del MCP conserva il bit su write fallita; applyAutomation azzera comunque heat_/cool_ a OutputBus.h:36-37; buildStatus pubblica lastPipe.heat=false a main.cpp:233-234; l'allarme dice "uscite disabilitate" a WebApi.cpp:291), MA il presupposto fisico non esiste sull'esemplare del proprietario. Verifiche al banco 15/08/2026 in docs/ANALISI.md: riga 188 (A3) "MCP23008... sull'esemplare reale il chip non è montato. REL4-8 non disponibili; i carichi…

### C09 — Reset silenzioso del MCP23008 (IODIR→input): scritture che fingono successo, uscite morte senza allarme
- **SEVERITÀ**: minore  (rilievo originale: critico)
- **CONFIDENZA**: da verificare sul campo
- **ESITO CONFUTAZIONE**: confermato  ·  trovato dagli assi: B  ·  rilievi grezzi: G1-B-2
- **ARCHETIPO (§4)**: 16
- **FILE**: src/hal/Mcp23008.h:16 ; src/hal/Mcp23008.h:32 ; src/hal/OutputBus.h:62-68
- **CODICE**:
```
Mcp23008.h:16: present_ = write8(REG_OLAT, 0x00) && write8(REG_IODIR, 0xE0);  // GP0..4 out, GP5..7 in  [IODIR scritto SOLO qui]
Mcp23008.h:32: if (v == olat_) return true;  [dedup: nessuna transazione se lo specchio coincide]
OutputBus.h:62: if (mcp_ && !mcp_->present() && millis() - lastMcpRetryMs_ > 5000) {  [retry solo dopo un NACK]
```
- **COSA ACCADE**: Stato iniziale: armato, freddo=REL4, compressore che cicla normalmente. Sequenza: un transitorio sul pin RESET del MCP23008 o un avvallamento della sua alimentazione (che l'ESP supera) resetta il chip: IODIR torna 0xFF (tutti ingressi), OLAT=0, il relè si apre. Da quel momento ogni setPin va a buon fine dal punto di vista del driver: il chip ACKa le scritture di OLAT (present_ resta true) ma i GP restano ingressi e i relè non si eccitano mai più; quando lo stato richiesto non cambia la scrittura è addirittura saltata dal dedup v==olat_. Nessun percorso rilegge IODIR/OLAT (la classe non ha una read) e il retry di OutputBus::loop parte solo con present_==false: il guasto è permanente e invisibile. Conseguenza osservabile: /api/status continua a mostrare cool=true nei periodi di richiesta, nessun allarme outputsFault, e la fermentazione deriva verso l'ambiente per giorni — la cotta si rovina in silenzio (o, sul caldo, non viene mai scaldata). Solo un riavvio dell'ESP (mcp.begin) ripristina le uscite.
- **PERCHÉ ORA**: Richiede il mapping su REL4-8 e un reset del solo MCP (EMC sul rail o sul RESET, brown-out parziale): evento raro ma il dispositivo resta acceso per mesi accanto a compressore e relè da 230 V, sorgenti classiche di transitori. La rubrica lo rende critico a prescindere dalla probabilità: è un guasto di classe 2 reso invisibile.
- **CORREZIONE**: Verifica periodica (es. ogni pochi secondi o a ogni transizione) di IODIR e OLAT via lettura I2C: se non corrispondono allo specchio, dichiarare fault e rieseguire begin(); in alternativa riscrivere IODIR+OLAT a cadenza fissa. Il dedup v==olat_ può restare solo se accompagnato dalla verifica.
- **ESPERIMENTO**: A sistema armato con freddo=REL4 e richiesta di raffreddamento attiva, portare brevemente a massa il RESET del MCP23008 (o togliere/ridare alimentazione al solo chip): verificare che il relè si apra, che /api/status continui a mostrare cool=true senza allarme "driver uscite", e che solo il riavvio dell'ESP ripristini l'attuazione.
- **ADJUDICAZIONE DEL REVISORE**: Come C08: reset silenzioso del MCP è inerte senza MCP montato (ANALISI.md:188). Confermato dai confutatori ma va riconciliato col fatto hardware. Da critico a minore.

### C19 — skip() durante l'attesa di partenza ritardata non azzera delayedStartEpoch: profilo 'completato' risorge / step saltato riparte
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: C,H  ·  rilievi grezzi: G1-C-7,G1-H-3
- **ARCHETIPO (§4)**: nuovo
- **FILE**: lib/axcore/src/ax/ProfileEngine.cpp:68-82 ; lib/axcore/src/ax/ProfileEngine.cpp:220-228 ; lib/axcore/src/ax/ProfileEngine.cpp:101-112
- **CODICE**:
```
ProfileEngine.cpp:70-74: int8_t next = nextEnabled((int8_t)(s_.stepIdx + 1));
  if (next < 0) {
    s_.completed = true;  // ultimo step saltato: profilo completo, setpoint mantenuto
    return true;
  }
--- ProfileEngine.cpp:220-227: if (s_.delayedStartEpoch != 0) {
    if (s_.paused || sdiff(in.nowEpoch, s_.delayedStartEpoch) < 0) {
      out.waitingDelayedStart = true;
      return out;
    }
    s_.delayedStartEpoch = 0;
    enterStep(s_.stepIdx, in.nowEpoch, in.tempC, in.tempValid, steps_[s_.stepIdx].targetC);
--- ProfileEngine.cpp:108: s_.completed = false;  // enterStep azzera completed
```
- **COSA ACCADE**: Stato iniziale: profilo con un solo step abilitato (es. sosta 18 °C per 14 giorni) avviato con partenza ritardata per domani alle 8:00; uscite ferme in attesa. Sequenza: l'utente ci ripensa e preme 'Salta step' (o un'automazione manda profile/skip): skip() trova next<0, marca completed=true — la UI mostra profilo completato — ma delayedStartEpoch resta impostato e lo stato viene persistito. Domattina alle 8:00 il tick entra nel ramo della partenza ritardata, azzera delayedStartEpoch e chiama enterStep sullo stesso stepIdx: enterStep azzera completed e lo step 'saltato' parte davvero. Conseguenza osservabile: il frigo/riscaldatore si mette in marcia su un programma che l'utente credeva chiuso il giorno prima, e lo stato torna 'step 1/1 in corso'. Con più step il salto durante l'attesa produce l'effetto meno sorprendente di cambiare lo step di partenza, ma sempre senza cancellare né anticipare la partenza ritardata.
- **PERCHÉ ORA**: Nessun comando tranne stop() cancella delayedStartEpoch; skip è esposto senza guardie da HTTP, MQTT e (in futuro) UI: la macchina a stati accetta la transizione ma ne lascia in vita un pezzo.
- **CORREZIONE**: In skip(), se delayedStartEpoch != 0 rifiutare il comando (la partenza non è ancora avvenuta: 409 'annulla prima la partenza ritardata', come già fa il cold crash) oppure azzerare delayedStartEpoch quando il salto porta a completed. La prima strada è più coerente con handleColdCrash che già tratta questo stato come esclusivo.
- **ADJUDICAZIONE DEL REVISORE**: skip() in attesa partenza ritardata non azzera delayedStartEpoch → profilo 'completato' risorge / step saltato riparte. Reale, minore (stato incoerente, recuperabile).
- **VOTO TECNICO** (reale=True): Ho letto riga per riga e confermo tutti i riferimenti. skip() (ProfileEngine.cpp:68-82, verbatim) ha come unica guardia `if (s_.mode != RunMode::Profile || s_.completed) return false;` (riga 69): durante l'attesa mode=Profile e completed=false, quindi procede; non tocca mai delayedStartEpoch. Il ramo di partenza ritardata in tick() (ProfileEngine.cpp:220-228, verbatim) e' valutato PRIMA del controllo su completed (riga 234): a now>=D azzera delayedStartEpoch e chiama enterStep(s_.stepIdx), che a…
- **VOTO IMPATTO** (reale=False, sev=invariata): Catena tecnica verificata e reale: skip() non tocca delayedStartEpoch (ProfileEngine.cpp:68-82): con un solo step abilitato pone s_.completed=true (riga 72), con piu' step avanza stepIdx via enterStep (riga 76). Il ramo partenza ritardata del tick (ProfileEngine.cpp:220-227) e' valutato PRIMA del blocco completed/paused (riga 234), quindi finche' delayedStartEpoch!=0 ritorna sempre waitingDelayedStart; allo scadere di D azzera delayedStartEpoch e chiama enterStep(stepIdx), che riazzera s_.comple…

### C20 — POST /api/setpoint (e climate_temp) a profilo completato/Off risponde 200 ok ma non fa nulla (override rifiutato in silenzio)
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: C  ·  rilievi grezzi: G1-C-8
- **ARCHETIPO (§4)**: 16
- **FILE**: lib/axcore/src/ax/ProfileEngine.cpp:90-97 ; src/svc/WebApi.cpp:657-672
- **CODICE**:
```
ProfileEngine.cpp:90-96: void ProfileEngine::overrideSetpoint(float setpointC) {
  if (s_.mode == RunMode::Profile && !s_.completed) {
    s_.overrideActive = true;
    s_.overrideSetpoint = setpointC;
  } else if (s_.mode == RunMode::Thermostat) {
    s_.thermostatSetpoint = setpointC;
  }
}  // Off e Profile+completed: nessun effetto
--- WebApi.cpp:669-671: r_.engine->overrideSetpoint(v);
  r_.persistNow();
  sendOk();  // nessun controllo di modalità: ok anche quando non è successo niente
```
- **COSA ACCADE**: Stato iniziale: cold crash completato, il motore mantiene 2 °C (comportamento documentato: a profilo completato resta l'ultimo setpoint). Sequenza: l'utente, magari da remoto via HA o con curl, alza il setpoint a 18 °C per il condizionamento: POST /api/setpoint {"value":18} → overrideSetpoint entra nel ramo Profile ma completed è true → nessun effetto; l'handler risponde comunque 200 {"ok":true}. Conseguenza osservabile: chi si fida della risposta crede la birra in risalita; resta a 2 °C finché non guarda lo stato. Lo stesso avviene a modalità Off e dal comando MQTT climate_temp (che oltretutto non ha risposta). È la forma dell''errore che finge successo': il comando accettato non è il comando eseguito.
- **PERCHÉ ORA**: Il percorso 'profilo completato che mantiene il setpoint' è lo stato normale di fine cotta, e ritoccare la temperatura in quel momento (condizionamento, diacetyl rest tardivo) è un'operazione plausibile; API e MQTT non gating-ano il comando per modalità.
- **CORREZIONE**: far tornare a overrideSetpoint un bool ed esporlo: 409 con messaggio ('profilo completato: usa il termostato o avvia un nuovo profilo') quando il comando non può avere effetto; in MQTT, ripubblicare subito lo stato reale già avviene ma non basta a segnalare il rifiuto — valutare un topic di errore o l'accettazione del comando come passaggio a termostato.
- **ADJUDICAZIONE DEL REVISORE**: setpoint/climate_temp su completed/Off risponde 200 ma no-op. Direzione sicura (resta freddo). Minore (falso ack).
- **VOTO TECNICO** (reale=True): Riferimenti verificati riga per riga, tutti corretti. ProfileEngine.cpp:90-97: overrideSetpoint() agisce solo se (mode==Profile && !completed) oppure mode==Thermostat; a Profile+completed e a Off nessun ramo viene eseguito, quindi il valore viene scartato (il corpo dell'if non gira, s_.overrideSetpoint non viene nemmeno scritto). WebApi.cpp:657-671 (handleSetpoint, rotta registrata a WebApi.cpp:87): auth -> parse -> range check (-10..40) -> overrideSetpoint(v) -> persistNow() -> sendOk(), SENZA…
- **VOTO IMPATTO** (reale=False, sev=minore): Difetto reale di semantica API/UX (override accettato ma non eseguito), ma senza conseguenza d'impatto concreta per il proprietario. Catena verificata: overrideSetpoint() è no-op a Profile+completed/Off (ProfileEngine.cpp:90-97) mentre l'handler risponde comunque 200 ok (WebApi.cpp:669-671) e il ramo MQTT non dà ack (main.cpp:632-637). (1) Direzione sicura del guasto: il comando è un no-op, quindi il dispositivo continua a mantenere l'ultimo setpoint di fine cotta (2 C) in RAFFREDDAMENTO (Profil…

### C25 — factoryResetPending protegge solo il persist pre-riavvio: nella finestra il dispositivo 'resettato' rinasce armato
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: D,F  ·  rilievi grezzi: G1-D-5,G1-F-5
- **ARCHETIPO (§4)**: 5
- **FILE**: src/main.cpp:204-216 ; src/main.cpp:344-349 ; src/main.cpp:745-747 ; altri persist non protetti nella finestra: src/main.cpp:630-657 (handler MQTT), src/main.cpp:188-191 (onIspindel)
- **CODICE**:
```
main.cpp:214-215:
  factoryResetPending = true;  // il persist pre-riavvio NON deve ricreare lo stato
  rebootAtMs = millis() + 1000;

main.cpp:344-346 (controlTick, gira ancora nella finestra di 1 s):
  if (lastTick.stepChanged || lastTick.profileCompleted || lastPipe.cool != prevCool) {
    prevCool = lastPipe.cool;
    persistNow();

main.cpp:745-747 (l'unico punto protetto):
  if (rebootAtMs && (int32_t)(nowMs - rebootAtMs) > 0) {
    if (!factoryResetPending) persistNow();
    ESP.restart();
```
- **COSA ACCADE**: Stato iniziale: profilo in corso, sistema armato, compressore che sta per commutare. Sequenza: POST /api/factory_reset -> i file vengono rimossi, rebootAtMs = +1000 ms, ma il loop continua: entro quel secondo controlTick gira almeno una volta e, se il relè freddo commuta (o uno step cambia, o arriva un comando MQTT, che chiamano persistNow), /state.json viene ricreato con armed=true, mapping relè, nome cotta, OG e snapshot del motore. Conseguenza osservabile: dopo il 'reset di fabbrica' il dispositivo riparte con configurazione di default ma stato precedente: risulta ARMATO, con la cotta e l'OG dell'utente precedente ancora su flash — un reset che non resetta, scoperto solo guardando /api/status. (La dedup di StateStore non salva: lastSeenEpoch cambia a ogni chiamata, quindi il JSON è sempre diverso e la scrittura avviene.)
- **PERCHÉ ORA**: La guardia esiste (factoryResetPending) ma copre una sola delle sorgenti di persistNow attive nella finestra: stessa forma dell'archetipo 5, invariante applicata in un punto solo. La probabilità per singolo reset è bassa (serve una transizione nel secondo di finestra) ma la conseguenza è confusa e persistente.
- **CORREZIONE**: Far valere factoryResetPending dentro persistNow stessa (if (factoryResetPending) return;), oppure fermare motore e disarmare prima di cancellare i file. Valutare nella stessa sede se /log/* e /boot.json debbano sopravvivere a un reset 'di fabbrica' (oggi restano: main.cpp:204-213).
- **ADJUDICAZIONE DEL REVISORE**: Finestra factory-reset ricrea state.json/devices.json → dispositivo 'resettato' rinasce armato. Reale, minore (finestra ~1-2s, raro).
- **VOTO TECNICO** (reale=True): Ho cercato la ragione per cui il difetto NON puo' accadere e non l'ho trovata: ogni presidio che avrebbe potuto salvare il rilievo e' assente. Riferimenti verificati riga per riga.

1) Tutti i file:riga del rilievo corrispondono al codice reale. factoryReset (main.cpp:204-216): rimuove config/state/profile/calib/devices (206-213), poi `factoryResetPending = true` (214) e `rebootAtMs = millis() + 1000` (215). NON disarma, NON ferma il motore, NON tocca lo stato in RAM ne' `lastSaved_`.

2) L'hand…
- **VOTO IMPATTO** (reale=False, sev=invariata): Il meccanismo tecnico esiste (lo confermerà il confutatore tecnico): la guardia factoryResetPending copre solo il persist di riga src/main.cpp:746, mentre il checkpoint 5-min (src/main.cpp:732-734), controlTick (src/main.cpp:344-346), l'handler MQTT (src/main.cpp:636-652) e onIspindel (src/main.cpp:190) chiamano persistNow() senza guardia, e la dedup di StateStore non blocca perche persistNow rinfresca lastSeenEpoch (src/main.cpp:76 -> StateStore.cpp:67,76). Quindi /state.json puo davvero rinasc…

### C26 — Boot con orologio inaffidabile: profileStartEpoch non ri-basato ma traslato da shiftClock → inizio profilo nel futuro (~2081)
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: D  ·  rilievi grezzi: G1-D-6
- **ARCHETIPO (§4)**: nuovo
- **FILE**: src/main.cpp:595-609 ; lib/axcore/src/ax/ProfileEngine.h:63-73 ; src/svc/MqttSvc.cpp:243-244 ; src/svc/WebApi.cpp:251
- **CODICE**:
```
main.cpp:600-608 (ri-base: manca profileStartEpoch):
    if (pstate.engine.mode == ax::RunMode::Profile) {
      ax::EngineSnapshot snap = engine.snapshot();
      snap.stepStartEpoch = timeSvc.nowEpoch();
      if (snap.paused) snap.pausedAtEpoch = timeSvc.nowEpoch();
      snap.stableSinceEpoch = 0;
      snap.stableRefGravity = 0;
      engine.restore(snap);
    }

ProfileEngine.h:69-72 (la traslazione invece lo include):
    shift(s_.stepStartEpoch);
    shift(s_.pausedAtEpoch);
    shift(s_.profileStartEpoch);
    shift(s_.stableSinceEpoch);

MqttSvc.cpp:243-244:
  if (st.mode == 2 && st.profileStartEpoch > 0 && st.nowEpoch > st.profileStartEpoch)
    doc["days"] = serialized(String((st.nowEpoch - st.profileStartEpoch) / 86400.0f, 1));
```
- **COSA ACCADE**: Stato iniziale: profilo avviato con orologio affidabile (profileStartEpoch reale, es. 1.756e9), poi blackout e batteria RTC esaurita. Sequenza: al boot trusted=false -> timeline fittizia (base~100); il blocco di main.cpp:600-608 ri-basa stepStartEpoch/pausedAtEpoch sull'epoca fittizia ma lascia profileStartEpoch al valore reale. Arriva il WiFi, NTP sincronizza: clockJump ~= +1.756e9 e shiftClock lo somma a TUTTI gli àncora, incluso profileStartEpoch, che diventa ~3.5e9 (anno ~2081). Conseguenza osservabile: il campo 'days' sparisce da MQTT/Home Assistant (guardia nowEpoch > profileStartEpoch falsa) e /api/status espone startEpoch nel futuro; il contatore dei giorni di fermentazione — quello su cui si decidono dry-hop e cold crash — è perso o assurdo per tutto il resto della cotta, e viene persistito così.
- **PERCHÉ ORA**: Basta la combinazione blackout + RTC scarico + NTP, che è lo scenario nominale previsto da §0 ('orologio non attendibile che può saltare quando si sincronizza'). La ri-base è una compensazione applicata a metà: corregge tre àncora su quattro, e la traslazione globale successiva sovra-corregge il quarto.
- **CORREZIONE**: Nel blocco di ri-base trattare profileStartEpoch come gli altri àncora (ri-basarlo conservando il delta con stepStartEpoch, o azzerarlo dichiarando 'inizio non noto'); in alternativa marcare gli àncora ri-basati perché shiftClock non li trasli due volte. Va scelta e documentata una sola semantica.
- **ADJUDICAZIONE DEL REVISORE**: Boot orologio inaffidabile: profileStartEpoch traslato → inizio nel futuro ~2081, giorni di fermentazione spariscono da MQTT/UI. Cosmetico. Minore.
- **VOTO TECNICO** (reale=True): Catena tracciata riga per riga e confermata. (1) Il blocco di ri-base src/main.cpp:600-608 imposta snap.stepStartEpoch=timeSvc.nowEpoch(), eventualmente pausedAtEpoch, e azzera stableSinceEpoch/stableRefGravity, ma NON tocca profileStartEpoch (verificato: nessuna delle assegnazioni lo riguarda). (2) profileStartEpoch arriva reale al blocco: StateStore.cpp:21 lo carica, engine.restore(pstate.engine) a main.cpp:584 lo copia (ProfileEngine.cpp:99 restore() = s_=snap, nessun clamp); allo scattare de…
- **VOTO IMPATTO** (reale=False, sev=invariata): La catena tecnica è reale e raggiungibile con lo scenario nominale (blackout + RTC scarico + NTP): il blocco di boot ri-basa stepStartEpoch/pausedAtEpoch/stableSinceEpoch ma NON profileStartEpoch (src/main.cpp:600-608), che resta al valore reale R; poi shiftClock lo trasla comunque insieme agli altri (lib/axcore/src/ax/ProfileEngine.h:71) sommando clockJump ≈ R' derivato in TimeService.cpp:51-53, portandolo a R+R' ≈ 3.5e9 (anno ~2081) e persistendolo (src/main.cpp:714). Conseguenze osservabili v…

### C27 — ConfigStore non controlla mai l'overflow del documento 4096B: config cresciuta perde in silenzio le sezioni finali
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: probabile
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: D  ·  rilievi grezzi: G1-D-7
- **ARCHETIPO (§4)**: 13
- **FILE**: src/svc/ConfigStore.cpp:9-10 ; src/svc/ConfigStore.cpp:18-27 ; src/svc/ConfigStore.cpp:29-102 ; assenza di limiti di lunghezza in fromJson per hostname/ssid/password/ntpServer/tz/mqtt* (righe 111-208, limitati solo probeLabel:147-152 e gravityDevice:179)
- **CODICE**:
```
ConfigStore.cpp:19-25 (save):
  DynamicJsonDocument doc(4096);
  toJson(cfg, doc, false);
  File f = LittleFS.open("/config.tmp", "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
(nessun doc.overflowed(): se il pool da 4096 si esaurisce, createNestedObject/assegnazioni falliscono in silenzio e il JSON serializzato è semplicemente privo delle sezioni finali)

ConfigStore.cpp:9-12 (load, stessa capacità ma consumo maggiore: in deserializzazione anche le chiavi vengono copiate nel pool):
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, f);
  ...
  if (err) return false;
```
- **COSA ACCADE**: Stato iniziale: campi stringa lunghi accumulati su PUT successivi (i segreti si conservano con '***', quindi ogni PUT può aggiungere stringhe senza mai rivederle tutte insieme: es. mqttPass lunga + ispindelToken lungo + tz/ntpServer/ssid). Sequenza: al PUT successivo toJson esaurisce il pool da 4096: le sezioni inserite per ultime (mqtt, ota) non entrano nel documento; il save scrive un JSON valido ma monco, risponde 200. Conseguenza osservabile: al riavvio la config si carica 'con successo' ma MQTT risulta non configurato e otaEnabled torna al default — dati spariti senza errore. Nel caso ancora più sfavorevole (file vicino ai 4 KB) è la load al boot a fallire per NoMemory, perché la deserializzazione copia anche le chiavi nel pool: config che si salva ma non si ricarica -> default + valid=false -> uscite disabilitate.
- **PERCHÉ ORA**: Nessuno dei campi di rete/MQTT ha un limite di lunghezza in fromJson, quindi la dimensione totale non è vincolata da nessuna parte; il troncamento è muto perché ArduinoJson non segnala l'esaurimento del pool se non lo si interroga con overflowed().
- **CORREZIONE**: Controllare doc.overflowed() dopo toJson (e fallire il save), e mettere limiti di lunghezza espliciti ai campi stringa in fromJson come già fatto per probeLabel; dimensionare la capacità di load sopra quella di save per l'asimmetria chiavi-copiate.
- **ADJUDICAZIONE DEL REVISORE**: ConfigStore no overflow check 4096 → sezioni finali (mqtt/ota) perse in silenzio su config cresciuta. Minore (archetipo 13).
- **VOTO TECNICO** (reale=True): Tutti i riferimenti file:riga del rilievo corrispondono al codice reale e la meccanica e confermata dalla libreria; non ho trovato alcuna guardia a monte che lo impedisca.

1) save() senza controllo overflow (ConfigStore.cpp:18-27): "DynamicJsonDocument doc(4096); toJson(cfg, doc, false); ... size_t n = serializeJson(doc, f); ... if (n == 0) return false;". Nessuna chiamata a doc.overflowed(). Verificato in libreria: MemoryPool.hpp:193-202 (allocString) e 209-216 (allocRight) su pool esaurito es…
- **VOTO IMPATTO** (reale=False, sev=minore): Il difetto di codice esiste (ConfigStore.cpp:19-25 non interroga mai doc.overflowed() dopo toJson; fromJson righe 111-208 non limita i campi rete/MQTT, a differenza di probeLabel:147-152 e gravityDevice:179), ma la sua attivazione dipende solo dalla dimensione totale delle stringhe. Ho misurato con la libreria reale (ArduinoJson 6.21.6, .pio/libdeps/d1_mini): un worst-case realistico GENEROSO (uiPassword 30, ssid 32, wifiPassword 63 = massimo WPA2, IP completi, ntpServer 21, tz 26, 3x probeId 16…

### C30 — JSON profilo 6144B (~1/3 heap): se l'alloc fallisce per frammentazione il profilo è trattato ASSENTE (elenco monco→sovrascrittura)
- **SEVERITÀ**: minore  (rilievo originale: grave)
- **CONFIDENZA**: da verificare sul campo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: E  ·  rilievi grezzi: G1-E-5
- **ARCHETIPO (§4)**: 16
- **FILE**: src/svc/ProfileStore.cpp:76-87 ; src/svc/ProfileStore.cpp:127-152 ; src/svc/WebApi.cpp:355,388,406,444
- **CODICE**:
```
readFile(): DynamicJsonDocument doc(6144); DeserializationError err = deserializeJson(doc, f); f.close(); if (err) return false;  // ProfileStore.cpp:80-83 (NoMemory => err => false)
list(): for (id=1..kMaxProfiles) { if (!readFile(libPath(id),&name,steps,&n)) continue; ... }  // ProfileStore.cpp:131-132 (fallimento lettura = profilo saltato)
handleProfilesList()->list();  handleGetProfile()/handleProfileById(): DynamicJsonDocument doc(6144);  // WebApi.cpp:355,388,406,444
```
- **COSA ACCADE**: Stato iniziale: device acceso da settimane, heap ~19 KB con frammentazione ~14% (il piu' grande blocco libero puo' scendere sotto 6144 B sotto carico: MQTT connesso + risposta in costruzione). Sequenza: l'utente apre l'elenco profili (GET /api/profiles). list() alloca un DynamicJsonDocument(6144) per OGNI profilo (fino a 20 volte in ciclo). Se per uno o piu' profili il blocco da 6144 B non e' disponibile, DynamicJsonDocument nasce a capacita' 0, deserializeJson ritorna NoMemory, readFile ritorna false e list() fa 'continue': quel profilo esistente SPARISCE dall'elenco, senza errore. Conseguenza osservabile: la UI mostra uno slot 'vuoto' per un profilo che esiste su flash; se l'utente salva un nuovo profilo in quello slot, PUT /api/profiles/{id} sovrascrive p{id}.json esistente -> perdita di un profilo non ricostruibile. Lo stesso fallimento silenzioso su handleGetProfile restituirebbe un profilo 'senza step' per una cotta in corso.
- **PERCHÉ ORA**: Raggiungibile a heap frammentato dopo lungo uptime (lo scenario che l'asse E impone: 'caso peggiore con 19 KB liberi'). L'allocazione da 6144 B e' la piu' grande e la piu' sensibile alla frammentazione, e viene ripetuta 20 volte in un solo handler.
- **CORREZIONE**: Distinguere 'file assente' da 'lettura fallita per memoria': readFile deve segnalare l'errore di allocazione/parse in modo diverso da 'non esiste', e list()/handleGetProfile devono propagarlo (es. marcare lo slot come 'errore, non sovrascrivibile' invece di 'vuoto'). Ridurre la pressione heap: un unico documento riusato per l'intero list() invece di 20 allocazioni, o capacita' dimensionata al caso reale con controllo di overflowed()/allocazione esplicito prima di procedere.
- **ESPERIMENTO**: Ridurre artificialmente l'heap libero (o frammentarlo) fino a sotto ~6 KB di blocco contiguo e chiamare GET /api/profiles con piu' profili salvati: verificare se alcuni scompaiono dall'elenco; poi confermare che un PUT sullo slot 'scomparso' sovrascrive il file preesistente.
- **ADJUDICAZIONE DEL REVISORE**: Tecnico confermato (readFile conflate assente/OOM = archetipo 16); impatto raro (alloc sequenziale, 6144<19KB). Il core reale è la conflazione false==assente. Grave→minore su probabilità.
- **VOTO TECNICO** (reale=True): Ho tentato di confutarlo e non ci sono riuscito: i riferimenti sono corretti e il meccanismo regge riga per riga, incluso il comportamento della libreria (verificato, non dedotto).

1) Riferimenti esatti. ProfileStore.cpp:80 `DynamicJsonDocument doc(6144);`, :81 `DeserializationError err = deserializeJson(doc, f);`, :83 `if (err) return false;`. Fondamentale: :79 `if (!f) return false;` (file assente) e :83 (parse/OOM) collassano entrambi nello STESSO `false` — il chiamante non puo' distinguere…
- **VOTO IMPATTO** (reale=False, sev=minore): Il MECCANISMO esiste ma il PERCORSO d'impatto non è plausibile e la conseguenza è ridimensionata. Confermato: readFile() (src/svc/ProfileStore.cpp:83) ritorna false sia per 'file assente' sia per NoMemory/parse-error, e list() (src/svc/ProfileStore.cpp:132) fa 'continue', quindi un profilo esistente può sparire dall'elenco senza errore; il front-end conferma la catena di sovrascrittura: freeLibId() (data-src/index.html:1155) sceglie il primo id 1..20 assente da LIB, e un PUT successivo sovrascri…

### C33 — GET /api/status mescola campi vivi del motore e cache vecchia fino a 1s: risposta internamente contraddittoria
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: F  ·  rilievi grezzi: G1-F-7
- **ARCHETIPO (§4)**: 17
- **FILE**: src/svc/WebApi.cpp:212-309 ; src/main.cpp:723-727 ; src/main.cpp:654-656
- **CODICE**:
```
WebApi.cpp:214: AppStatus st = r_.status();          // cache, ricostruita solo al controlTick (1 Hz)
WebApi.cpp:244: prof["running"] = r_.engine->running();   // VIVO
WebApi.cpp:252: prof["overrideActive"] = r_.engine->overrideActive();   // VIVO
WebApi.cpp:239: doc["mode"] = st.mode;                // CACHE
main.cpp:656: curStatus = buildStatus();   // solo il handler MQTT riallinea subito la cache; quelli HTTP no
```
- **COSA ACCADE**: Stato iniziale: profilo in marcia, due client sulla pagina. Sequenza: il client A invia POST /api/profile/stop (200 OK); il client B (o lo stesso A, che la UI fa ripartire subito) esegue GET /api/status prima del successivo controlTick: la risposta dichiara profile.running=false (letto vivo dal motore) ma mode=2, outputs.cool=true e setpoint presente (dalla cache curStatus non ricostruita dai handler HTTP, a differenza di quello MQTT che chiama buildStatus a riga 656). Un client che reagisce allo stato (dashboard, script) legge una fotografia impossibile — profilo fermo con uscite di profilo attive — e può loggare o decidere sul falso; a UI umana l'effetto è un flicker incoerente di ≤1 s dopo ogni comando.
- **PERCHÉ ORA**: La coppia handler-HTTP/cache è asimmetrica rispetto a quella MQTT per costruzione; ogni comando HTTP apre la finestra, quindi accade a ogni uso, anche se la conseguenza resta transitoria.
- **CORREZIONE**: O ricostruire curStatus al termine di ogni handler HTTP che muta il motore (come fa il handler MQTT), o servire /api/status interamente dalla cache senza mescolare letture vive: l'importante è che la fotografia sia di UN solo istante.
- **ADJUDICAZIONE DEL REVISORE**: GET /api/status mescola vivo e cache 1s → risposta contraddittoria dopo un comando. Minore.
- **VOTO TECNICO** (reale=True): Riferimenti tutti verificati riga per riga. WebApi.cpp:214 `AppStatus st = r_.status();` legge la cache statica curStatus (main.cpp:675 `refs.status = []() { return curStatus; };`). I campi cache: WebApi.cpp:239 `doc["mode"] = st.mode;`, 240 setpoint da st, 246 `prof["completed"] = st.completed`, 264-265 `out["heat"/"cool"] = st.heat/st.cool`. I campi vivi: WebApi.cpp:244 `prof["running"] = r_.engine->running();`, 252 `overrideActive()`, 241 `thermostatSetpoint()`, 267-273 coolRel/heatRel/testMo…
- **VOTO IMPATTO** (reale=False, sev=minore): L'incoerenza tecnica esiste davvero: `handleStatus` mescola cache e letture vive. `WebApi.cpp:214` legge `curStatus` (cache, ricostruita solo al controlTick), da cui derivano `mode` (:239), `setpoint` (:240), `heat`/`cool` (:264-265), mentre `running()` (:244) e `overrideActive()` (:252) sono letti vivi dal motore. Il handler HTTP di stop (`WebApi.cpp:641,653-654`) fa `engine->stop()`+`persistNow()`+`sendOk()` ma NON ricostruisce `curStatus`, a differenza del handler MQTT (`main.cpp:656`). Quind…

### C34 — rebootAtMs usa 0 come sentinella ma = millis()+ritardo: se traborda a 0 il riavvio programmato non avviene mai
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: F  ·  rilievi grezzi: G1-F-8
- **ARCHETIPO (§4)**: 9
- **FILE**: src/main.cpp:218 ; src/main.cpp:745-748 ; src/main.cpp:215 ; src/hal/ProbeBus.h:222-225
- **CODICE**:
```
main.cpp:218: static void scheduleReboot(uint32_t delayMs) { rebootAtMs = millis() + delayMs; }
main.cpp:745: if (rebootAtMs && (int32_t)(nowMs - rebootAtMs) > 0) {
ProbeBus.h:223-224 (stessa forma, difesa): graziaFinoMs_[idx] = nowMs + kGraziaMs;
  if (!graziaFinoMs_[idx]) graziaFinoMs_[idx] = 1;   // qui il caso è gestito
```
- **COSA ACCADE**: Stato iniziale: uptime prossimo al wrap di millis() (~49,7 giorni e multipli). Sequenza: una PUT /api/config con modifiche di rete risponde reboot:true e chiama scheduleReboot(1500); se millis()+1500 traborda esattamente a 0 (finestra di 1 ms su 2^32 per evento), rebootAtMs=0 viene letto dalla riga 745 come «nessun riavvio programmato»: la UI annuncia il riavvio, il dispositivo non riavvia mai, e le modifiche di rete/hostname/MQTT non vengono applicate finché qualcuno non riavvia per altra causa — su un dispositivo non sorvegliato può voler dire settimane con la config su flash diversa da quella in esercizio. Lo stesso vale per /api/reboot e per il ritardo di 1000 ms del factory reset (riga 215: reset eseguito sui file ma riavvio mai eseguito, dispositivo lasciato in marcia con i file cancellati sotto i piedi fino al prossimo spegnimento).
- **PERCHÉ ORA**: Il dispositivo è progettato per uptime superiori al periodo di wrap (§0) e la codebase conosce già questa trappola — ProbeBus la difende esplicitamente spostando la sentinella a 1 — ma qui la stessa forma è rimasta scoperta (invariante rispettata in un punto, violata in un altro).
- **CORREZIONE**: Stessa difesa già usata in apriGrazia: dopo il calcolo, se rebootAtMs==0 forzarlo a 1; oppure sostituire la sentinella con un flag booleano separato (reboot richiesto + istante), eliminando il doppio significato dello 0.
- **ADJUDICAZIONE DEL REVISORE**: rebootAtMs=0 sentinella collide al wrap millis (finestra 1ms/49,7gg). Reale, minore (archetipo 9).
- **VOTO TECNICO** (reale=True): Ho letto il codice reale e ogni riferimento file:riga combacia. main.cpp:64 dichiara `static uint32_t rebootAtMs = 0;`: lo 0 e' effettivamente la sentinella "nessun riavvio". main.cpp:218 `scheduleReboot(delayMs){ rebootAtMs = millis()+delayMs; }` e main.cpp:215 (factory reset) `rebootAtMs = millis()+1000;` assegnano senza alcuna guardia. main.cpp:745 `if (rebootAtMs && (int32_t)(nowMs - rebootAtMs) > 0)`: il termine `rebootAtMs &&` cortocircuita a "non programmato" quando il valore e' 0; il con…
- **VOTO IMPATTO** (reale=False, sev=minore): Il difetto meccanico esiste ed è verificato: src/main.cpp:64 usa 0 come sentinella (`static uint32_t rebootAtMs = 0;`), main.cpp:218 e main.cpp:215 calcolano `rebootAtMs = millis() + delayMs` senza la guardia che invece ProbeBus.h:223-224 applica (`if (!graziaFinoMs_[idx]) graziaFinoMs_[idx] = 1;`), e main.cpp:745 (`if (rebootAtMs && ...)`) interpreta 0 come "nessun riavvio". L'invariante è quindi davvero rispettata in un punto e violata in un altro. Ma il mio mandato è l'impatto concreto sul pr…

### C35 — Plato: i modelli di fabbrica convertono in °P anche le soglie di attenuazione → 'att. ≥ 253%', cancello che non si chiude mai
- **SEVERITÀ**: minore  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: G  ·  rilievi grezzi: G1-G-2
- **ARCHETIPO (§4)**: 12
- **FILE**: data-src/index.html:1163-1170 ; data-src/index.html:1186 ; data-src/index.html:1104 ; data-src/index.html:1609
- **CODICE**:
```
index.html:1166-1169: `if(isPlato()){ r.stableBand=Math.round(r.stableBand*SG2P*1000)/1000; if(r.gravityTarget>1.0)r.gravityTarget=sgToPlato(r.gravityTarget); }` — la conversione non controlla gravityMode. Esempio index.html:1186: `{type:"gravity",target:16,gravityMode:1,gravityTarget:50,maxDurationSec:3*DAY}` — 50 è una PERCENTUALE di attenuazione, ma sgToPlato(50)=259-259/50=253.82. Applicata sia in visione (1104: `mod.s.forEach((st,i)=>box.appendChild(stepRow(TS(st),i)))`) sia nella copia in libreria (1609: `await put("/api/profiles/"+id,{name:nome,steps:t.s.map(TS)})`).
```
- **COSA ACCADE**: Stato iniziale: `gravityUnit="plato"` in Impostazioni (opzione di prima classe, tendina 'Unità'). Sequenza: l'utente apre 'Ale pulita fresca (Nottingham)' e preme 'Usa modello': lo step 1 viene salvato in libreria con gravityTarget=253.82 al posto di 50; l'editor e il riepilogo mostrano 'att. ≥ 253.82%'. Avviata la cotta, l'attenuazione apparente non può superare ~100%, quindi il cancello 'sale a 20 °C a metà attenuazione' non scatta mai e lo step esce solo al timeout di sicurezza (3 giorni pieni a 16 °C invece di ~metà fermentazione). Conseguenza osservabile: il numero assurdo è visibile nel riepilogo del profilo, e i rialzi guidati dall'attenuazione (riposo diacetile, anti-stallo delle saison, gradini delle quad) arrivano sistematicamente in ritardo di giorni rispetto a quanto il modello promette nella descrizione. Colpisce i 30 modelli su 49 che usano gravityMode:1 (verificato con estrazione automatica su tutti i blocchi).
- **PERCHÉ ORA**: Raggiungibile appena si sceglie °Plato (unità standard per molti homebrewer europei) e si usa la libreria di fabbrica, che è il percorso consigliato dalla pagina ('Libreria vuota: si apre il primo modello'). Il commento stesso (1163-1165) dimostra che la conversione parziale era il rischio noto, ma la guardia `>1.0` distingue solo SG da °P, non 'densità' da 'percentuale'.
- **CORREZIONE**: Convertire gravityTarget solo quando lo step è gravityMode 0 ('densità ≤ soglia'): `if(r.gravityMode===0 && r.gravityTarget>1.0) r.gravityTarget=sgToPlato(...)`. Le percentuali (mode 1) e i parametri di mode 2 restano intatti. Aggiungere al riepilogo un sanity-check che segnali target di attenuazione >100%.
- **ADJUDICAZIONE DEL REVISORE**: Versione catastrofica REFUTATA: validateSteps rifiuta con 400 le soglie attenuazione >100%, niente corruzione silenziosa. Residuo reale: il pulsante 'Usa modello' non funziona per i modelli ad attenuazione in Plato (400). Grave→minore, riformulato.
- **VOTO TECNICO** (reale=False): Il meccanismo client-side esiste ma la conseguenza descritta (persistenza silenziosa + cancello mai chiuso + 3 giorni a temperatura sbagliata + 30 profili corrotti in libreria) è impedita da una guardia a monte nel firmware, quindi il difetto come formulato NON regge.

VERIFICATO come presente (parte tecnica): in data-src/index.html:1166-1168 la conversione `if(r.gravityTarget>1.0)r.gravityTarget=sgToPlato(r.gravityTarget)` non controlla gravityMode; con sgToPlato (index.html:825) sgToPlato(50)=…
- **VOTO IMPATTO** (reale=True, sev=invariata): Catena verificata riga per riga e deterministica sul percorso principale, non teorica.

1) La conversione scatta davvero con la sola scelta di °Plato in Impostazioni. isPlato() (data-src/index.html:821) legge ST.gravity.unit, che è alimentato dallo stato del firmware (src/svc/WebApi.cpp:277 `gr["unit"] = r_.cfg->gravityUnit`), a sua volta impostato dalla tendina di prima classe (index.html:364, salvata a :2046 come `unit:v("c_gunit")`). Configurato °Plato → isPlato()=true su ogni caricamento suc…

### C38 — Test manuale relè: OLED e MQTT dichiarano 'riposo/idle' mentre un relè è fisicamente eccitato (AppStatus non ha testMode)
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: G  ·  rilievi grezzi: G1-G-5
- **ARCHETIPO (§4)**: 17
- **FILE**: src/hal/OutputBus.h:32-38 ; src/svc/AppStatus.h:10 ; src/svc/OledUi.cpp:76-82 ; src/svc/MqttSvc.cpp:229-231 ; src/svc/MqttSvc.cpp:246-247
- **CODICE**:
```
OutputBus.h:32-33: `void applyAutomation(bool heat, bool cool) { if (testMode_) return; ... }`. AppStatus.h: nessun campo testMode (solo `bool heat = false, cool = false, armed = false;`). OledUi.cpp:77-81: `if (st.heat) out = "RISCALDA"; else if (st.cool) out = "RAFFREDDA"; ... else out = "riposo";`. MqttSvc.cpp:230-231: `doc["climate_action"] = st.heat ? "heating" : st.cool ? "cooling" : (st.mode == 0 ? "off" : "idle");` — heat/cool vengono dalla pipeline (a sistema disarmato: tutto false), non dallo stato fisico dei relè. La pagina web invece lo mostra (index.html:572 `if(st.outputs.testMode)o="TEST MANUALE"`).
```
- **COSA ACCADE**: Stato iniziale: sistema disarmato, utente nella scheda Sistema che identifica i relè (workflow documentato prima dell'armamento), carichi ancora collegati. Sequenza: preme 'REL2 ON'; il frigo parte. Il browser si blocca o l'utente si allontana: il relè resta eccitato fino al timeout di 10 minuti (OutputBus.h:61). Conseguenza osservabile: per tutto quel tempo l'OLED accanto al dispositivo dice 'riposo' e '[DISARMATO]', Home Assistant dice climate_action=idle e i binary Riscaldamento/Raffreddamento OFF: ogni display tranne la pagina web afferma il contrario dello stato fisico di un'uscita a 230 V.
- **PERCHÉ ORA**: Il test relè è un passaggio obbligato della messa in servizio e ripetuto a ogni cambio mapping; il timeout di 10 minuti limita ma non elimina la finestra di falsità.
- **CORREZIONE**: Aggiungere testMode (ed eventualmente lo stato fisico dei relè, già disponibile via relayState()) ad AppStatus; OLED mostra 'TEST MANUALE' nella riga uscite e MQTT pubblica un flag test_mode o riflette relayState nel climate_action.
- **ADJUDICAZIONE DEL REVISORE**: Test relè: OLED/MQTT dicono idle mentre un relè è eccitato (AppStatus senza testMode). Su REL1/REL3 reale. Minore.
- **VOTO TECNICO** (reale=True): Confutazione tentata e fallita: tutti i riferimenti file:riga sono corretti e il meccanismo è reale. Verificato: AppStatus.h:5-41 non contiene alcun campo testMode (riga 10: 'bool heat=false, cool=false, armed=false;'). In test mode applyAutomation è no-op (OutputBus.h:32-33) mentre testRelay() eccita fisicamente il relè via writeRel/digitalWrite (OutputBus.h:75-79,89-91). In buildStatus st.heat/st.cool provengono dalla pipeline (main.cpp:233-234), non dallo stato fisico; con sistema disarmato s…
- **VOTO IMPATTO** (reale=False, sev=minore): La discrepanza tecnica esiste ed è verificata: AppStatus.h:10 non ha un campo testMode; main.cpp:233-234 popola st.heat/st.cool da lastPipe (che a sistema disarmato è false); OledUi.cpp:77-82 mostra quindi "riposo" e OledUi.cpp:72 aggiunge "[DISARMATO]"; MqttSvc.cpp:230-231 pubblica climate_action idle/off con heat/cool=false; solo WebApi.cpp:273 e index.html:572 mostrano "TEST MANUALE". Ma NESSUNA conseguenza concreta segue, per quattro ragioni convergenti. (1) Il test relè esiste solo a sistem…

### C39 — Con caldo a isteresi la dashboard mostra 'PID 0%' mentre il riscaldatore è al 100%
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: G  ·  rilievi grezzi: G1-G-6
- **ARCHETIPO (§4)**: 17
- **FILE**: data-src/index.html:575 ; lib/axcore/src/ax/ControlPipeline.h:92-95 ; src/svc/AppConfig.h:33
- **CODICE**:
```
index.html:575: `if(st.outputs.heat&&ST.mode!==0)sub="PID "+st.outputs.pidDuty+" %";` — nessun controllo che il PID sia attivo. ControlPipeline.h:92-95: `if (cfg_.heatUsePid) { st.pidDuty = pid_.update(setpointC, tempC, dtSec); ... }` — con heatUsePid=false pidDuty resta 0.0f. AppConfig.h:33: `bool usePid = true;  // D9: PID sul caldo; false = isteresi anche sul caldo` (opzione esposta in Impostazioni: 'PID sul caldo (sennò isteresi)').
```
- **COSA ACCADE**: Stato iniziale: utente ha deselezionato 'PID sul caldo' (isteresi pura), inverno, cavo scaldante in marcia. Sequenza: la dashboard mostra la pill RISCALDA e sotto 'PID 0 %'. Conseguenza osservabile: il display afferma che il riscaldatore eroga lo 0% mentre è a piena potenza; un utente che diagnostica 'non scalda abbastanza' da quel numero prende decisioni sbagliate (ritocca kP/kI che non sono in uso, o crede il cavo guasto). /api/status conferma: outputs.heat=true, outputs.pidDuty=0, config usePid=false.
- **PERCHÉ ORA**: Basta la spunta di configurazione, offerta come alternativa di prima classe nella stessa pagina.
- **CORREZIONE**: Esportare in /api/status il fatto che il PID è attivo (o riusare CFG già caricata) e mostrare il sub 'PID N %' solo in quel caso; a isteresi mostrare niente o 'caldo ON'.
- **ADJUDICAZIONE DEL REVISORE**: 'PID 0%' mostrato con caldo a isteresi acceso. Numero fuorviante in diagnostica. Minore.
- **VOTO TECNICO** (reale=True): Non sono riuscito a confutare il rilievo: ogni riferimento file:riga corrisponde al codice reale e non esiste alcuna guardia a monte. index.html:575 recita esattamente `if(st.outputs.heat&&ST.mode!==0)sub="PID "+st.outputs.pidDuty+" %";` — la condizione controlla solo heat e mode!==0, mai se il PID sia l'algoritmo attivo sul caldo; la riga 576 (`$("outsub").textContent=sub;`) mostra il sub senza ulteriori override nel ramo caldo. In ControlPipeline.h Status.pidDuty è inizializzato a 0.0f (riga 3…
- **VOTO IMPATTO** (reale=False, sev=invariata): Il difetto tecnico esiste: con `usePid=false` (isteresi sul caldo) la dashboard scrive "PID 0 %". Verificato: `data-src/index.html:575` costruisce il sub senza guardare se il PID sia attivo; `ControlPipeline.h:92-93` chiama `pid_.update` solo sotto `if (cfg_.heatUsePid)`, quindi `st.pidDuty` resta il default `0.0f` (`ControlPipeline.h:31`), esportato tale quale in `WebApi.cpp:270`. Ma sul piano dell'IMPATTO la conseguenza per il proprietario non è concreta, per tre ragioni convergenti.

1) I seg…

### C40 — Doppia numerazione step: status/OLED/MQTT contano i disattivati, il runcard solo gli attivi
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: G  ·  rilievi grezzi: G1-G-7
- **ARCHETIPO (§4)**: 18
- **FILE**: src/svc/WebApi.cpp:247-248 ; src/svc/OledUi.cpp:64 ; src/svc/MqttSvc.cpp:223-224 ; data-src/index.html:582 ; data-src/index.html:963-965 ; data-src/index.html:1028
- **CODICE**:
```
WebApi.cpp:247-248: `prof["stepIdx"] = st.stepIdx; prof["stepCount"] = st.stepCount;` (indici sull'array completo: ProfileEngine::stepCount() ritorna count_, disabilitati inclusi). OledUi.cpp:64: `mode = "PROFILO " + String(st.stepIdx + 1) + "/" + String(st.stepCount);`. MqttSvc.cpp:224: `doc["step"] = String(st.stepIdx + 1) + "/" + String(st.stepCount);`. index.html:582: `mt+=" — step "+(st.profile.stepIdx+1)+"/"+st.profile.stepCount;` ma 964-965: `const idx=RUNPROF.steps.slice(0,P.stepIdx).filter(s=>s.enabled).length; const cur=en[idx];` e 1028: `'<b>Step '+(idx+1)+' di '+en.length+'</b>'`.
```
- **COSA ACCADE**: Stato iniziale: profilo di 8 step con 2 disattivati (l'editor ha la checkbox 'attivo' per step), in marcia sul 5° step assoluto (3° tra gli abilitati). Sequenza: l'utente guarda la pagina: la riga di modalità dice 'step 5/8', il riquadro subito sotto dice 'Step 3 di 6'; l'OLED e Home Assistant dicono '5/8'. Conseguenza osservabile: due conteggi diversi per lo stesso avanzamento nella stessa schermata; chi segue la cotta da HA crede di essere a un punto del piano diverso da quello che il runcard descrive, e le istruzioni tipo 'usa Salta step' diventano ambigue (quale step 5?).
- **PERCHÉ ORA**: Basta un profilo con step disattivati, pratica normale quando si adatta un modello (si disattiva il crash o il dry hop invece di cancellarlo).
- **CORREZIONE**: Scegliere una sola numerazione (quella sugli step abilitati, che è ciò che l'utente percepisce come piano) e usarla ovunque: o il firmware espone enabledIdx/enabledCount in AppStatus, o la riga di modalità della pagina adotta il conteggio del runcard e OLED/MQTT vengono allineati.
- **ADJUDICAZIONE DEL REVISORE**: Doppia numerazione step (status conta disattivati, runcard no). Minore (archetipo 18).
- **VOTO TECNICO** (reale=True): Ho letto il codice reale e OGNI riferimento del rilievo corrisponde. Sorgente della doppia numerazione: in lib/axcore/src/ax/ProfileEngine.h:41 `stepCount()` ritorna `count_`, e count_ è il totale caricato (ProfileEngine.cpp:7-11 `setSteps`: `count_ = count`, copia tutti gli step senza filtrare i disabilitati); stepIdx() (ProfileEngine.h:79) ritorna `s_.stepIdx`, che è un indice ASSOLUTO nell'array completo `steps_[]` — nextEnabled (ProfileEngine.cpp:13-18) scorre `for i=from..count_` e ritorna…
- **VOTO IMPATTO** (reale=False, sev=invariata): La divergenza tecnica è reale: `lib/axcore/src/ax/ProfileEngine.cpp:8-15,247` mostra che `s_.stepIdx` è indice assoluto sull'array completo e il motore salta i disabilitati con `nextEnabled()`, mentre `stepCount()` ritorna `count_` (disabilitati inclusi). Ma la conseguenza non è concreta. Primo: OLED (`src/svc/OledUi.cpp:64`), MQTT (`src/svc/MqttSvc.cpp:224`) e la riga-modalità web (`data-src/index.html:582`) usano TUTTI la stessa numerazione assoluta stepIdx+1/stepCount, quindi sono coerenti tr…

### C41 — Tile 'Giorni' calcolata dal range del grafico invece che da profileStartEpoch: mostra ~1 giorno per una cotta di 12
- **SEVERITÀ**: minore  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: discorde  ·  trovato dagli assi: G  ·  rilievi grezzi: G1-G-8
- **ARCHETIPO (§4)**: 17
- **FILE**: data-src/index.html:778-780 ; data-src/index.html:749-750
- **CODICE**:
```
index.html:778-780: `let startEp=(ST&&ST.profile&&ST.profile.startEpoch)||0; if(!startEp&&LOG.length){const f=LOG.find(p=>p.g!==undefined);if(f)startEp=f.e} $("ferm_days").textContent=(startEp&&now)?((now-startEp)/86400).toFixed(1):'--';` — LOG è caricato con `api("/api/log?hours="+rangeH)` (749-750) e rangeH di default è 24.
```
- **COSA ACCADE**: Stato iniziale: fermentazione condotta in modalità termostato (flusso previsto dalla pagina stessa: 'In modalità termostato... impostala qui a mano' per l'OG), iSpindel che trasmette da 12 giorni, tab Andamento appena aperta col range default 24 h. Sequenza: profile.startEpoch è 0, quindi startEp diventa il primo punto densità delle ultime 24 ore. Conseguenza osservabile: la tile 'Giorni' mostra 1.0 (e cambia da sola passando il range a 14 g, saltando a 12.0): un'età di fermentazione falsa nel pannello usato per decidere quando imbottigliare; anche 'Attività punti/giorno (24h)' con range 6 h è in realtà un'estrapolazione da 6 ore, etichettata 24h.
- **PERCHÉ ORA**: Il termostato fisso è la modalità più semplice e comune per chi non usa i profili; il range default del grafico è 24 h.
- **CORREZIONE**: Ancorare i 'Giorni' a un dato che non dipenda dalla finestra del grafico: l'epoch di cattura dell'OG (persistito insieme a ogGravity, oggi non salvato: aggiungerlo) o, in mancanza, dichiarare l'incertezza ('≥ 1.0 g nel periodo mostrato') invece di un numero secco.
- **ADJUDICAZIONE DEL REVISORE**: Tile 'Giorni' dal range grafico non da profileStart → età fermentazione falsa in termostato. Minore.
- **VOTO TECNICO** (reale=True): Ho tentato di demolire il rilievo verificando ogni riferimento riga per riga; tutti reggono e ogni via di fuga si chiude confermandolo.

VERIFICA DEI RIFERIMENTI (tutti corretti):
- data-src/index.html:778-780 corrisponde testualmente: `let startEp=(ST&&ST.profile&&ST.profile.startEpoch)||0;` / `if(!startEp&&LOG.length){const f=LOG.find(p=>p.g!==undefined);if(f)startEp=f.e}` / `$("ferm_days").textContent=(startEp&&now)?((now-startEp)/86400).toFixed(1):'--';`
- data-src/index.html:749-750: `LOG=a…
- **VOTO IMPATTO** (reale=False, sev=minore): Catena tecnica confermata: in modalità termostato profileStartEpoch=0 e il campo startEpoch è omesso dal JSON (src/svc/WebApi.cpp:251 "if (st.profileStartEpoch) prof[\"startEpoch\"]=..."), quindi il fallback frontend (data-src/index.html:779) ancora startEp al primo punto densità dentro LOG, che è limitato a rangeH ore (default 24, data-src/index.html:749-750 e :506). La tile 'Giorni' (data-src/index.html:264, :780) sottostima davvero l'età. Ma l'impatto NON è concreto per tre motivi convergenti…

### C42 — Le 6 correzioni già pagate su ProbeBus (archetipi 1-4,10,11) non hanno test di regressione (FSM non testabile a banco)
- **SEVERITÀ**: minore  (rilievo originale: grave)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: incompleto  ·  trovato dagli assi: H  ·  rilievi grezzi: G1-H-6
- **ARCHETIPO (§4)**: 2
- **FILE**: src/hal/ProbeBus.h:52-67,130-148,155-201,184-186,98-103 ; platformio.ini:26-29 ; test/test_core/test_main.cpp:5-15
- **CODICE**:
```
ProbeBus.h:155-157: // L'ordine di lettura ruota a ogni ciclo. Leggere sempre nello stesso ordine concentra ogni residuo di marginalità sulla stessa sonda — e con l'ordine fisso quella sonda era l'indice 0, cioè proprio quella che pilota i relè. — h:53-56 (margine 250 ms sui 750 dichiarati), h:32-36 (firstFail contato prima del ritentativo), h:130-135 (stallo rilevato PRIMA della lettura), h:184-186 (grazia solo per QUESTA sonda), h:98-103 (reset solo se cambia la sonda) — platformio.ini:26-29: [env:native] platform = native / test_build_src = false — test_main.cpp:5-15: #include "ax/..." soltanto.
```
- **COSA ACCADE**: Stato iniziale: la suite nativa compila solo lib/axcore (test_build_src=false; tutto src/ dipende da Arduino.h e non compila sull'host). Sequenza: una qualunque manutenzione futura su ProbeBus — ritocco di kConvMs, dell'ordine di lettura, della grazia, del conteggio errori — reintroduce uno dei sei difetti che questo file ha già prodotto e pagato (ordine fisso, margine stretto, ritentativo che nasconde, compensazione a valle, grazia condivisa, azzeramento su riconfigurazione: 6 dei 21 archetipi del §4 nascono qui); i 59 test passano tutti e la regressione arriva in produzione. Conseguenza osservabile: falsi guasti della sonda 0 → controllo in stato sicuro → compressore fermato fuori dal tempo minimo e fermentazione senza controllo, cioè esattamente i guasti storici. È la risposta alla domanda §6.H-1: gli archetipi 2, 4, 10, 11 sarebbero stati intercettati da test a banco della FSM; il test non c'era perché la classe istanzia OneWire/DallasTemperature dentro begin() (h:86-90), chiama dallas_->getTempC direttamente nel ciclo (h:161) e millis() in annullaConversione (h:239) — ma il grosso della logica è già parametrico in nowMs (loop(nowMs)): la separazione è a un'interfaccia di distanza.
- **PERCHÉ ORA**: Il file più corretto della codebase è anche il meno protetto: ogni sua modifica futura è cieca. La probabilità non sta nell'uso ma nella manutenzione.
- **CORREZIONE**: Estrarre la FSM (stati, contatori, grazia, statistiche, finestra grezzi) in una classe pura in lib/axcore con sorgente letture iniettata (interfaccia leggi(addr)->float) e nowMs già parametrico; ProbeBus resta l'adattatore Dallas. Dieci test in forma: sequenze di letture buone/-127/85/fuori-range con clock simulato che riproducono ciascuno dei sei archetipi (es. «la sonda 0 non è mai la prima due cicli di fila», «3 cattive consecutive con ritentativo riuscito contano firstFail ma non invalidano», «stallo di 20 s non fabbrica guasti», «la grazia di una sonda non chiude quella delle altre»).
- **ADJUDICAZIONE DEL REVISORE**: CONFERMATO (tecnico) ma ridimensionato a minore: è un'assenza di copertura (test_build_src=false esclude src/), non un difetto corrente — le 6 correzioni sono in posizione e funzionanti. Rischio: una regressione futura su ProbeBus passerebbe i 59 test. Asse H esplicito nel mandato. Minore ma sostanziale.

---

## Rilievi respinti dalla confutazione (entrambi i confutatori negano)

> Elencati per onestà di processo: la ricerca li ha prodotti, la confutazione li ha demoliti. Restano qui perché un lettore possa verificarne la demolizione.
### C13 — Recovery del MCP: riposo compressore contato dall'istante del guasto, non dall'apertura fisica
- **SEVERITÀ**: respinto  (rilievo originale: grave)
- **CONFIDENZA**: probabile
- **ESITO CONFUTAZIONE**: respinto  ·  trovato dagli assi: B  ·  rilievi grezzi: G1-B-6
- **ARCHETIPO (§4)**: nuovo
- **FILE**: lib/axcore/src/ax/ControlPipeline.h:69-73 ; src/hal/OutputBus.h:62-68 ; lib/axcore/src/ax/CompressorGuard.h:30-33
- **CODICE**:
```
ControlPipeline.h:70-71: // Stato sicuro. La guardia registra comunque lo spegnimento (protezione valida).
      guard_.update(nowEpoch, false);
OutputBus.h:64-66: if (mcp_->recover()) { // Il recovery riparte da OLAT=0: i relè 4..8 sono fisicamente spenti.
CompressorGuard.h:31-32: return elapsed(now, st_.lastOffEpoch) >= (int64_t)restSec_ && elapsed(now, st_.lastOnEpoch) >= (int64_t)restSec_;
```
- **COSA ACCADE**: Stato iniziale: freddo=REL4, compressore in marcia; rest=300 s. Sequenza: il bus I2C si blocca a T0; al primo tick in failsafe la guardia registra lastOffEpoch=T0, ma il relè resta fisicamente chiuso (OLAT latchato — è il rilievo sul relè bloccato). Il bus si libera a T0+360 s: recover() scrive OLAT=0 e il compressore si ferma DAVVERO solo ora. Il latch d'isteresi è rimasto coolOn (la failsafe non lo azzera) e il mosto, per massa termica, è ancora sopra il setpoint: al tick successivo canStart calcola elapsed(now, T0)=360≥300 → il compressore viene rieccitato ~1 s dopo lo stop fisico, a caldo. In generale il riposo effettivo dopo l'apertura reale è restSec meno la durata del guasto: qualunque guasto più lungo di restSec lo azzera del tutto.
- **PERCHÉ ORA**: Stesso trigger del rilievo sul relè bloccato (wedge I2C sul bus condiviso con OLED/RTC) con in più la condizione che il guasto duri più del riposo configurato e che la sonda di controllo sia ancora sopra il setpoint al recovery — plausibile con sonda a immersione e guasti di alcuni minuti.
- **CORREZIONE**: Quando una scrittura di spegnimento del relè freddo fallisce, NON registrare l'off nella guardia (il relè non si è aperto); registrarlo — o chiamare assumeJustStopped — nel momento del recovery, quando OLAT=0 apre davvero il relè. Servirà passare a OutputBus un aggancio alla guardia o esporre l'evento di recovery verso main.
- **ADJUDICAZIONE DEL REVISORE**: RESPINTO: il failsafe (guard.update false) è raggiunto solo se !outputsAllowed; la sonda di controllo è OneWire non I2C, e su REL1/REL3 outputsFault non scatta. Premessa falsa.

### C28 — Array StepDef[12] (576B) sullo stack dei handler web PUT/GET profilo (rischio reset da stack overflow)
- **SEVERITÀ**: respinto  (rilievo originale: grave)
- **CONFIDENZA**: da verificare sul campo
- **ESITO CONFUTAZIONE**: respinto  ·  trovato dagli assi: E  ·  rilievi grezzi: G1-E-2
- **ARCHETIPO (§4)**: 5
- **FILE**: src/svc/WebApi.cpp:449 ; src/svc/WebApi.cpp:381 ; src/svc/WebApi.cpp:422 ; src/main.cpp:132-134 ; src/svc/StateStore.cpp:8-10
- **CODICE**:
```
handlePutProfile(): ax::StepDef steps[ax::kMaxSteps];  // WebApi.cpp:449 (poi saveActive()->writeFile()->LittleFS)
handleProfileById(): ax::StepDef steps[ax::kMaxSteps];  // WebApi.cpp:381 (GET) e :422 (PUT)
startProfileCmd(): // static: niente array sullo stack in fondo alla catena dei handler web
  static ax::StepDef steps[ax::kMaxSteps];  // main.cpp:134 (invariante RISPETTATA qui)
StateStore::load(): // ... questa save/load puo' girare in fondo alla catena dei handler web (causa accertata di reset da stack overflow)  // StateStore.cpp:8
```
- **COSA ACCADE**: Stato iniziale: l'utente salva/aggiorna un profilo lager completo da 12 step via PUT /api/profile o PUT /api/profiles/{id}. sizeof(StepDef)=48 (verificato per compilazione), quindi steps[12]=576 byte allocati sullo stack 'cont' dell'ESP8266 (4 KB) IN CIMA alla catena gia' profonda ESP8266WebServer::handleClient -> parsing -> lambda handler, e prima di chiamare saveActive()->writeFile()->LittleFS.open/rename (la libreria LittleFS usa a sua volta stack non banale). Il codice documenta (StateStore.cpp:8) che un DynamicJsonDocument(1024) in QUESTA stessa catena ha gia' causato 'reset da stack overflow' ed e' stato spostato sull'heap come rimedio; startProfileCmd usa deliberatamente un array 'static' per la stessa ragione. I tre handler del profilo, invece, tengono i 576 byte sullo stack. Conseguenza osservabile: se la somma stack (parsing + array + catena LittleFS) supera i 4 KB, il salvataggio del profilo provoca un reset; nel caso peggiore il device si resetta ogni volta che si prova a salvare proprio i profili a 12 step per cui kMaxSteps e' stato portato a 12, bloccando una funzione essenziale.
- **PERCHÉ ORA**: Raggiungibile con un profilo a molti step (il commento di Types.h nota che i lager completi ne chiedono di piu'), su una catena di stack che il progetto sa gia' essere al limite. Il margine dipende dallo stato dello stack a runtime, non da un input esotico.
- **CORREZIONE**: Applicare la stessa soluzione gia' scelta in startProfileCmd: rendere l'array 'static' (l'esecuzione e' single-thread, nessuna rientranza) oppure spostare il buffer degli step fuori dallo stack del handler. Uniformare l'invariante 'niente array sullo stack in fondo alla catena web' a TUTTI i punti che la toccano (handlePutProfile, handleProfileById GET e PUT, ProfileStore::list).
- **ESPERIMENTO**: Misurare ESP.getFreeContStack() (o cont_get_free_stack) all'ingresso e all'uscita di handlePutProfile con un profilo a 12 step, e confrontare l'high-water mark col limite di 4096; ripetere sotto carico (WiFi attivo, MQTT connesso). Un margine residuo inferiore a poche centinaia di byte conferma il rischio.
- **ADJUDICAZIONE DEL REVISORE**: RESPINTO da entrambi (conseguenza reset non provata), MA la forma è reale e viola l'invariante che il codice stesso rispetta altrove (buffer fuori stack). Esperimento: misurare lo stack high-water durante PUT profilo 12 step. Tenere d'occhio.

### C43 — minStateSec/minSwitchSec non sopravvivono al riavvio: il test 'across reboot' esiste solo per la guardia compressore
- **SEVERITÀ**: respinto  (rilievo originale: minore)
- **CONFIDENZA**: certo
- **ESITO CONFUTAZIONE**: incompleto  ·  trovato dagli assi: H  ·  rilievi grezzi: G1-H-7
- **ARCHETIPO (§4)**: nuovo
- **FILE**: lib/axcore/src/ax/ControlPipeline.h:146-153 ; lib/axcore/src/ax/HysteresisController.h:57-59 ; lib/axcore/src/ax/Interlock.h:60-62 ; src/svc/StateStore.cpp:29-30 ; test/test_core/test_main.cpp:79-93
- **CODICE**:
```
ControlPipeline.h:148-151: HysteresisController hyst_; PidController pid_; CompressorGuard guard_; Interlock lock_; (solo guard_ ha persist/restore) — StateStore.cpp:29-30: st->guard.lastOnEpoch = doc["coolLastOn"] | 0; st->guard.lastOffEpoch = doc["coolLastOff"] | 0; (nessun campo per heatOffAt/coolOffAt/heatChangedAt/coolChangedAt) — HysteresisController.h:58: if (since == 0) return INT64_MAX; // mai commutato: nessun vincolo — test_main.cpp:79-93: test_compressor_rest_across_reboot (l'analogo per Interlock/Hysteresis non esiste)
```
- **COSA ACCADE**: Stato iniziale: caldo a isteresi in marcia (o appena spento), minSwitchSec=120, minStateSec=60. Sequenza: brownout e riavvio in pochi secondi; i latch di Interlock e HysteresisController ripartono a 0, che per costruzione significa «nessun vincolo» (INT64_MAX); se al primo tick la temperatura chiede freddo, l'inversione caldo→freddo avviene senza il tempo morto e senza anti-chattering — la guardia compressore non copre il caso perché lastOn/lastOff del freddo sono lontani. Con alimentazione instabile (brownout ripetuti) il relè caldo può commutare a ogni riavvio senza alcun freno. Conseguenza osservabile: violazione di minSwitchSec/minStateSec attraverso i riavvii, mentre la proprietà gemella del compressore è persistita, ripristinata E testata (test_compressor_rest_across_reboot): l'asimmetria è invisibile a chi legge la suite e nessun commento la dichiara scelta. Danno contenuto (stress termico/meccanico dei relè e dell'impianto, non un guasto di classe 1-2), da cui la severità minore.
- **PERCHÉ ORA**: Qualsiasi blackout breve durante una fase di riscaldamento; più frequente con alimentazioni instabili, che sono anche il caso in cui il vincolo servirebbe di più.
- **CORREZIONE**: Persistere heatOffAt_/coolOffAt_ (e volendo i changedAt dell'isteresi) accanto alla guardia in /state.json, oppure al boot assumere conservativamente «appena commutato» come si fa per il compressore con assumeJustStopped; in alternativa documentare la scelta. Test nella forma di test_compressor_rest_across_reboot applicato a Interlock.
- **ADJUDICAZIONE DEL REVISORE**: RESPINTO (impatto): l'inversione caldo→freddo in pochi secondi è fisicamente impossibile (massa termica ~23L); il compressore (REL3) è protetto dalla guardia persistita (anzi resa più conservativa al boot); il relè caldo (REL1) resistivo tollera le commutazioni ed è comunque diseccitato dal brownout. Nessun danno di classe 1-2.

---

## Ledger di copertura

File del §5 coperti dagli 8 assi del Giro 1 (dichiarazione degli agenti; «intero» = letto integralmente, «parziale» = letto in parte). L'elenco include anche le librerie di terze parti aperte dai confutatori sotto `.pio/libdeps/d1_mini/` e i documenti letti in Fase 3.

- `.pio/libdeps/d1_mini/ArduinoJson/src/ArduinoJson/Configuration.hpp (grep ENABLE_NAN/ENABLE_INFINITY)` — intero: — · parziale: A
- `.pio/libdeps/d1_mini/ArduinoJson/src/ArduinoJson/Numbers/parseNumber.hpp` — intero: A · parziale: —
- `.pio/libdeps/d1_mini/ArduinoJson/src/ArduinoJson/Serialization/CountingDecorator.hpp` — intero: D · parziale: —
- `.pio/libdeps/d1_mini/ArduinoJson/src/ArduinoJson/Serialization/Writers/PrintWriter.hpp` — intero: D · parziale: —
- `.pio/libdeps/d1_mini/ArduinoJson/src/ArduinoJson/Serialization/serialize.hpp` — intero: D · parziale: —
- `.pio/libdeps/d1_mini/ArduinoJson/src/ArduinoJson/version.hpp` — intero: A · parziale: —
- `.pio/libdeps/d1_mini/PubSubClient/src/PubSubClient.cpp (grep: dispatch MQTTPUBLISH)` — intero: — · parziale: F
- `.pio/libdeps/d1_mini/PubSubClient/src/PubSubClient.h (grep: firma callback, retain)` — intero: — · parziale: F
- `~/.platformio/packages/framework-arduinoespressif8266/cores/esp8266/Updater.cpp (grep su end/_verify/setMD5/signature)` — intero: — · parziale: A
- `~/.platformio/packages/framework-arduinoespressif8266/libraries/ESP8266WebServer/src/ESP8266WebServer-impl.h (send/sendContent: righe ~480-560)` — intero: — · parziale: E
- `~/.platformio/packages/framework-arduinoespressif8266/libraries/ESP8266WebServer/src/ESP8266WebServer.h (costanti HTTP_MAX_*)` — intero: — · parziale: E
- `~/.platformio/packages/framework-arduinoespressif8266/libraries/ESP8266WebServer/src/ESP8266WebServer.h (costanti HTTP_MAX_*_WAIT)` — intero: — · parziale: A
- `~/.platformio/packages/framework-arduinoespressif8266/libraries/ESP8266WebServer/src/Parsing-impl.h (parsing del body: righe ~36-200)` — intero: — · parziale: E
- `~/.platformio/packages/framework-arduinoespressif8266/libraries/ESP8266WebServer/src/Parsing-impl.h (righe 38-234: lettura corpo POST, header, timeout)` — intero: — · parziale: A
- `data-src/index.html (grep mirati + finestre 845-880 e 995-1015: editor step, hint 'non avanzerà mai', delayedStartAt, bottone sync ora, libreria profili ramp_rate)` — intero: — · parziale: C
- `data-src/index.html (grep mirati su password/login/token/innerHTML/fetch + righe 525-544, 2030-2062, 2240-2265)` — intero: — · parziale: A
- `data-src/index.html (righe 1-1249 e 1595-2265 riga per riga; blocchi-dati dei modelli 1249-1595 analizzati integralmente con estrazione automatica: conteggio step, gravityMode, timeout)` — intero: G · parziale: —
- `docs/REVISIONE_FORENSE.md` — intero: A,B,C,D,E,F,G,H · parziale: —
- `framework ESP8266 cores/esp8266/core_esp8266_si2c.cpp (righe 440-500 più grep: logica di recovery del bus I2C, a supporto del rilievo sul relè bloccato)` — intero: — · parziale: B
- `framework-arduinoespressif8266 WString.cpp (solo String::toFloat)` — intero: — · parziale: C
- `framework-arduinoespressif8266/cores/esp8266/Stream.h e StreamSend.cpp (sendSize/sendGeneric, timeout)` — intero: — · parziale: F
- `framework-arduinoespressif8266/libraries/ESP8266WebServer/src/ESP8266WebServer-impl.h (handleClient, send/sendContent, righe 282-560)` — intero: — · parziale: F
- `framework-arduinoespressif8266/libraries/ESP8266WebServer/src/ESP8266WebServer.h (costanti HTTP_MAX_*)` — intero: — · parziale: F
- `include/hardware_map.h` — intero: B · parziale: —
- `include/web_assets.h (solo testa e coda: e' un blob PROGMEM autogenerato)` — intero: — · parziale: E
- `lib/axcore/src/ax/CompressorGuard.h` — intero: B,C,F,H · parziale: —
- `lib/axcore/src/ax/ControlPipeline.h` — intero: B,C,F,G,H · parziale: —
- `lib/axcore/src/ax/GravityTracker.h` — intero: C,F,G,H · parziale: —
- `lib/axcore/src/ax/HysteresisController.h` — intero: B,C,F,H · parziale: —
- `lib/axcore/src/ax/Interlock.h` — intero: B,C,F,H · parziale: —
- `lib/axcore/src/ax/PeakEstimator.h` — intero: B,C,H · parziale: —
- `lib/axcore/src/ax/PidController.h` — intero: B,C,H · parziale: —
- `lib/axcore/src/ax/ProfileEngine.cpp` — intero: A,C,D,F,G,H · parziale: —
- `lib/axcore/src/ax/ProfileEngine.h` — intero: C,D,F,G,H · parziale: —
- `lib/axcore/src/ax/SafetyMonitor.h` — intero: B,C,H · parziale: —
- `lib/axcore/src/ax/TimeUtil.h` — intero: C,H · parziale: —
- `lib/axcore/src/ax/Types.h` — intero: C,D,E,H · parziale: —
- `platformio.ini` — intero: D,E,H · parziale: —
- `src/hal/Mcp23008.h` — intero: B · parziale: —
- `src/hal/OutputBus.h` — intero: B,F,G · parziale: —
- `src/hal/OutputBus.h (letto ~riga 40-84: loop/testmode; non l'intero header)` — intero: — · parziale: E
- `src/hal/ProbeBus.h` — intero: E,F,H · parziale: —
- `src/hal/ProbeBus.h (solo costanti kAvgWindow/kCalibMaxSpanC e API statistiche)` — intero: — · parziale: G
- `src/hal/RtcClock.h` — intero: C · parziale: —
- `src/main.cpp` — intero: A,B,C,D,E,F,G,H · parziale: —
- `src/svc/AppConfig.h` — intero: A,B,D,E · parziale: —
- `src/svc/AppConfig.h (solo grep mirati: default usePid, gravityUnit, batteryAlertV, logSampleMin)` — intero: — · parziale: G
- `src/svc/AppConfig.h (solo riga 49 via grep)` — intero: — · parziale: H
- `src/svc/AppStatus.h` — intero: D,E,G · parziale: —
- `src/svc/BootLog.cpp` — intero: D,E · parziale: —
- `src/svc/BootLog.h` — intero: D,E · parziale: —
- `src/svc/CalibStore.cpp` — intero: D · parziale: —
- `src/svc/CalibStore.h` — intero: D,E · parziale: —
- `src/svc/ConfigStore.cpp` — intero: A,D,E · parziale: —
- `src/svc/ConfigStore.cpp (righe 130-208 più grep sui parametri di controllo)` — intero: — · parziale: B
- `src/svc/ConfigStore.cpp (righe 150-209; grep su gravityUnit)` — intero: — · parziale: H
- `src/svc/ConfigStore.cpp (solo campi gravity*, via grep)` — intero: — · parziale: C
- `src/svc/ConfigStore.h` — intero: D · parziale: —
- `src/svc/DataLog.cpp` — intero: D,E,F,G · parziale: —
- `src/svc/DataLog.h` — intero: D,E,G · parziale: —
- `src/svc/DataLog.h (solo firma forEach)` — intero: — · parziale: F
- `src/svc/DeviceRegistry.cpp` — intero: A,D,E · parziale: —
- `src/svc/DeviceRegistry.h` — intero: A,D,E · parziale: —
- `src/svc/MqttSvc.cpp` — intero: A,E,F,G · parziale: —
- `src/svc/MqttSvc.cpp (solo callback comandi e publish, via grep)` — intero: — · parziale: C
- `src/svc/MqttSvc.cpp (solo grep mirato: righe 243-244, calcolo dei giorni di fermentazione)` — intero: — · parziale: D
- `src/svc/MqttSvc.h` — intero: A · parziale: —
- `src/svc/NetworkManager.cpp` — intero: A,F · parziale: —
- `src/svc/NetworkManager.h` — intero: A · parziale: —
- `src/svc/NetworkManager.h (solo ipString/rssi/apActive)` — intero: — · parziale: G
- `src/svc/OledUi.cpp` — intero: E,G · parziale: —
- `src/svc/OledUi.h` — intero: G · parziale: —
- `src/svc/ProfileStore.cpp` — intero: C,D,E · parziale: —
- `src/svc/ProfileStore.h` — intero: D · parziale: —
- `src/svc/StateStore.cpp` — intero: B,C,D,E,F,H · parziale: —
- `src/svc/StateStore.h` — intero: D · parziale: —
- `src/svc/TimeService.cpp` — intero: B,C,D,E,F,H · parziale: —
- `src/svc/TimeService.h` — intero: B,C,F,H · parziale: —
- `src/svc/WebApi.cpp` — intero: A,E,F · parziale: —
- `src/svc/WebApi.cpp (righe 1-50, 311-440, 495-716; più grep mirati su skip/setManual/gravityUnit)` — intero: — · parziale: H
- `src/svc/WebApi.cpp (righe 1-780 lette; 780-1375 e 1390-1434 solo grep mirati: handleTime, computeToken; non letti i corpi di handleCalib*, handleLogCsv/Json, handleProbeCalib, handleUpdate*)` — intero: — · parziale: C
- `src/svc/WebApi.cpp (righe 51-139, 260-365, 680-720, 1340-1434, più grep mirati su arm/disarm/testMode/OTA)` — intero: — · parziale: B
- `src/svc/WebApi.cpp (righe 80-205, 300-479, 480-614, 615-775, 860-979, 1080-1190, 1360-1399: registrazione rotte, auth/login, handler config/profili/coldcrash/comandi/ispindel/og/calib/probe-calib/log.csv/time/relay; NON lette: 1-79, 206-299, 776-859, 980-1079, 1191-1359, 1400-1434)` — intero: — · parziale: D
- `src/svc/WebApi.cpp (tabella rotte 46-131 via grep; 212-361 handleStatus/config/profile; 481-655 coldcrash e comandi; 713-935 ispindel/og/calibrazione; 970-1249 diagnostica sonde e log CSV/JSON)` — intero: — · parziale: G
- `src/svc/WebApi.h` — intero: A,E,F · parziale: —
- `src/svc/WebApi.h (non riletto in questo giro: descritto dalla mappa di fase 1)` — intero: — · parziale: D
- `test/test_core/test_main.cpp` — intero: H · parziale: —
- `test/test_core/test_main.cpp (solo grep RampRate/maxDuration)` — intero: — · parziale: C

**Gap di copertura noti** (da chiudere in un giro successivo):
- `tools/gzip_assets.py` — non letto da alcun asse né confutatore. Genera gli asset PROGMEM al build: un difetto qui (troncamento, encoding, ETag errato) si propaga alla UI servita.
- `docs/ANALISI.md` e `docs/COLLAUDO.md` — **non letti nel Giro 1**; ANALISI.md è stato aperto solo in Fase 3 e ha cambiato i verdetti sul cluster MCP. COLLAUDO.md resta non letto: potrebbe contenere altri fatti di collaudo che ridimensionano o confermano rilievi.
- Il critico di completezza (Fase 4) **non ha potuto girare** (limite di sessione): la verifica sistematica «cosa non è stato guardato» è rimasta incompleta. Vedi §NON VERIFICATO.

---

## NON VERIFICATO — l'elenco onesto di ciò che è rimasto fuori portata

1. **Critico di completezza (Fase 4)**: interrotto dal limite di sessione. Non è stata fatta la verifica indipendente «cosa non è stato guardato». Le lacune elencate nel ledger sono quelle che il revisore ha notato, non un'analisi esaustiva.
2. **Giri 2–5 non eseguiti**: il mandato (§7 fase 5) prevede giri con strategie diverse — **per file** (lettura integrale senza tema), **per archetipo** (ogni forma del §4 cercata ovunque), **per percorso d'uso** (end-to-end), **per assenza** (cosa dovrebbe esserci e non c'è). È stato eseguito **solo il Giro 1 (per asse)**. La ricerca si è fermata per **esaurimento del budget di sessione** (limite settimanale e poi di sessione raggiunti durante la Fase 3), non per convergenza. **Questo è il criterio d'arresto: budget, non convergenza.**
3. **`tools/gzip_assets.py`, `docs/COLLAUDO.md`**: non letti.
4. **Verifiche sul campo**: nessun rilievo «da verificare sul campo» è stato eseguito sul dispositivo (nessun contatto con l'hardware, come da regole operative). Gli esperimenti sono descritti nei rispettivi rilievi.
5. **Comportamento run-time delle librerie**: verificato per lettura dove i confutatori le hanno aperte (ArduinoJson overflow/`measureJson`, ESP8266WebServer parsing e `HTTP_MAX_POST_WAIT`, PubSubClient). Non verificato a run-time.
6. **C42 (test mancanti)**: confermato come lacuna reale ma non è stato scritto alcun test; è una raccomandazione, non una correzione.

---

## Sequenza di correzione proposta

Ordinata per rischio e per dipendenze. **Ogni correzione va revisionata come codice nuovo** (§7): in questo progetto due dei difetti peggiori erano dentro correzioni di altri difetti.

**Blocco 1 — i due nodi «tempo» (vanno insieme, stessa causa):**
1. **C12 + C31** — Dare a `HysteresisController` e `Interlock` un `shiftClock(delta)` e chiamarlo dal blocco salti d'orologio di `main.cpp:710-713` (come già per motore/guardia/gravity); **e** riordinare `loop()` così che `consumeClockJump()` sia processato dopo `web.loop()`/`serialPoll` nello stesso giro, oppure impedire a `controlTick` di girare nel giro in cui è stato accodato un salto. Sono la stessa famiglia (àncore su epoch non allineate a un salto) e vanno corrette e testate insieme.

**Blocco 2 — le macchine a stati che si bloccano/avanzano male in silenzio:**
2. **C14** — Aggiungere allo step `RampRate` (e a ogni step senza uscita a tempo) un timeout di sicurezza **e** un flag di stallo in `TickOutput`, esposto come allarme in `buildStatus`/UI/OLED/MQTT — il rimedio deve rendere il guasto **visibile**, non solo sbloccarlo.
3. **C15** — Rendere `gravityUnit` immutabile a cotta in corso (rifiuto in `handlePutConfig` se un profilo è attivo), oppure convertire coerentemente OG, soglie e riferimenti allo switch. Nel frattempo togliere dal commento la dicitura «solo etichetta».
4. **C21** — Allineare `ConfigStore::save` e `ProfileStore::writeFile` al pattern già presente negli altri quattro store: confrontare i byte scritti con `measureJson`, rimuovere il tmp e `return false` prima del `rename`. Costo e rischio minimi.

**Blocco 3 — superficie di comando esterna:**
5. **C32** — Nel callback MQTT, ignorare i messaggi con flag `retained` per i comandi d'azione (o esporre il flag e trattarli come stato, non come comando).
6. **C07** — Valutare un'autorizzazione per-comando o almeno documentare la dipendenza dagli ACL del broker; disabilitare `profile/start` retained.

**Blocco 4 — congelamento e validazione:**
7. **C17 + C16** — Uniformare il freeze-guard: `-1 °C` anche su `/api/setpoint`, `/api/thermostat` e `climate_temp` MQTT; validare che il payload MQTT sia numerico (rifiutare `toFloat()==0` da stringa non numerica).
8. **C29** — Limitare `handleLogCsv`/`handleLogJson` per default (finestra oraria obbligatoria) e/o cedere il controllo (`yield`/split) fra i record, non solo fra i file-giorno.
9. **C23 + C24** — Propagare l'esito delle scritture su `persistNow`/`append` (allarme storage in `AppStatus`/`SafetyMonitor`); fare il rollback di `cfg` in `handlePutConfig` sul 500 (pattern già in `handleProbeCalib`).

**Blocco 5 — verità di ciò che è mostrato (asse G):**
10. C36, C37, C18, C22, C30, C35 e i minori di presentazione: numeri fabbricati, dashboard congelata, OG del sanificante, overflow °P. Nessuno è urgente quanto i blocchi 1–4, ma sono la classe di guasto che il §1 mette al centro.

**Copertura di test (C42, non una correzione):** estrarre la FSM di `ProbeBus` dietro un'interfaccia di sorgente letture iniettabile e portare a banco i 6 archetipi già pagati su quel file.

---

## Giudizio sulla missione (§1)

> *Il dispositivo può restare acceso e armato per sei mesi, gestendo una fermentazione, senza che il proprietario debba controllarlo; e ogni modo in cui può fallire è o impedito, o reso innocuo, o reso visibile?*

**No.** Non ancora. Tre ragioni, in ordine:

1. **Restano rilievi critici che si manifestano in silenzio.** C14 (rampa che non converge → profilo appeso per settimane senza allarme) e C12/C31 (salti d'orologio che congelano un relè o fast-forwardano il profilo) violano direttamente la clausola «reso visibile» del §1: sono modi di fallire che il dispositivo **non sa raccontare**. C21 può lasciare una cotta senza controllo dopo un blackout, in silenzio. Finché questi non sono corretti (e le correzioni revisionate), la risposta è no.

2. **La ricerca si è fermata per budget, non per convergenza.** È stato eseguito **un solo giro su cinque** (per asse). I giri per file, per archetipo, per percorso d'uso e per assenza — quelli che il §9 dice trovare cose che il primo giro non vede — **non sono stati fatti**. Il critico di completezza non ha potuto girare. Non è dimostrato che non ci sia altro: è dimostrato che *questa* strategia ha dato ciò che poteva.

3. **Aree del ledger coperte solo in parte.** `tools/gzip_assets.py` e `docs/COLLAUDO.md` non letti; la scoperta tardiva di `ANALISI.md` mostra che documenti non nel ledger §5 cambiano i verdetti — segno che la copertura documentale va estesa.

**Ciò che la revisione ha stabilito con fiducia**: il nucleo di controllo (`lib/axcore/`) è solido e ben testato (59 test), gli invarianti di sicurezza centrali (mai caldo+freddo, uscite spente all'avvio/in errore, guardia compressore attraverso i riavvii) reggono, e la maggior parte dei rilievi di sicurezza di rete sta **dentro** un modello di minaccia LAN esplicitamente accettato. I difetti critici trovati sono localizzati e correggibili con interventi mirati e a basso rischio — nessuno richiede una riprogettazione. Corretti i sei critici e completati i giri di ricerca mancanti, il dispositivo può ragionevolmente avvicinarsi alla definizione del §1.

---

## Criterio d'arresto dichiarato

La ricerca si è fermata per **esaurimento del budget** (§7 criterio 3 e §10): durante la Fase 3 sono stati raggiunti prima il limite settimanale e poi il limite di sessione. **Giri eseguiti: 1 (per asse).** Non è convergenza. Un lettore deve considerare questo referto come il risultato di **una** lente di ricerca, avversarialmente filtrata — non della ricerca completa prevista dal mandato.
