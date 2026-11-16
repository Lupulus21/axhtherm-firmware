# CHANGELOG — AxHTherm 2.5

## 3.17.3 — stack delle pagine dei profili

Seconda diagnostica dell'utente (15/11/2026, 3.17.2): CSV completo (2.408 righe), heap minimo
11 KB, stack libero minimo 1.348 B (prima 1.108).
- I 12 step letti dai handler dei profili stanno sull'heap per la durata della richiesta
  (`ProfileStore::StepBuf`, ~530 B), non più sullo stack.
  - Riguarda `ProfileStore::list`, che la pagina chiama a ogni apertura, `handleProfileById`
    (con due array) e `handlePutProfile`.
  - Senza memoria rispondono 503: mai una lista vuota, che il client prenderebbe per id
    liberi.
  - Catene stimate dal binario: lista 1.536 → 960 B, profilo 1.520 → 944 B, salvataggio
    1.168 → 480 B.
- Grafico: un rinvio del failsafe ogni 64 campioni letti (prima uno per campione: 28.902 in
  12 minuti).

## 3.17.2 — export CSV che non partiva, stack della pagina di stato

Dalla prova dell'utente sulla scheda 1 (15/11/2026) e dal file «Scarica diagnostica».

- **Export CSV bloccato** (difetto della 3.14.0). L'export a pezzi aspettava 1.460 B liberi nel
  buffer TCP prima di scrivere. Con lwIP «low memory» (MSS 536) il buffer è al massimo
  2 × 536 = 1.072 B, quindi non scriveva mai: il browser restava appeso e dopo 30 s la scheda
  chiudeva. Ora scrive tante righe quante ne entrano nel buffer libero (128 B per riga, con
  margine, fino a 12).
- **Stack della pagina di stato.** Dopo 24 minuti di uso della pagina lo stack libero minimo era
  1.108 B. Il frame più grande era `handleStatus` (1.248 B), richiesto ogni 3 s e con sotto
  tutto l'invio TCP. Ora `AppStatus` sta sull'heap e la risposta si costruisce in quattro
  funzioni separate (`statusSistema`, `statusCamera`, `statusAllarmi`, `statusRete`).
  Frame di `handleStatus`: 1.248 → 80 B.
- **Registro eventi.** I salti d'orologio sotto il minuto non si registrano più: le correzioni
  NTP di pochi secondi all'avvio lasciavano un «salto di 0 min» a ogni accensione.

## 3.17.1 — stack all'avvio

Prima misura della 3.17.0 sulla scheda 1 (07/11/2026): stack libero minimo 1.152 B già 26 s dopo
l'avvio, senza pagina web né MQTT. Quindi il picco era in `setup()`, il cui frame era di 1.056 B
(`WebApi::Refs` e un array di 12 step) e restava sotto i salvataggi su LittleFS, che sono catene
profonde.
- I riferimenti della WebApi si preparano in una funzione propria (`avviaWeb`).
- Il profilo attivo si riprende nel buffer statico che `startProfileCmd` aveva già
  (`stepsTmp`), senza RAM in più.
- Frame di `setup()`: 1.056 → 160 B.
- La ricostruzione dell'andamento rinvia il failsafe ogni 64 campioni invece che a ogni
  campione: prima gonfiava il contatore dei rinvii (862 in un avvio).

## 3.17.0 — ventola

Quarta fase del piano del 07/11/2026. L'utente monterà una ventola nel frigo: il ruolo è
pronto e si prova con il LED del relè anche prima che ci sia (COLLAUDO, prova 43).

- **Ruolo «ventola»** su un relè libero (`fan.relay`, consigliato REL2: pin diretto, coperto
  dal failsafe; ammessi anche REL4–8). Il relè non conta in `mcpNeeded`: un guasto dell'MCP sulla
  ventola non ferma la cotta. Un relè uguale a quello del caldo o del freddo si ignora, e la
  dashboard lo dice.
- **Logica in `ax::FanControl`**, su secondi monotoni (un salto d'orologio non la tocca):
  - «segue caldo e freddo», con post-funzionamento (default 2 minuti);
  - ricircolo opzionale «X min ogni Y min», in cui contano anche le accensioni per caldo e
    freddo;
  - oppure «sempre accesa»;
  - spenta a scheda disarmata o ferma.
  I cambi sono di minuti, quindi nessun lampeggio sui GPIO dei relè.
- Pannello in Impostazioni, indicazione in dashboard e sull'OLED («+vent.»), sensore binario
  «Ventola» in Home Assistant.

### Misure
126/126 test nativi (2 nuovi: post-funzionamento, ricircolo e «sempre accesa»). `d1_mini`: RAM
statica 44.944 B (+112), flash 780.228 B (+1.680). UI 55,7 KB gzip.

## 3.16.0 — fine fermentazione, step da confermare, allarmi per nome

Terza fase del piano del 07/11/2026.

- **Andamento della densità** (`ax::GravityTrend`). Medie orarie delle letture fresche del
  densimetro assegnato, con pendenza ai minimi quadrati su 24 e 48 ore. Le medie assorbono il
  rumore dell'iSpindel (±0,001 SG).
  - Stati: attiva (cala di almeno 0,004 SG al giorno), rallenta, ferma (meno di 0,001 SG in
    48 ore), oppure ignoto con meno di 36 ore di dati o un densimetro muto da 3 ore.
  - All'avvio l'andamento si ricostruisce dalle ultime 72 ore del registro, così un riavvio o
    un aggiornamento non azzerano le 48 ore. Si azzera a ogni nuova cotta e al cambio di
    densimetro.
- **Avvisi «fermentazione finita» e «fermentazione ferma?»**. «Finita»: densità ferma da 48 ore
  con attenuazione almeno del 50%. «Ferma?»: ferma ma sopra l'atteso, cioè attenuazione sotto
  il 50% o step a densità con la soglia non raggiunta.
  - Isteresi: si spengono solo quando la densità torna a calare davvero, così non vanno e
    vengono vicino alla soglia.
  - Dove si vedono: dashboard, OLED, Home Assistant e registro eventi.
  - «Ferma?» compare fra gli allarmi (`FERMENTAZIONE_FERMA`, id 20) ma non entra
    nell'aggregato «allarme», e si disattiva in Impostazioni (`ispindel.stuckAlert`).
  - Chiude il «lavoro futuro» dello step a densità che può restare appeso.
- **Step «aspetta la mia conferma»** (CT-14, `StepDef::confirm`). A fine step il profilo tiene
  il setpoint e aspetta l'utente: dry hop, campione, travaso.
  - Si conferma con «Conferma e prosegui» in dashboard, con il pulsante «Conferma step» in HA,
    con `POST /api/profile/confirm` o con il comando seriale `conferma`. «Salta step» vale come
    conferma.
  - L'attesa si salva (`waitConfirm` in `/state.json`) e sopravvive ai riavvii.
- **Densità «stabile» negli step meno sensibile al rumore**. La finestra riparte solo con due
  letture di fila fuori banda: un valore isolato non fa più ricominciare le 48 ore. Il motore
  distingue le letture nuove con `TickInput::gravityEpoch`.
- **Registro più onesto.** La densità si scrive solo finché è fresca (prima un densimetro muto
  lasciava una riga piatta con l'ultimo valore). Il record porta la sua unità (bit6 = unità
  registrata, bit5 = °P); CSV e grafico convertono record per record, quindi cambiare SG ↔ °P
  non stravolge lo storico.
- **Home Assistant.**
  - Sensori nuovi: «Fermentazione», «Calo della densità» e «Allarmi attivi» (codici separati da
    virgola, massimo 255 caratteri).
  - Sensori binari nuovi: «Fermentazione finita», «Fermentazione ferma?» e «Step in attesa di
    conferma». Pulsante nuovo: «Conferma step». La densità ha l'unità (SG o °P).
  - Ogni comando ha una risposta su `<base>/cmd_result`, e i rifiuti vanno nel registro eventi.
    Prima un setpoint mandato a scheda ferma o a profilo finito spariva in silenzio.
  - Lo stato MQTT si costruisce sull'heap: `publishState` passa da 1.296 a 304 B di stack.

### Misure
124/124 test nativi (6 nuovi: andamento attivo/fermo/rallenta, dati insufficienti, conversione
°P/SG, conferma dello step, conferma sull'ultimo step e con «Salta», valore isolato fuori
banda). `d1_mini`: RAM statica 44.832 B (+660), flash 778.548 B (+6.188). UI 55,2 KB gzip.

## 3.15.0 — registro eventi, OLED a pagine, interfaccia

Seconda fase del piano del 07/11/2026.

- **Registro eventi nella EEPROM del Tiny RTC** (AT24C32 a 0x50, finora inutilizzata). Conserva
  gli ultimi 256 eventi (`svc/EventLog`, `hal/At24c32.h`, `ax::EventRing`): record da 16 B con
  sequenza, CRC-8 e un byte magico, senza intestazione da riscrivere. Ogni cella viene riscritta
  una volta ogni 256 eventi.
  - Eventi: avvio con la causa, arma e disarma, cambi di modo e di step, pausa e ripresa,
    profilo completato, cold crash raggiunto, caldo bloccato (con il motivo) e sbloccato,
    configurazione salvata, aggiornamento firmware, salti d'orologio.
  - Allarmi accesi e spenti: si registrano solo se restano tali per 30 s (`ax::AlarmDebounce`),
    così uno che va e viene non riempie il registro.
  - Si legge in Sistema → Registro eventi, da `GET /api/events` e con il comando seriale
    `eventi`. Senza EEPROM (vecchia scheda) il registro resta spento, senza allarme.
- **Tabella unica degli allarmi** (`svc/Alarms`): id, codice, testo dell'OLED e priorità, la
  stessa per OLED, registro e (nella prossima versione) Home Assistant.
- **OLED a pagine**, ogni 5 s:
  - la pagina di stato di sempre, con «+N» se gli allarmi sono più d'uno;
  - la fermentazione: densità e OG, attenuazione e ABV, step e tempo che manca, età della
    lettura;
  - tutti gli allarmi, solo se sono almeno due. Così «OROLOGIO INAFFIDABILE» non nasconde più
    gli altri.
  - Contro il burn-in il contenuto si sposta di 0–2 pixel ogni 2 minuti. Nuova luminosità
    configurabile (`display.brightness`, applicata subito).
- **Interfaccia:**
  - conferma su Stop e Salta step (UX-29);
  - pulsante Esci (RS-06);
  - in Sistema: da quanto è accesa (UX-39), stato di MQTT con i tentativi falliti (ID-08) e
    stato del registro;
  - messaggi chiari per una scansione OneWire fallita o vuota (UX-32);
  - dopo «Riavvia» la pagina aspetta la scheda e si ricarica da sola (UX-04);
  - «Scarica diagnostica»: stato, sonde, configurazione (password mascherate) e ultimi 280
    eventi in un file (IP-18).

### Misure
118/118 test nativi (3 nuovi: anello degli eventi su EEPROM vergine e con contenuti estranei,
giro completo, antirimbalzo). `d1_mini`: RAM statica 44.172 B (+264), flash 772.360 B
(+11.120). UI 54,5 KB gzip.

## 3.14.0 — solidità

Prima fase del piano del 07/11/2026 (priorità dell'utente: solidità, fine fermentazione e
avvisi, display e interfaccia).

- **Stack.** Sulla scheda PCBWay il minimo libero era 880 B su 4 KB. La catena più profonda era
  la discovery MQTT a ogni riconnessione: `loop` (560 B) → `publishDiscovery` (1.248 B) →
  `pubSensor` (832 B). Ora `controlTick`, `handleSerialLine`, `logSample` e il temporaneo
  `AppStatus` del tick vivono in funzioni proprie (`tickCamere`), e il frame di `loop()` scende
  a 96 B. Il climate di Home Assistant ha la sua funzione (`publishClimate`), quindi
  `publishDiscovery` scende a 112 B. Catena più profonda stimata: circa 2 KB.
- **Export CSV a pezzi.** Prima l'intero file (fino a 50.000 righe) partiva in una sola chiamata
  e la regolazione restava ferma per tutto il download. Ora l'handler manda le intestazioni e
  `WebApi::loop()` scrive un pezzo di ~12 righe per giro, solo quando il buffer TCP ha posto.
  - `DataLog::cursorRead` riparte dal punto del file in cui si era fermato.
  - Il client fermo per 30 s viene chiuso. Un secondo export contemporaneo riceve 503.
- **Grafici con i picchi.** Ogni gruppo di campioni diventa due punti con minimo e massimo di
  ogni serie (`ax::MinMaxGroup`), nell'ordine in cui sono avvenuti. Caldo e freddo sono in OR sul
  gruppo, quindi un ciclo breve del compressore non sparisce. La risposta si manda a pezzi da ~1 KB.
- **MQTT con il broker spento.** Timeout di connessione a 0,5 s e del CONNACK a 1 s (prima 1,5 e
  2 s). Attese crescenti fra i tentativi: 30 s, 1, 2, 4 min, poi 5 min; si riparte da 30 s quando
  torna il WiFi o dopo una connessione riuscita. Un broker indicato per nome si risolve di nuovo
  dopo 5 fallimenti di fila (ID-06).
- **Pagina web.** Aggiornamento ogni 3 s (prima 2 s) e nessuna richiesta a pagina nascosta: al
  ritorno si aggiorna subito (UX-43). Le soglie dei dati fermi si adeguano (6,5 s / 10 s).
- **OTA.** All'inizio di un upload autorizzato si spegne il caldo (SF-14, `OutputBus::heatOff`).
  Il freddo resta com'è, per la coerenza della guardia del compressore.
- **Testi.** Commenti superati in `ProfileEngine.h` e `WebApi.cpp`, misura dell'heap in
  ARCHITETTURA. Il testo del backup ora dice che la libreria ha il suo file e che la taratura del
  densimetro non è inclusa.

Valutati e lasciati come sono:
- il flush finale del log, residuo noto e documentato in `DataLog::append`: il core non ne
  restituisce l'esito;
- il riconoscimento del cold crash: il flag c'è già (`crashStartEpoch`) e il confronto col nome
  fa da guardia quando il profilo attivo viene sostituito.

### Misure
115/115 test nativi (2 nuovi su `MinMaxGroup`). `d1_mini`: RAM statica 43.908 B (−124), flash
761.240 B (+2.216). Frame misurati sul binario: `loop` 560 → 96 B, `publishDiscovery`
1.248 → 112 B.

## 3.13.0 — cold crash a due velocità

Richiesta dell'utente del 29/10/2026: oltre a 3 °C ogni 6 ore (0,5 °C/h), anche **1,5 °C ogni 6
ore (0,25 °C/h)**, scelta nel modulo del cold crash (`kColdCrashSlowCph`). La velocità si salva
con il cold crash (`crashCph` in `/state.json`; quelli avviati prima valgono 0,5 °C/h) e governa la
stima d'arrivo programmata e i testi. La soglia dell'allarme di discesa lenta si proporziona:
proposta metà di quanto il programma scende in 6 ore (1,5 o 0,75 °C), da 0,25 °C fino a due terzi
(2 o 1 °C). In docs/PROFILI.md la spiegazione del perché una rampa lenta funziona anche con la
risoluzione delle DS18B20: la rampa la fa il setpoint, calcolato sul tempo, non la lettura.

113/113 test nativi (una verifica in più: la stima programmata a 0,25 °C/h). `d1_mini`: RAM statica
44.032 B, flash 759.024 B.

## 3.12.0 — profili: attesa del target, ripresa dopo blackout, libreria

Richieste dell'utente del 28/10/2026 (rilievi CT-12, CT-16, ID-V03 e la seconda fase di ID-18).

- **Attendi il target** (`StepDef::waitTarget`, solo step a temperatura fissa): il tempo dello
  step parte quando il mosto entra nella tolleranza del target; dopo 24 ore di attesa parte
  comunque, con l'avviso «STEP PARTITO SENZA IL TARGET». Lo stato dell'attesa si salva
  (`waitTarget`, `waitTimedOut` in `/state.json`) e sopravvive ai riavvii; il campo nel profilo
  si scrive solo se attivo, quindi i profili esistenti non cambiano.
- **Ripresa dopo un blackout lungo** (`ProfileEngine::kResumeGapSec`, 10 minuti): uno step
  scaduto mentre la scheda era ferma fa avanzare di uno step solo, e il nuovo parte da adesso.
  Prima si avanzavano tutti gli step scaduti con inizi nel passato. Le interruzioni brevi
  mantengono il passo esatto.
- **Export dei profili con l'unità di densità** e conversione SG ↔ °Plato all'import (soglie
  «densità sotto» e bande di stabilità).
- **Backup e ripristino dell'intera libreria** in un file, dalla scheda Profili.

### Misure
113/113 test nativi (4 nuovi sul motore; aggiornati i test del blackout e dei profili lunghi, che
ora avanzano un'ora alla volta). `d1_mini`: RAM statica 44.024 B, flash 758.252 B. Revisione
indipendente: nessun difetto significativo; corretti l'avanzamento in dashboard durante l'attesa,
l'arrivo con un setpoint manuale, l'attesa scaduta durante un blackout, il controllo del tipo di
step e gli avvisi del ripristino della libreria.

## 3.11.0 — cold crash

Richieste dell'utente del 28/10/2026.

- **Discesa unica a 3 °C ogni 6 ore** (0,5 °C/h, `kColdCrashCph`) dalla temperatura misurata al
  target: uno step a rampa °C/h con il caldo disattivato, al posto della sosta a 5 °C e delle due
  velocità di prima.
- **Arrivo segnalato e temperatura mantenuta.** La rampa °C/h finisce solo quando il mosto entra
  nella tolleranza del target, quindi la fine dello step è l'arrivo: si registra l'istante
  (salvato in `/state.json`), lo dicono la dashboard (riquadro verde), l'OLED («COLD CRASH: OK»)
  e Home Assistant (sensore binario «Cold crash raggiunto», per la notifica). Il profilo
  completato mantiene il target finché l'utente non lo ferma: non c'è più il campo «giorni».
- **Allarme di discesa lenta** (`ax::CrashMonitor`): se in 6 ore il mosto scende meno della
  soglia scelta nel modulo (0,5–2 °C, proposta 1,5 °C) e non è ancora arrivato, «COLD CRASH
  LENTO» in dashboard, OLED, MQTT. Sostituisce, durante il cold crash, l'allarme generico della
  rampa ferma.
- **Stima d'arrivo** in dashboard e in Home Assistant: dalla discesa reale (campioni ogni 10
  minuti, finestra di 6 ore) dopo la prima ora, prima sulla velocità programmata.
- **Antigelo**: su uno step che vieta il caldo con setpoint fino a 4 °C, il caldo torna
  consentito se il mosto scende oltre 1 °C sotto il setpoint, fino a mezzo grado sotto. Copre il
  garage sotto zero; le salite libere a temperature più alte non cambiano. Visibile in dashboard e
  in Home Assistant («Antigelo attivo»). Se non basta, allarme «MOSTO TROPPO FREDDO» (sotto −2 °C,
  o 2 °C sotto il setpoint con l'antigelo acceso), e l'allarme di scostamento conta il caldo
  dell'antigelo come consentito.
- Il cold crash non parte con il mosto già sotto il target (409): si usa il termostato.
- In pausa la misura della discesa riparte (niente allarme né stima); un «Salta step» non vale
  come arrivo, che richiede il mosto davvero in tolleranza; durante il cold crash il sensore HA
  «Profilo completato» resta spento (vale «Cold crash raggiunto»). Un avvio normale del profilo
  chiude il cold crash; gli istanti di partenza e arrivo seguono i salti d'orologio.
- Home Assistant: un campo assente dallo stato vale «sconosciuto» invece di lasciare l'ultimo
  valore (una stima d'arrivo vecchia restava a schermo).
- Home Assistant: il termostato accetta −1…40 °C (era 0–30: da HA non si impostavano un
  lagering sotto zero né un kveik a 38 °C; rilievo ID-05).

### Misure
109/109 test nativi (1 nuovo, `CrashMonitor`). `d1_mini`: RAM statica 43.976 B (+288, quasi tutta la
finestra di 6 ore), flash 755.036 B. Revisione indipendente: nessun difetto critico; corretti la nota
dell'antigelo che non compariva, il tetto della soglia (era 3 °C, dava falsi allarmi), l'allarme
«mosto troppo freddo», i salti d'orologio, il riavvio di un cold crash completato e i buchi della sonda.

## 3.10.0 — senza conferma del termostato bimetallico

Decisione dell'utente (28/10/2026): il termostato bimetallico in serie al riscaldatore non verrà
montato, e il firmware non deve chiederne la conferma. Tolti: il rifiuto di armare un relè del
caldo senza conferma (pagina, seriale `arma`, `armOutputs`), il caldo sospeso all'avvio della
3.9.2, il comando seriale `bimetallico ok`, l'allarme «CALDO SOSPESO», la voce `safety.bimetal`
della configurazione (ignorata se presente in un file vecchio) e la sua gestione nel ripristino
del backup. Restano i controlli del riscaldatore (sonda ferma, tempo massimo, salita a caldo
spento, freddo inefficace), il limite assoluto a 42 °C e il limite sull'aria: tolgono il comando
al caldo e danno l'allarme, ma non possono aprire un relè con i contatti incollati. COLLAUDO,
FLASH, ARCHITETTURA e ALLARMI aggiornati. 108/108 test nativi; `d1_mini`: RAM statica 43.688 B, flash 750.788 B.

## 3.9.2 — conferma del bimetallico anche per le schede già armate

Trovato al primo collaudo al banco (28/10/2026, aggiornamento dalla 3.0.0). All'avvio lo stato
«armata» salvato si ripristina senza passare da `armOutputs`, e quindi senza il controllo della
conferma del termostato bimetallico introdotto con la 3.3.0: una scheda armata da un firmware
precedente ripartiva con il riscaldatore abilitato senza conferma. Ora, finché il caldo è
assegnato e la conferma manca, il **caldo è sospeso** e il freddo continua (come la decisione
D1: un aggiornamento a cotta in corso non ferma il frigo). Allarme in dashboard («CALDO
SOSPESO…», con cosa fare), OLED («! CONFERMA BIMETALLICO»), MQTT e una riga sul seriale
all'avvio. Si toglie riarmando dalla pagina con la conferma o con `bimetallico ok` dal seriale.
L.allarme di scostamento termico, come con il blocco del caldo, si congela invece di contare.
108/108 test nativi. `d1_mini`: RAM statica 43.716 B, flash 752.544 B.

## 3.9.1 — esito del salvataggio, residuo

Voce 4.9 del piano di lavoro (residuo di SF-16). Setpoint, termostato, cold crash e
impostazione dell'ora rispondevano «ok» anche se lo stato non veniva salvato su flash (flash
piena o usurata): al riavvio sarebbero tornati com'erano, o nel caso del cold crash con l'indice
dello step di prima applicato al piano nuovo. Ora rispondono 500 con il motivo, come già
arma, disarma, comandi del profilo e sblocco del caldo. I comandi da MQTT non hanno una risposta:
resta l'allarme «scrittura su flash fallita».

108/108 test nativi. `d1_mini`: RAM statica 43.708 B (invariata), flash 751.996 B.

## 3.9.0 — densimetri e sonde

Fase 8 del piano di lavoro, terzo gruppo (rilievi RS-09, ID-V02, ID-11, PH-19 della revisione interna).
Chiude la fase 8.

### Ingresso dei densimetri più robusto
- Senza codice dei densimetri, `/ispindel` accetta solo letture dichiarate JSON (415 altrimenti):
  una pagina web malevola non può inviarle senza preflight, a cui la scheda non risponde.
  iSpindel, GravityMon e la ricetta di Home Assistant inviano già JSON.
  Cambio di comportamento: chi invia a mano (per esempio `curl -d` senza intestazione) deve
  aggiungere `-H 'Content-Type: application/json'`, oppure impostare il codice.
- Allarme «CODICE DEI DENSIMETRI vuoto» quando c'è un profilo con step a densità caricato.
- Pulsanti «Genera codice» (16 caratteri casuali dal browser) e «Togli codice» in Impostazioni →
  iSpindel: prima il codice, una volta impostato, non si poteva più togliere dalla pagina.

### Unità della temperatura e sorgenti senza ID (prerequisiti del ponte BLE)
- La temperatura si converte in °C secondo `temp_units` ("C", "F", "K", anche con «°»; assente = °C). Prima un
  densimetro o un ponte in °F veniva rifiutato per intero sopra i 50 °F, e sotto registrato come
  °C sbagliati.
- Registro dei densimetri: una sorgente con ID 0 (il Tilt inoltrato da Home Assistant) si
  riconosce solo dal nome. Due sorgenti senza ID finivano nella stessa voce, e la scelta del
  densimetro e l'allarme «più densimetri» non funzionavano. Un invio senza ID non cancella l'ID noto.
  Limite: una sorgente senza ID che cambia nome diventa una voce nuova (la vecchia resta finché il
  registro, pieno, non la sostituisce); scegliere il densimetro in Impostazioni evita l'allarme.
- Contratto del ponte (ID stabile, nome unico, °C o `temp_units`, batteria in volt) in
  docs/ISPINDEL.md §7; la ricetta di Home Assistant invia `temp_units`.
- Già a posto e non toccati: la taratura guidata rifiuta le letture senza angolo; la nota della
  ricetta HA sta fuori dal blocco YAML.

### Taratura legata alla sonda fisica
Se in una posizione cambia la sonda e la correzione non viene toccata nello stesso salvataggio,
la correzione segue la sonda: quella che aveva in un'altra posizione (sonde scambiate), oppure 0
per una sonda nuova (`ax::followProbeOffsets`). Prima una sonda sostituita si portava dietro in
silenzio la correzione della vecchia, fino a 5 °C. La pagina lo dice dopo il salvataggio.

### Misure
108/108 test nativi (1 nuovo, la taratura che segue la sonda). `d1_mini`: RAM statica 43.708 B
(+52 rispetto alla 3.8.0), flash 751.844 B (+1,8 KB). Revisione indipendente: nessun difetto
significativo; corretti i minori (codice non rimovibile, simbolo «°», ordine delle prove 34-35).

## 3.8.0 — diagnostica

Fase 8 del piano di lavoro, secondo gruppo (rilievi PH-11, UX-36, UX-44 della revisione interna).

### Tipo di guasto delle sonde
La libreria restituisce −127 per qualunque guasto della DS18B20. Ora, quando una lettura fallisce,
la scheda rilegge lo scratchpad (i 9 byte della sonda, ~12 ms) e classifica il motivo: nessuna
sonda risponde al reset (guasto del bus comune), la sonda non trasmette (tutti 0xFF: staccata o
cavo suo), linea a massa (tutti 0x00), dati corrotti (CRC), sonda riavviata (85 °C di
accensione), fuori scala (`ax::classifyScratchpad`). La rilettura vale anche da ritentativo: un
guasto transitorio si ritenta fino a due volte (prima una), e se i dati riletti sono integri e
plausibili la lettura è recuperata. Una sonda riavviata viene riportata a 12 bit. Il tipo si
conta una volta per ciclo fallito, come le letture perse. Solo per le DS18B20/DS1822: le altre
famiglie tengono il ritentativo di prima con la libreria. Il percorso delle letture riuscite non
cambia. Conteggi per tipo in Sistema → «Sonde:
affidabilità e taratura», in `/api/probes` (`kinds`, `lastKind`) e nel comando seriale `sonde`.

### Dizionario degli allarmi
Nuovo `docs/ALLARMI.md`: per ogni allarme della dashboard e dell'OLED cosa significa, cosa fa il
firmware (solo allarme, sospende, blocca, ferma) e cosa fare; i quattro motivi del caldo bloccato;
le fasce in cima alla pagina; i messaggi di accesso e aggiornamento; i tipi di guasto delle
sonde; le righe dell'OLED.

### Misure
107/107 test nativi (1 nuovo, la classificazione). `d1_mini`: RAM statica 43.656 B (+64 rispetto
alla 3.7.0), flash 750.012 B (+1,2 KB). Revisione indipendente: nessun difetto sulla regolazione;
corretti i tipi di guasto (il bus è comune a tutte le sonde) e il conteggio per ciclo.

## 3.7.0 — interfaccia e accesso

Fase 8 del piano di lavoro, primo gruppo (rilievi UX-01, UX-05, RS-20, RS-07, RS-11, UX-03, ID-18,
UX-35, RS-27, RS-32 della revisione interna).

### Dati fermi distinti dagli altri guasti
La dashboard distingue tre casi: nessuna risposta (dopo due cicli i valori si spengono in grigio,
dopo 8 s la fascia rossa «CONNESSIONE PERSA»), sessione scaduta (solo il pannello di accesso,
prima la fascia gli compariva sopra) e risposta arrivata ma pagina che non riesce a mostrarla
(fascia arancione «PAGINA IN ERRORE» con il motivo: prima sembrava un guasto di rete).

### Riavvio proposto, non imposto
Salvare impostazioni di rete, display, MQTT o orologio non riavvia più la scheda da sola: la
pagina chiede se farlo subito e, con una regolazione in corso, avvisa che i relè resteranno
spenti per una decina di secondi e che la pausa del compressore ripartirà. Rispondendo no le
impostazioni restano salvate e la dashboard lo ricorda, con il pulsante «Riavvia ora».

### Tentativi sbagliati
Le pause di 300 ms dentro la richiesta, che fermavano anche il loop di controllo e si
aggiravano con la Basic auth, sono sostituite da una penalità comune a login, Basic auth,
password attuale e OTA, tenuta **per indirizzo IP**: dal quinto errore di fila ogni tentativo
da quell'indirizzo è rifiutato subito (429) per una pausa che raddoppia da 2 a 60 s. Si azzera
con un accesso riuscito o dopo 15 minuti senza errori. Uno script o un sensore di Home
Assistant rimasto con la password vecchia non chiude fuori il proprietario che entra da un altro
dispositivo. Il codice dei densimetri ha una penalità separata. Il login accetta solo JSON
dichiarato: un modulo inviato da un altro sito non può consumare i tentativi.

Chiuso anche un difetto più vecchio: il salvataggio delle impostazioni rispondeva in modo
diverso a una «nuova» password uguale a quella attuale e a una diversa, e con un cookie di sessione
si potevano provare password senza limiti. Ora qualunque cambio chiede prima la password attuale.

### Aggiornamento firmware verificato
Il `firmware.bin` del core contiene la propria lunghezza e un CRC dell'intero file (li scrive
`elf2bin.py`; sono quelli che il core usa per `ESP.checkFlashCRC`). La scheda li verifica mentre
il file arriva e conferma l'aggiornamento solo se il file è intero e integro. Prima un file
troncato con un'intestazione valida, anche già rovinato sul disco, veniva installato sopra il
firmware buono: il bootloader copia senza controlli, e la scheda restava in riavvio continuo,
recuperabile solo via USB. Ora resta il firmware attuale. Rifiutati anche i `.bin.gz` e i file
di altro tipo. La pagina controlla la lunghezza dichiarata prima di inviare, verifica che la
sessione sia valida, mostra l'avanzamento e l'esito restituito dalla scheda, e si ricarica da
sola quando la scheda torna. Da riga di comando non serve nulla di nuovo.

### Backup della configurazione
Sistema → «Scarica backup» salva le impostazioni in un file JSON, con le password già mascherate
dalla scheda; «Ripristina» lo rimanda attraverso il salvataggio normale, con la stessa
validazione, e le password della scheda restano (anche un codice dei densimetri vuoto nel file
non cancella quello della scheda). La conferma del termostato bimetallico non si ripristina mai:
riguarda il cablaggio di quella scheda. Da un backup di un'altra scheda (riconosciuta dal chip)
non si copiano nome, rete, display, topic MQTT e sonde. Dopo un reset di fabbrica la pagina
ricorda di reinserire la password del WiFi prima di riavviare. Libreria dei profili, tarature
del densimetro e registro dei densimetri restano fuori (seconda fase, se servirà).

### Nome della scheda nell'intestazione Host
Contro il DNS rebinding (una pagina web che si fa passare per la scheda): pagina, API, OTA e
`/ispindel` rispondono solo se la scheda è chiamata con un IP, con il suo nome, o con il suo nome
seguito da `.local` o da un dominio dei router di casa (`.lan`, `.home`, `.home.arpa`,
`.fritz.box`, `.localdomain`, `.internal`). Altrimenti 403, senza rivelare il nome. Non ferma una
pagina che chiama la scheda per IP: per `/ispindel` la difesa resta il codice dei densimetri
(facoltativo, da impostare), per il resto la sessione.

### Limiti noti
Le impostazioni in attesa di riavvio sono già salvate: il comando seriale `password` e la
schermata di prima configurazione dell'OLED mostrano i valori nuovi della rete di
configurazione mentre gira ancora quella vecchia; l'uscita dalla modalità ridotta riaccende MQTT
con le impostazioni nuove.

### Misure
106/106 test nativi (4 nuovi: penalità, penalità per indirizzo, verifica dell'immagine su un file
sintetico e sul `firmware.bin` compilato). `d1_mini`: RAM statica 43.592 B (+368 rispetto alla
3.6.0), flash 748.800 B (+5,6 KB). Due revisioni indipendenti: la prima ha mostrato che l'MD5
calcolato dal browser non proteggeva da un file già troncato sul disco, e ha portato alla verifica
di lunghezza e CRC sulla scheda.

## 3.6.0 — regolazione e limiti

Fase 7 del piano di lavoro (rilievi CT-02, UX-06, ID-15, SF-07, CT-09, UX-27, CT-11, SF-08, SF-05
della revisione interna). Decisione dell'utente D2 (26/10/2026): le sonde «camera» e «ambiente» sono
montate.

### Parametri di regolazione in intervalli plausibili
Al salvataggio: tolleranza 0,1–3 °C, kP 0–50, kI 0–5, kD 0–100, finestra PID 2 s–10 min, piena
potenza sotto 0,5–20 °C, taglio sopra 0–5 °C (oltre ai tetti già presenti su riposo e tempi). Un
valore fuori intervallo viene riportato nel limite; la risposta elenca i campi corretti
(`corrected`) e la UI lo dice e rilegge la configurazione. All'avvio, una configurazione
scritta da un firmware precedente con valori fuori intervallo viene corretta e lo dice una riga
di log. Un campo dei limiti sull'aria lasciato vuoto non viene inviato (resta com'era). Prima un refuso (tolleranza 5 invece di
0,5) passava in silenzio e degradava regolazione e chiusura delle rampe.

### Limiti sull'aria della camera (sonda «camera»)
- **Freddo sospeso** se l'aria scende sotto l'aria minima (default −5 °C, −10 = nessun minimo):
  nel cold crash a −1 evita che il mosto vicino alle pareti geli. Riparte 3 °C sopra: un margine
  stretto farebbe ripartire il compressore ogni pochi minuti.
- Facoltativo, **spento di default**: freddo sospeso anche sotto «setpoint − distanza». Rallenta
  il raffreddamento iniziale e fa ciclare il compressore sull'aria: lo si attiva solo se serve.
- **Caldo sospeso** se l'aria supera «setpoint + 10 °C» (0 = spento): un cavo scaldante in aria
  non cuoce la camera mentre il mosto insegue il target.
- Sonda della camera configurata ma non valida: limiti disattivati e allarme.
- Gli stop imposti da un limite (o dallo step, o da un blocco) non insegnano nulla allo
  stimatore predittivo.
- L'allarme di scostamento termico **continua a contare** quando un limite sull'aria sospende
  l'attuatore (prima si azzerava): un limite quasi sempre attivo, per esempio con la sonda
  «camera» attaccata all'evaporatore, non può nascondere un impianto che non ce la fa. Si
  congela solo con il caldo bloccato dalla sicurezza, che ha già il suo allarme; un allarme
  maturato non lampeggia più mentre è congelato.
- Con il freddo sospeso dal limite sull'aria la finestra del «freddo inefficace» (codice 4)
  riparte a ogni ciclo: un SSR incollato in quel caso lo trova la salita a caldo spento
  (codice 3), perché a freddo sospeso il riscaldatore fa salire il mosto.

### Limiti assoluti sul mosto
Caldo tolto oltre 42 °C, freddo tolto sotto −3 °C, con 1 °C di isteresi e allarme. Fissi nel
codice: i setpoint stanno in −1..40, quindi scattano solo se qualcosa a monte ha sbagliato.

### Freddo non comandato: solo allarme
Relè del frigo incollato (`CoolWatch`, serve la sonda «camera»). Con il freddo comandato
spento, solo un frigo che gira tiene l'aria ben più fredda (2 °C) del mosto, del setpoint e,
se c'è la sonda «ambiente», della stanza. Due firme:
1. dieci minuti dopo uno spegnimento vero del freddo l'aria **non è risalita**: è il guasto
   tipico, il relè che si incolla mentre il frigo gira;
2. a frigo fermo l'aria crolla di 3 °C in 5 minuti (finestra scorrevole; serve la sonda
   «ambiente»): il relè che si chiude da solo.
Vale anche in modo fermo (riferimento: il mosto), per un relè che si incolla a fine cotta.
L'allarme si spegne quando il freddo torna comandato o quando l'aria risale vicino al mosto:
un relè davvero incollato la tiene fredda, un falso allarme si smentisce da solo.
**Nessun blocco**: con il frigo che non si ferma il riscaldatore è l'unico contrasto al gelo, e
il firmware non può aprire un relè saldato. Per lo stesso motivo, finché l'allarme è attivo, i
rilevatori della sonda ferma e del caldo prolungato non bloccano il riscaldatore (il caldo al
100% che non fa salire il mosto è spiegato dal frigo). Allarme in dashboard, OLED («! FRIGO NON SI
FERMA») e MQTT.

### Inversione caldo/freddo: non rilevata in automatico, di proposito
La sua firma (mosto in salita col "freddo" acceso) coincide con quella di un SSR incollato che
sovrasta il frigo, e spegnere il freddo nel secondo caso toglierebbe l'unico contrasto al
riscaldatore. La revisione indipendente ha mostrato che anche usando la sonda dell'aria i casi
si confondono (compressore che cicla, cavo in aria). L'inversione la scopre la prova degli
attuatori al collaudo (prova 18), da fare prima di ogni cotta.

### Pendenza e tempo d'arrivo
`SlopeEstimator`: pendenza del mosto su 20 minuti (media dei 3 campioni più recenti contro i 3
più vecchi, un campione al minuto: errore di quantizzazione sotto 0,2 °C/h) e minuti al setpoint.
Un buco della sonda oltre 90 s fa ripartire la finestra.
In dashboard («↗ +0,80 °C/h · al setpoint tra 2 h 30 min»), in `/api/status` (`slope`) e in Home
Assistant («Pendenza temperatura»).

Due revisioni indipendenti prima del rilascio. La prima: quattro rilievi maggiori portati a
riprogettare (inversione tolta, freddo non comandato reso solo allarme, limiti sull'aria più
prudenti, allarme di scostamento non più azzerato dai limiti). La seconda: riferimento del
freddo non comandato su min(setpoint, mosto) e allarme che si smentisce, scostamento termico
che non lampeggia e conta anche sotto i limiti. Corretti anche i minori: apprendimento
predittivo, buchi della sonda, campi vuoti, correzioni all'avvio, capienza di `/api/status` a
4,5 KB, rilevatori e pendenza azzerati al cambio di una sonda, etichette delle sonde nella UI
(«camera (aria)», «ambiente (stanza)»: i ruoli sono fissi).

### Misure
102/102 test nativi (11 nuovi). `d1_mini`: RAM statica 43.224 B (+476 rispetto alla 3.5.0), flash
743.200 B.

## 3.5.0 — diagnostica di campo

Fase 6 del piano di lavoro (rilievi PH-02, SF-10, SF-12, PH-10, IP-16, SF-V01, RS-12 del
revisione interna): rendere misurabile la stabilità sul campo.

### Registro dei riavvii completo
- **Motivo dei riavvii voluti** (`RebootInfo`, memoria RTC): riavvio richiesto, impostazioni
  salvate, aggiornamento firmware, reset di fabbrica. Prima comparivano tutti come «Software/
  System restart», indistinguibili.
- **Durata della sessione precedente**: aggiornata ogni 10 secondi in memoria RTC (niente usura
  della flash). Il campo esisteva in `/boot.json` ma non veniva mai riempito.
- **Causa ed indirizzo dell'eccezione** (`exccause`, `epc1`) quando il riavvio è un blocco del
  firmware: con il binario corrispondente si risale alla riga.
- **Contatore totale** degli avvii, oltre agli ultimi 8.
- In Diagnostica la tabella mostra motivo, eventuale eccezione e durata («dopo»).

### Riavvii a catena
Tre blocchi di fila, ognuno entro 10 minuti dall'avvio: la sessione parte in **modalità
ridotta**, senza MQTT, con l'allarme «RIAVVII A CATENA» in dashboard e sull'OLED. Dopo 30 minuti
stabili la modalità ridotta finisce da sola e MQTT si riaccende; un riavvio voluto chiude la
serie. La regolazione non si ferma mai. Contano come blocchi il watchdog, le eccezioni e i
**blocchi software** (memoria esaurita in `new`, stack, `abort`/`assert`): l'SDK li riporta come
un normale riavvio software, e li distingue l'assenza dell'intento, che ogni riavvio voluto
scrive prima. La memoria RTC usata sta dal byte 256: i primi 128 li usa l'aggiornamento OTA.

### Ritardi del ciclo di controllo
Tick oltre 2 s (massimo, ultimo, quando) e rinvii del failsafe dai percorsi lunghi legittimi; un
tick lungo durante il quale il failsafe è stato rinviato (export, OTA, discovery MQTT) si conta a
parte, così il massimo misura solo gli stalli veri, e la durata di `setup()` non conta. In
`/api/status` (`loop`), nella pagina Sistema, nel comando seriale `heap`, nel log ogni minuto e in
Home Assistant (sensore «Ritardo massimo del ciclo»). Prima gli stalli sotto i 60 s del failsafe
comparivano solo sul seriale, e solo oltre i 5 s.

Revisione indipendente prima del rilascio: corretti due rilievi maggiori (blocchi software non
riconosciuti come tali; modalità ridotta che non finiva da sola) e quattro minori (regola della
serie, blocco mascherato da un intento, durata delle sessioni brevi, ritardi falsati da avvio e
operazioni lecite). Verificato che la zona RTC usata non tocca quella dell'OTA né altro.

### Misure
91/91 test nativi. `d1_mini`: RAM statica 42.748 B (+336 rispetto alla 3.4.0), flash
737.596 B.

## 3.4.0 — accesso sicuro di default

Fase 5 del piano di lavoro (rilievi RS-01, RS-02, RS-V01, RS-10, RS-V07, UX-11, IP-11, RS-23,
RS-16, UX-V03 della revisione interna). Scelta dell'utente: password casuale **solo** su scheda nuova
o dopo un reset di fabbrica, letta dal monitor seriale; il display è solo un modo in più.

### Password e nomi per scheda
- **Primo avvio** (nessun `/config.json`): due password casuali e distinte, per l'interfaccia e
  per la rete di configurazione, 12 caratteri senza caratteri ambigui (circa 59 bit) dal
  generatore hardware; nomi con il chip ID: `axhtherm-XXXX` per rete di configurazione
  (`-setup`), nome `.local`, topic MQTT e dispositivo Home Assistant.
- Le credenziali si scrivono **una volta** sul seriale, nel riquadro «CREDENZIALI DI QUESTA
  SCHEDA». Comandi seriali: `password` le rimostra, `password nuova` le rigenera (la rete di
  configurazione cambia al riavvio). Niente stampa a ogni avvio: i log mandati per assistenza
  non devono contenere segreti.
- **OLED**: con la rete di configurazione attiva e finché le password sono quelle generate,
  una schermata «PRIMA CONFIGURAZIONE» con rete, password della rete, indirizzo e password
  admin. Quando l'utente sceglie la sua password, non le mostra più.
- **Schede già configurate**: niente cambia, tengono password e nomi. La password di fabbrica
  `axhtherm` resta riconosciuta (allarme e OTA disattivato finché non la cambi).

### Password attuale richiesta
- Per cambiare la password dell'interfaccia serve quella attuale (`currentPassword` nel PUT di
  `/api/config`, campo «Password attuale» in Impostazioni).
- Per l'aggiornamento firmware serve la password attuale (intestazione `X-Ax-Password`, campo
  accanto al pulsante). Un cookie di sessione da solo non basta più.

### Lunghezza minima
Password nuove dell'interfaccia e della rete di configurazione: da 8 caratteri in su (sotto,
la rete di configurazione partirebbe in silenzio con la password di fabbrica). Le configurazioni
esistenti con una password corta non vengono rifiutate: danno un allarme.

### Interfaccia
Il nome della scheda compare nell'intestazione: con due schede si sa quale si sta guardando.

### Correzioni dalla revisione indipendente (prima del rilascio)
Nessun aggiramento delle password; corretti:
- **Casualità**: la radio si sveglia prima di generare le password (con la radio spenta il
  generatore hardware del core 3.x è dichiarato "probabilmente" pseudo-casuale) e si mescola il
  contatore dei cicli.
- **OLED**: la schermata con le password compare solo alla prima configurazione (nessuna rete
  di casa impostata), non con l'AP di emergenza, dove copriva temperatura e allarmi.
- **Nome della scheda** validato (1–26 caratteri `[a-z0-9-]`): un nome più lungo impediva alla
  rete di configurazione di partire. SSID e password della rete di configurazione hanno ora una
  regola sola (`NetworkManager::apSsidFor/apPassFor`): il seriale stampa quelli davvero in uso.
- **Ultima risorsa da seriale**: `reset fabbrica` (con conferma `reset fabbrica si`).
- **Reset di fabbrica dalla UI**: chiede la password attuale, perché dopo si rientra solo dal
  seriale o dal display.
- **OTA con password sbagliata**: non rinvia più il failsafe (chi ha solo il cookie non può
  tenere fermo il loop mandando dati); messaggio d'errore con il motivo vero.
- Password senza spazi in testa o in coda (l'intestazione HTTP li toglie); 300 ms di attesa sui
  tentativi falliti di password attuale e OTA, come il login; la password attuale viaggia solo
  quando la si cambia.

### Misure
91/91 test nativi. `d1_mini`: RAM statica 42.412 B (+128 rispetto alla 3.3.0), flash
734.384 B.

## 3.3.0 — sicurezza del riscaldatore

Fase 4 del piano di lavoro (rilievi SF-01, SF-02, SF-03, SF-04, SF-06, SF-16, SF-V02 del
revisione interna). Decisione dell'utente D1 (25/10/2026): un guasto del riscaldatore
**blocca il solo caldo**, con riarmo manuale e blocco conservato dopo i riavvii; il freddo
resta consentito. Sostituisce la scelta precedente "solo allarme, mai taglio delle uscite".

### Quattro rilevatori (`lib/axcore/src/ax/HeaterGuard.h`, logica pura con 11 test)
| Codice | Condizione | Che guasto copre |
|---|---|---|
| 1 sonda ferma | caldo acceso di continuo e la sonda di controllo sale meno di 0,2 °C in una finestra (default 60 min, configurabile 15–240, 0 = spento) | sonda congelata o fuori dal mosto: il PID resterebbe al 100% per sempre |
| 2 tempo massimo | caldo acceso di continuo oltre N ore (default 12, configurabile 2–48, non disattivabile) | qualunque guasto che tenga il caldo acceso; non si fida della sonda |
| 3 salita a caldo spento | dopo 10 min di caldo spento, +1 °C entro 20 min; un salto di oltre 0,5 °C fra due letture fa ripartire la finestra (sonda spostata, offset cambiato) | SSR o relè del caldo incollato in conduzione, con un riscaldatore abbastanza potente |
| 4 freddo inefficace | freddo acceso di continuo, temperatura oltre 2 °C sopra il setpoint, discesa < 0,3 °C in 2 ore | caldo che lavora contro il frigo (SSR incollato con riscaldatore debole), o frigo guasto |

I tempi sono accumulati dai tick, non differenze di millis(); con la sonda non valida si
congelano, a camera disarmata, configurazione non valida o uscite MCP in guasto si azzerano.
I rilevatori guardano i relè DAVVERO assegnati: senza relè del caldo non girano, e senza relè
del freddo il rilevatore 4 non vede mai il freddo acceso. Cambiare sonda di controllo o il
suo offset fa ripartire le finestre.

**Limiti, da conoscere.** Il PID a finestra spegne e riaccende il caldo, quindi "acceso di
continuo" significa potenza al 100% circa: i rilevatori 1 e 2 vedono una sonda congelata
SOTTO il setpoint (il caso pericoloso), non una congelata al setpoint o sopra. Il rilevatore 3
vede salite di almeno ~3 °C/h: un SSR incollato su un tappetino da 25–60 W in 20 L (~1 °C/h)
lo vede solo il 4, se c'e' un frigo che lo contrasta. Per questo il termostato bimetallico
resta obbligatorio: i rilevatori sono la seconda linea.

### Il blocco
- Toglie il consenso al caldo dal punto unico della pipeline (`allowHeat`), che azzera anche
  l'integrale del PID. Il relè si spegne nello stesso tick del rilevamento.
- Salvato subito in `/state.json` (`heatLock`, `heatLockAt`): un riavvio, anche autonomo, non
  lo cancella. Si toglie solo con un gesto esplicito: pulsante «Sblocca il caldo» in
  dashboard (`POST /api/safety/reset`) o `sblocca` da seriale.
- Visibile ovunque: avviso con il motivo e il rimedio in dashboard, «! CALDO BLOCCATO» sull'OLED
  (in cima alla priorità), entità «Caldo bloccato» e codice `heat_lock_code` in Home Assistant,
  flag di allarme nel registro.

### Termostato bimetallico confermato sulla scheda
Un relè del caldo si arma solo dopo la conferma del termostato bimetallico a riarmo manuale in
serie al riscaldatore: la UI la chiede la prima volta e la salva in `/config.json`
(`safety.bimetal`); da seriale `bimetallico ok`. Un dispositivo già armato resta armato.

### Altro
- Esito del salvataggio (SF-16): arma, disarma, avvia/ferma/pausa/riprendi/salta step e
  sblocco rispondono con errore 500 se lo stato non è stato scritto su flash, invece di dire
  "ok" su uno stato che al riavvio tornerebbe indietro. Cold crash, setpoint e termostato no:
  restano da allineare.
- Non si arma con la configurazione non valida (corrotta al boot, default in RAM): la
  conferma del bimetallico avrebbe riscritto `/config.json` con i default.
- La risposta dell'armamento che salva la conferma porta il nuovo `configRev`: la UI lo
  allinea e il salvataggio successivo delle impostazioni non prende un 409 falso.
- MQTT (SF-V02): il failsafe non attivo e il blocco del caldo hanno un'entità propria e
  accendono l'allarme aggregato.
- Impostazioni: nuovo pannello «Sicurezza del riscaldatore»; `docs/COLLAUDO.md` Fase E con le
  prove guidate dei rilevatori a carichi scollegati.

Revisione indipendente prima del rilascio: nessun difetto critico; corretti i due rilievi
maggiori (rilevatori sui relè realmente assegnati; salti istantanei della sonda) e i minori
(congelamento a sonda invalida, soglia del cold crash, configRev, rollback della conferma da
seriale, config non valida, priorità sull'OLED dopo i guasti in corso).

### Misure
91/91 test nativi (12 nuovi). `d1_mini`: RAM statica **42.284 B** (+376 rispetto alla
3.2.0), flash 729.884 B.

## 3.2.0 — fondamenta: memoria misurata e liberata

Prima di aggiungere funzioni si misura e si libera memoria: è il vincolo principale di
questa scheda (fase 3 del piano di lavoro; rilievi PH-07, PH-08, IP-17, IP-01, PH-30 del
revisione interna).

### Testi in flash: −6,7 KB di RAM
Su ESP8266 i letterali stanno in `.rodata`, che vive in DRAM: erano circa 12 KB di testi,
quasi tutti nostri. Spostati in flash i formati di `Serial.printf` (`printf_P`/`PSTR`), i
messaggi d'errore dell'API (`sendErr(…, F(…))`, nuova sovraccarica per le stringhe in flash),
i messaggi di `validateSteps` (`PSTR`/`FPSTR`), i testi degli allarmi di `/api/status`, le
scritte dell'OLED e i percorsi delle rotte. Restano in RAM le chiavi JSON (ArduinoJson le
conserva per puntatore) e pochi testi brevi.

I testi degli allarmi ora vengono copiati nel documento di `/api/status`: la sua capacità
passa da 3 a 4 KB, solo per la durata della richiesta.

### Immagini di avvio iOS da un solo gestore
Le 10 immagini non hanno più una rotta ciascuna (handler, String dell'URI e std::function
in heap permanente): le serve `onNotFound` confrontando il percorso con la tabella.

### Minimi di memoria dall'avvio
Nuovo `HeapMon`. In `/api/status` (`heapMin`, `heapMaxBlockMin`, `stackFreeMin`,
`allocFail`), nel comando seriale `heap`, nella riga di log ogni minuto e nella pagina Sistema:
- minimo storico dell'heap libero, misurato dall'allocatore a ogni allocazione
  (`-DUMM_STATS_FULL`, con `-DUMM_INFO` dichiarato a mano), quindi picchi degli handler compresi;
- minimo del blocco contiguo più grande, campionato ogni secondo;
- stack libero minimo del loop;
- numero di allocazioni fallite.

### Identificativo di build
`tools/build_id.py` genera `include/build_id.h` (non versionato) con il commit e il
marcatore `-mod` se l'albero ha modifiche non salvate. Compare nel banner seriale, in
`/api/status` (`build`), nella pagina Sistema e nella versione del dispositivo in Home
Assistant. Due binari "uguali" ora si distinguono.

### Misure
79/79 test nativi. `d1_mini`: RAM statica **41.908 B** (−6.684 rispetto alla 3.1.0), flash
724.856 B (+1.840: i testi spostati occupano ora la flash, come previsto). Heap sul
dispositivo da misurare al collaudo.

## 3.1.0 — una camera per scheda, firmware più snello

**Il firmware torna a governare una sola camera di fermentazione.** La scelta d'uso è
una scheda per frigo; il codice per più camere della 3.0.0 aumentava superficie, memoria e
casi da verificare senza servire a nulla. La 3.0.0 a due camere resta recuperabile dal tag
git `v3.0.0`.

Strada scelta: **potatura**, non riscrittura. La struttura interna (`ChamberCtx`, gli array
per camera) resta, fissata a una camera (`static_assert(AX_N_CHAMBERS == 1)`): così i
percorsi di sicurezza — tick di controllo, persistenza, ripresa al boot, failsafe — non
vengono riscritti, e tutte le correzioni fatte durante la 3.0.0 restano.

### Cosa è stato tolto
- Environment `d1_mini_2ch` e flag `AX_N_CHAMBERS` sovrascrivibile.
- Rotte `/api/ch/<n>/...`, la loro tabella e i wrapper `onChamber` (le 15 rotte si
  registrano di nuovo con `server_.on`, senza la lambda in più che costava heap permanente).
- Campi `chamber`/`chambers`/`chamberLabel` in `/api/status` e `chambers[]` in `/api/calib`.
- Sfasamento della finestra PID e dell'avvio del compressore fra camere.
- Densimetro per camera: instradamento a tre regole, rifiuto 409 all'avvio di un profilo a
  densità senza densimetro e `gravitySourceMissing` (a una camera erano sempre inattivi).
  Torna il filtro della 2.23.2: nome assegnato o, se vuoto, qualsiasi; letture senza nome
  sempre accettate. L'azzeramento della lettura al cambio di densimetro resta.
- Comando seriale `arma <camera> …` (`disarma <n>` resta accettato e disarma tutto);
  alternanza delle camere sull'OLED.
- MQTT e discovery Home Assistant per camera: topic e `uniq_id` della camera 1 erano già
  quelli della 2.23.2 e non cambiano, quindi entità e automazioni esistenti restano valide.
- UI: selettore di camera, righe multiple dei densimetri, etichette «— camera N».
- 6 test legati alle camere (sfasamenti, sorgente di densità assente): 79 test nativi.

### Cosa resta, di proposito
- Lettura di `/state.json` in schema v1 e v2, e di `/config.json` con la sezione `chambers`.
  Il file di stato si continua a scrivere in v2 con un solo elemento, leggibile dalla 3.0.0.
- Pulizia del registro per nome: i file `/log/d<giorno>_1.bin` lasciati dalla 3.0.0 vengono
  smaltiti dalla retention come gli altri.
- Driver MCP23008: è montato sulle schede PCBWay e tiene definiti REL4–8.

### Correzione: falso allarme di storage dopo l'aggiornamento
Con un `/state.json` scritto dalla 3.0.0 a due camere, la camera in più veniva scartata e
segnalata come guasto di scrittura su flash, fino al riavvio successivo. Ora una camera
scartata che era **disarmata e ferma** produce solo una riga di log; l'allarme resta per
quella che era armata o in marcia, perché il suo stato è davvero perso.

### Aggiornamento di un dispositivo con la 3.0.0
Sul broker restano le entità retained della seconda camera (`…_ch2`): Home Assistant le
mostra con valori fermi. Si cancellano pubblicando un messaggio vuoto retained su ogni topic
di configurazione. `<id>` è il chip ID esadecimale (`axhtherm_<id>` è l'identificativo del
dispositivo in HA), `<base>` il base topic MQTT (default `axhtherm`):

```sh
# funziona in zsh (shell di macOS) e in bash
B() { mosquitto_pub -h '<broker>' -u '<utente>' -P '<password>' -r -n -t "$1"; }
D=axhtherm_<id>
B "homeassistant/climate/${D}_ch2/config"
for k in tControl tAux1 tAux2 setpoint mode_text step gravity og attenuation abv days \
         grav_age grav_batt heap rssi; do B "homeassistant/sensor/${D}_ch2_$k/config"; done
for k in heat cool armed running completed alarm grav_stale batt_low; do
  B "homeassistant/binary_sensor/${D}_ch2_$k/config"; done
for k in start stop pause resume skip; do B "homeassistant/button/${D}_ch2_$k/config"; done
B "<base>/ch2/state"
```

**Densimetro.** Se nella 3.0.0 la camera 1 accettava qualsiasi densimetro (campo vuoto) e la
camera 2 ne aveva uno assegnato, dopo l'aggiornamento anche le letture di quello strumento
arrivano alla camera unica: le densità di due fermentatori si alternano. Lo segnala l'allarme
«più densimetri»; il rimedio è scegliere il densimetro in Impostazioni.

### Misure
79/79 test nativi. `d1_mini`: RAM statica **48.592 B** (−368 rispetto alla 3.0.0 a una
camera), flash **723.016 B** (−4.812), UI gzip 43.746 B (−1.812). Heap sul dispositivo da
misurare al collaudo. Revisione indipendente del diff: nessun difetto critico o maggiore;
i cinque rilievi minori sono corretti in questa stessa versione.

## 3.0.0 — due camere di fermentazione indipendenti

**Prima versione stabile a due camere.** Il salto di major non è cosmetico: la 2.x governava
un fermentatore, la 3.0 ne governa `N` (oggi 2) con setpoint, profilo, sonde, uscite,
densimetro e stato propri, e con le protezioni riformulate **per camera** invece che per
dispositivo. Le sezioni `2.24.0-rc1` e `-rc2` più sotto sono le sue release candidate e
restano leggibili come cronaca del percorso — in particolare la rc2 documenta il guasto di
heap trovato sull'hardware reale.

Questa release aggiunge l'ultimo pezzo mancante: **un densimetro per camera (D7)**.

### Il densimetro, e perché il difetto era più profondo di come l'avevo descritto

Con due camere il densimetro era rimasto **uno solo, di dispositivo**: `main.cpp` passava lo
stesso `GravityTracker` al motore di profilo di entrambe. Uno step a densità sulla camera 2
avrebbe letto lo strumento della camera 1.

Instradare la lettura per camera **non bastava**, e la ricognizione avversariale lo ha
dimostrato prima che scrivessi codice. `ProfileEngine::stepDone` valuta il **timeout dello
step prima della validità del dato** — scelta deliberata, documentata e coperta da un test:
un densimetro incrostato non deve tenere in ostaggio il profilo. Ma **tutti e nove** gli step
a densità della libreria reale hanno un limite fra 2 e 7 giorni. Una camera senza densimetro
non si sarebbe bloccata: avrebbe aspettato qualche giorno e poi sarebbe avanzata comunque —
nei profili reali, al cold crash — **senza aver mai misurato nulla e senza un allarme**,
perché `alarmGravity` richiede `densimetroVisto || senzaLimite`, entrambi falsi in quel caso.
Stesso esito del difetto originale, solo posticipato.

### Cosa cambia

- **Un `GravityTracker` per camera** (44 B ciascuno) e allarme batteria per camera: avviare un
  profilo sulla camera B non spegne più l'allarme dello strumento di A.
- **Instradamento a una sola camera per lettura, per costruzione.** Tre regole esplicite:
  lettura anonima → camera 1; nome rivendicato → quella camera e nessun'altra; nome non
  rivendicato → camera 1 solo se è in «accetta chiunque». La prima regola non è un residuo:
  il **Tilt inoltrato da Home Assistant** (`docs/HOME_ASSISTANT.md` §7) manda un payload
  **senza `name`**, e un instradamento per solo nome lo avrebbe scartato in silenzio.
- **Taratura (`correction`) per camera.** È l'offset di *uno strumento*, non del sistema:
  applicare quello del primo alle letture del secondo falsifica la densità dentro
  `GravityTracker::update`, cioè a monte di ogni allarme e di ogni confronto del motore.
  Due iSpindel tarati a 0,003 di distanza bastano a far avanzare uno step al valore sbagliato.
- **Blocco vero, su due livelli.** `TickInput::gravitySourceMissing` disabilita la
  scorciatoia del timeout quando la camera non ha *nessuna* sorgente — il timeout resta
  attivo quando lo strumento c'è ed è muto, che è il caso per cui esiste. E l'avvio di un
  profilo con step a densità su una camera senza densimetro viene **rifiutato con 409** e un
  messaggio che dice come rimediare: è l'unico momento in cui l'utente è presente, giorni
  prima del danno.
- **Riassegnazione a cotta in corso**: cambiando strumento il tracker si azzera. La lettura
  in pancia apparteneva al precedente, con la sua taratura.
- `alarmMultiGravity` ridefinito: l'ambiguità esiste solo se la camera 1 è in «accetta
  chiunque» mentre trasmettono più strumenti. Una camera senza densimetro non è ambigua — non
  riceve nulla — e segnalarla lì avrebbe dato un allarme permanente col testo sbagliato.
- **Interfaccia**: una riga per camera (densimetro + taratura). L'invio avviene solo se le
  righe sono state costruite, altrimenti un salvataggio a tendine vuote cancellerebbe le
  assegnazioni.

### Compatibilità

`chambers[i].densimetro` e `chambers[i].correzione` sono la fonte di verità; `ispindel.device`
e `ispindel.correction` restano scritti come **specchio della camera 1**, così un ritorno
indietro a un firmware precedente continua a funzionare. In lettura chambers[] ha la
precedenza, con guardia esplicita perché la sezione `ispindel` viene interpretata dopo.
La chiave si legge con `containsKey`, non con l'idioma `| default`: qui la stringa vuota è un
valore legittimo che **disassegna** lo strumento, e con `| default` non si sarebbe più potuto
togliere un densimetro assegnato per errore.

A `AX_N_CHAMBERS=1` le tre regole di instradamento si riducono esattamente al filtro della
2.23.2 e `gravitySourceMissing` è sempre falso: **comportamento identico**.

### Misure

85/85 test nativi (4 nuovi). `d1_mini` RAM 48.960 (+272), `d1_mini_2ch` RAM 51.376 (+444).
L'aumento a una camera **non è stato zero come avevo previsto**: su ESP8266 `.rodata` sta in
DRAM, e sono i letterali dei messaggi nuovi — il testo del 409 su tutti.

## 2.24.0-rc2 — margine di heap, con misure dal dispositivo

Prima release verificata **su hardware**. La rc1 girava sul dispositivo e ha rivelato un
guasto reale che nessuna analisi statica aveva colto.

### Il guasto e la sua cura

`GET /api/profiles/<id>` restituiva **a volte** un JSON troncato a metà. Non riproducibile a
comando, quindi facile da liquidare come casuale. Esponendo il **blocco contiguo massimo** in
`/api/status` è diventato un numero: oscillava fra **5.848 e 9.984 B**, mentre la lettura di un
profilo ne chiedeva **6.152 contigui**. La domanda cadeva *dentro* la banda dell'offerta —
ecco l'intermittenza. Non mancava memoria: mancava un **blocco**.

- **Documenti JSON dimensionati sul fabbisogno reale.** `kProfileDoc` è derivata dalla
  struttura (`JSON_OBJECT_SIZE`/`JSON_ARRAY_SIZE`), non arrotondata: **3.952 B** verificati
  compilando, contro i 6.144 di prima, che erano il doppio del necessario. Config a 4.096.
- **Rotte per camera servite da una tabella** invece che registrate una per una: le 16
  registrazioni `UriBraces` costavano ~2,9 KB di heap *permanente*. Gli URL non cambiano.

**Misure sul dispositivo, prima e dopo:**

| | heap libero | blocco contiguo max |
|---|---:|---:|
| 2.24.0-rc1, due camere | ~11.000 B | 5.848 – 9.984 B |
| **2.24.0-rc2** | **14.960 B** | **10.384 B** |

Prova: 30 letture del profilo più grande sotto stress deliberato (export CSV, config e lista
profili alternati per frammentare) → **30 riuscite, 0 troncamenti**.

Il margine è ora migliore di quello che il firmware a **una** camera aveva prima di questo
lavoro. **Nessuna funzione è stata sacrificata** per ottenerlo.

### Altro

- `/api/status` espone `heapFrag` e `heapMaxBlock`; il comando seriale `heap` e il log
  periodico mostrano il blocco massimo. Il totale libero da solo non spiega i guasti di
  allocazione, ed era il limite pratico nel diagnosticarli.
- Una camera **non ancora allestita** non segnala più «sonda guasta»: è una camera da
  configurare, non un guasto. Il gate di sicurezza resta intatto.
- Eliminando `UriBraces` dal percorso per camera sparisce per costruzione la trappola di
  `pathArg()` che causava un riavvio pre-autenticazione.

---

## 2.24.0-rc1 — da 1 a 2 camere di fermentazione

> **`-rc1` è deliberato.** Il codice è completo e verificato in compilazione, con test e
> analisi statica, ma **non è mai stato eseguito su hardware**: il dispositivo non è stato
> raggiungibile durante lo sviluppo. Le tre verifiche che mancano sono elencate in fondo.

### ⚠️ Prima di aggiornare — due punti non reversibili

1. **Il formato di `/state.json` cambia** (schema v2, per camera). Il firmware nuovo legge il
   vecchio formato e lo migra, ma al primo salvataggio riscrive in v2: **tornare al firmware
   precedente fa perdere lo stato**, perché il vecchio codice non riconosce v2 e ripartirebbe
   Off/disarmato. Per poter tornare indietro, salva prima una copia della LittleFS
   (`docs/FLASH.md`).
2. **Controlla che non ci sia una cotta in corso prima di flashare**: `GET /api/status` →
   `mode` (2 = profilo) e `profile.running`. Un flash riavvia il controllore e fa cadere i relè.

### Compatibilità con l'installazione esistente

A **una camera** il comportamento è invariato: stessi relè, stessi tempi, stesse API, stessi
topic ed entity_id MQTT, stesso disegno OLED. In dettaglio:

- `/config.json` e `/state.json` della v2.23.2 si caricano **intatti** (verificato confrontando
  meccanicamente ogni chiave con la baseline: nessuna persa).
- Il profilo attivo resta su `/profile.json` e il registro su `/log/d<giorno>.bin`: **una cotta
  in corso non ha nulla da convertire**.
- Gli endpoint storici (`/api/status`, `/api/setpoint`, …) continuano a funzionare e servono la
  camera 1. Le automazioni Home Assistant su `sensor.axhtherm_*` **non si rompono**: nome,
  `uniq_id` e topic della camera 1 sono byte-identici.
- Il comando seriale `arma <freddo> <caldo>` funziona ancora.

### Novità

- **N camere parametrico** (`AX_N_CHAMBERS`, flag di build). Environment dedicato
  `d1_mini_2ch`. Ogni camera ha sonda di controllo, setpoint, profilo, stato, uscite e
  armamento propri.
- **API per camera**: `/api/ch/<n>/…` accanto alle forme storiche (registrate solo con più di
  una camera, per non sprecare heap).
- **Frontend**: selettore di camera, visibile solo se il dispositivo ne gestisce più d'una.
- **OLED**: con più camere le alterna, 4 s ciascuna, con marcatore `C<n>`.
- **MQTT**: ramo per camera (`<base>/ch<N>/…`) e discovery Home Assistant con entità separate.
- **Persistenza versionata** (`"v"` in config e stato) con migrazione dal formato mono.
- **Etichetta di camera** configurabile, mostrata in `/api/status`.

### Sicurezza e robustezza

- **Failsafe su timer hardware** (novità): se il tick di controllo non gira da 60 s, un'ISR
  spegne i relè su GPIO diretti. Il web server gira dentro il loop cooperativo e il framework
  legge la rete con attese **senza tetto** che nutrono entrambi i watchdog: con il loop fermo i
  relè restavano latchati **a tempo indefinito**. Al ritorno la guardia compressore viene
  risincronizzata e lo stato di attuazione della pipeline azzerato, per non riprodurre un corto
  ciclo né far imparare agli stimatori un ciclo mai avvenuto.
  - **Non scatta sul lavoro legittimo**: export CSV, discovery MQTT e OTA rinviano il failsafe
    a ogni unità di lavoro completata. Si rinvia solo dove c'è **progresso dimostrato**, che è
    ciò che distingue un'operazione lunga da un loop parcheggiato in attesa.
  - **Dice quando non funziona**: una verifica di vitalità alza un allarme dedicato se l'ISR
    smette di arrivare (per esempio perché qualcuno si è preso timer1). Un dispositivo di
    sicurezza che fallisce in silenzio è peggio che non averlo.
  - **L'evento sopravvive al riavvio** (persistito in `/state.json`) e raggiunge MQTT: si
    perdeva proprio nel caso OTA per cui il failsafe esiste. Si prende in carico con un
    disarmo o un riarmo.
  - *Limite non correggibile: copre REL1-3. I relè su MCP23008 passano dall'I²C, che non è
    comandabile da un'ISR — è la ragione per cui l'allocazione raccomandata tiene **entrambi i
    compressori** su GPIO diretti.*
- **OTA**: keepalive TCP sull'upload (un peer morto viene rilevato in ~11 s invece che mai) e
  chiusura della connessione se non autorizzata.
- **`gravityCorrection` clampata** e **OG catturata automaticamente validata**: erano le due vie
  per cui un valore fuori scala finiva in `String(float,4)`, che sul core ESP8266 usa un buffer
  di stack da 33 byte senza controllo di lunghezza.
- **Panic pre-autenticazione corretto** (era presente anche nella baseline): `pathArg()` su un
  URI con graffe letterali eseguiva `assert(0 < 0)` → riavvio.
- **Unicità degli ID sonda** imposta: la stessa DS18B20 non può servire due camere.
- Guardie contro il troncamento silenzioso dei JSON su `/api/status` e sulla taratura.

### Correzioni

- Il **tetto di dimensione del registro** poteva disabilitarsi per sempre lasciando crescere il
  log fino a riempire la flash.
- La **capacità di lettura di `/state.json`** dipendeva dal firmware invece che dal file: un file
  a due camere non si caricava, e la soglia dipendeva dalla lunghezza del nome della cotta.
- **Sfasamento** della finestra PID e dell'avvio compressore fra camere, per non sommare gli
  spunti di corrente.
- Il **disarmo** garantisce di nuovo che cadano tutti i canali, non solo quelli mappati.

*Il dettaglio completo, con scenari di innesco, è in `fase4-passi1-4.md`, `fase4-passi5-7.md` e
`audit-sicurezza.md`, archiviati fuori dal repository (archivio delle revisioni, non pubblicato).*

### Cosa resta da verificare sul dispositivo

1. Che a una camera il comportamento sia **davvero** identico (finora provato per costruzione).
2. Che la **migrazione** di `/state.json` v1→v2 funzioni su un file reale.
3. Il **free heap effettivo**: è l'unico numero del cancello di verifica che è stato stimato e
   non misurato. Atteso ≥ 10 KB; sotto quella soglia il piano prevede di ripiegare su due
   controllori separati.
4. Il **failsafe su timer** non è mai scattato su hardware, e la sua verifica di vitalità
   non è mai stata osservata (al primo avvio il log deve restare muto: nessun allarme
   "failsafe non attivo").

Procedura completa nella guida a due camere, rimossa con la 3.1.0 (recuperabile dal tag `v3.0.0`).

---

## 2.23.2 e precedenti

Vedi `CORREZIONI.md` (archivio delle revisioni, non pubblicato) per la campagna di revisione avversariale che ha
portato il firmware alla qualità di produzione a una camera.
