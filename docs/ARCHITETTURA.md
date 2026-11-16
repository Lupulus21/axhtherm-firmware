# AxHTherm 2.5 — Architettura del nuovo firmware

Firmware autonomo per WEMOS D1 mini (ESP8266, 4 MB flash): controllo, configurazione e monitoraggio
locali via web UI servita dal dispositivo. Nessun server esterno obbligatorio; unico host esterno
possibile: pool NTP, disattivabile.

## Sicurezza (LAN domestica, HTTP in chiaro — modello accettato)
- **Login a sessione**: cookie `axs` = MD5(uiPassword+chipId), Max-Age 1 anno, HttpOnly/SameSite=Lax;
  `POST /api/login`, `/api/logout`. Fallback Basic auth `admin:uiPassword`
  per script. La pagina `/` è pubblica (solo UI, nessun dato); ogni `/api/*` è protetta; risposta 401
  in JSON senza `WWW-Authenticate` (niente popup del browser, in particolare nelle web-app iOS).
- **Password per scheda** (dalla 3.4.0): una scheda nuova o resettata genera al primo avvio due
  password casuali e distinte (interfaccia e rete di configurazione, 12 caratteri dal generatore
  hardware) e nomi con il chip ID (`axhtherm-XXXX`: rete `-setup`, `.local`, topic MQTT, nome in
  Home Assistant). Le scrive **una volta** sul seriale; le mostra l'OLED solo con la rete di
  configurazione attiva e finché l'utente non sceglie la sua password; il comando seriale
  `password` le rimostra, `password nuova` le rigenera, `reset fabbrica` (con conferma) è l'ultima
  risorsa senza rete (il cavo USB vale come chiave: chi ha la scheda in mano può comunque
  riflasharla). Una scheda già configurata tiene le sue. La schermata dell'OLED compare solo alla
  prima configurazione (nessuna rete di casa impostata), non con l'AP di emergenza.
- **Password attuale richiesta** per cambiare la password dell'interfaccia (`currentPassword` nel
  PUT) e per l'OTA (intestazione `X-Ax-Password`): un cookie di sessione da solo non basta.
  Password nuove da 8 caratteri in su; quelle corte delle configurazioni esistenti danno un allarme.
- **Password di fabbrica `axhtherm`** (schede configurate prima della 3.4.0, o configurazione
  illeggibile al boot, che riparte dai default con le uscite disabilitate): OTA disattivato e
  allarme "cambia la password". Non protegge da chi è sulla LAN e la conosce: va cambiata.
- **Reset di fabbrica dalla UI**: chiede la password attuale, perché dopo si rientra solo dal
  seriale o dal display. Nome della scheda: 1–26 caratteri `[a-z0-9-]` (diventa l'SSID `-setup`).
- **Tentativi sbagliati** (dalla 3.7.0): penalità per indirizzo IP condivisa da login, Basic auth,
  password attuale e OTA (`ax::RetryPenaltyByKey`, 4 indirizzi). Dal quinto errore di fila ogni
  tentativo da quell'indirizzo è rifiutato subito con 429 per una pausa che raddoppia (2 s … 60 s);
  si azzera con un accesso riuscito o dopo 15 minuti senza errori. Nessuna pausa dentro la
  richiesta: il loop di controllo non si ferma. Il codice dei densimetri ha una penalità sua. Il
  login accetta solo `Content-Type: application/json` (niente moduli da altri siti).
- **Nome della scheda nell'intestazione Host** (dalla 3.7.0, contro il DNS rebinding): pagina,
  `/api/*`, OTA e `/ispindel` rispondono solo se la scheda è chiamata con un IP, con il suo nome o
  con il suo nome seguito da `.local` o da un dominio dei router di casa (`.lan`, `.home`,
  `.home.arpa`, `.fritz.box`, `.localdomain`, `.internal`); altrimenti 403, senza il nome.
- **OTA**: autorizzazione ricalcolata a ogni upload e verificata di nuovo alla richiesta finale; il flag
  d'autorizzazione è azzerato su upload annullato (niente reboot non autenticato via richiesta vuota).
  Dalla 3.7.0 la scheda verifica mentre riceve la lunghezza e il CRC che `elf2bin.py` scrive
  dentro `firmware.bin` (`ax::FwImageCheck`) e conferma l'aggiornamento (`Update.end(true)`, che
  scrive il comando per il bootloader) solo se il file è intero e integro.
- **Riavvio proposto, non imposto** (dalla 3.7.0): le impostazioni attive solo al riavvio (rete,
  display, MQTT, orologio) vengono salvate e segnalate (`rebootNeeded` nella risposta,
  `rebootPending` in `/api/status`); la UI chiede se riavviare, avvisando se c'è una regolazione
  in corso.
- **/ispindel** resta senza login (è lo iSpindel a pubblicare), ma con **token opzionale** e **clamp di
  plausibilità** su densità/temperatura: una lettura falsa iniettata dalla LAN viene scartata e non può
  spostare l'avanzamento di uno step a densità.

Toolchain: PlatformIO, board `d1_mini`, core Arduino ESP8266 3.1.2 (platform espressif8266 4.2.1).
Il percorso del progetto non deve contenere spazi (PlatformIO).

---

## 1. Principi non negoziabili

1. **Uscite spente all'avvio e in ogni condizione di errore** (sonda di controllo assente/CRC/-127/85 °C,
   configurazione corrotta, RTC inaffidabile → si assume ritardo compressore NON trascorso).
2. **Mai caldo e freddo insieme** (interlock strutturale, non solo logico: un unico punto di applicazione
   delle uscite che impone la mutua esclusione).
3. **Mai blink software su D6/D7/D8** (condivisi con REL1–3). I LED riflettono di fatto lo stato dei relè.
4. **Uscite DISARMATE alla nascita** (flag persistito): l'automazione si arma solo dopo conferma del
   mapping relè→carico da parte dell'utente.
5. **Il loop di controllo non si blocca mai**: niente `delay()`, conversioni DS18B20 asincrone,
   rete e filesystem non devono mai fermare la regolazione; watchdog attivo e nutrito nel loop.
6. **Controllo funzionante senza WiFi**: la rete è un servizio accessorio.
7. **Persistenza solo su transizioni + checkpoint ogni 5 min**, mai nel loop; scritture atomiche
   (file temporaneo + verifica della dimensione + rename, che su LittleFS sovrascrive atomicamente).
8. **Salti d'orologio gestiti**: quando l'epoch salta (>60 s: sync NTP dopo un boot con RTC
   inaffidabile, o impostazione manuale) gli epoch àncora di profilo e guardia compressore
   vengono traslati dello stesso delta — i tempi trascorsi restano veri, niente fast-forward
   del profilo né azzeramento del riposo compressore. Al boot, inoltre: il compressore che
   risultava in marcia si considera "appena fermato" (riposo pieno), e uno stato salvato su
   timeline fittizia (`epochsTrusted=false`) viene ri-basato in modo conservativo.
9. **Guasto del driver uscite** (MCP23008 che non risponde con mapping su REL4–8): retry
   immediato + periodico, allarme dedicato e consenso alle uscite revocato. Nota: i carichi
   reali stanno comunque su REL1–3 (GPIO diretti, connettore RELAY).

> ### La premessa su cui poggia tutto il principio 1
>
> «Uscite spente» significa qualcosa solo se **bobina diseccitata = carico spento**, cioè se i
> contatti dei relè esterni sono **NO** (normalmente aperti). È l'assunzione di `safeInitEarly`,
> dell'ULN2803 non invertente, del reset dell'MCP via `WRST`, e del failsafe hardware che scrive
> `GPOC` sui pin relè per **togliere** corrente alle bobine quando il loop si pianta.
>
> Con contatti **NC** ognuna di queste protezioni si rovescerebbe, e il failsafe diventerebbe un
> **amplificatore di guasto**: durante uno stallo accenderebbe compressore e riscaldante insieme
> e li lascerebbe accesi fino al reset.
>
> **Verificato NO il 2026-09-02**, non per ispezione ma misurando 30 giorni di registro: le due
> uscite sono a riposo nel 93 % dei campioni (2426/2605) e in quel 93 % la camera sta a 28,4 °C
> di mediana — cioè ad ambiente — con sonda di controllo e ausiliaria concordi entro 0,77 °C.
> Se `REL3` fosse NC il frigo girerebbe quasi sempre; se `REL1` fosse NC il riscaldante
> scalderebbe quasi sempre e le due sonde divergerebbero. Entrambe le ipotesi sono refutate.
> Dettaglio nell'archivio delle revisioni (non pubblicato), D5.
>
> ⚠️ **Va ri-verificato se si ricablano i carichi o si cambiano i moduli relè.**

10. **Un attuatore che non attua è invisibile al controllo.** Il firmware rileva lo scostamento
    termico (>5 °C senza progresso per 3 h) e alza l'allarme, ma non può correggere un carico
    scollegato o un relè guasto — è successo il 25-26/08/2026 (prova a banco, cavo riscaldante
    staccato: 14 h di comando caldo con la temperatura in discesa). Per questo il collaudo
    prevede una **prova di accettazione degli attuatori prima di ogni cotta**, non solo al primo
    montaggio: `docs/COLLAUDO.md`, prova 18.

11. **Blocco del caldo su guasto del riscaldatore** (dalla 3.3.0, `HeaterGuard`): sonda ferma a
    caldo acceso, caldo acceso oltre il tempo massimo, salita rapida a caldo spento, freddo che
    lavora da ore senza effetto. Al rilevamento si toglie il consenso al **solo caldo** (il freddo
    resta consentito, perché è l'unico attuatore che contrasta un riscaldatore che non si spegne).
    Il blocco è salvato su flash, sopravvive ai riavvii e si toglie solo a mano (UI o seriale
    `sblocca`). Un relè con i contatti incollati il firmware non può aprirlo: dalla 3.10.0 non si
    chiede più la conferma di un termostato bimetallico (scelta dell'utente, 28/10/2026).

13. **Limiti a valle della regolazione** (dalla 3.6.0), tutti come divieti di un verso nella
    politica di attuazione della pipeline: aria della camera (freddo sospeso sotto −5 °C,
    caldo sospeso sopra setpoint + 10, configurabili, isteresi 3 °C); limiti assoluti sul mosto
    (caldo tolto oltre 42 °C, freddo sotto −3 °C); parametri di regolazione riportati in
    intervalli plausibili. Il relè del frigo incollato dà solo un allarme (`CoolWatch`,
    riferimento min(setpoint, mosto)); finché è attivo i rilevatori del riscaldatore non lo
    bloccano. Ruoli fissi delle sonde ausiliarie: la prima è l'aria della camera, la seconda
    la stanza.
    L'inversione caldo/freddo nel cablaggio NON si rileva in automatico: la sua firma coincide
    con quella di un SSR incollato, e sbagliare spegnerebbe il frigo che lo contrasta.

12. **Riavvii a catena → modalità ridotta** (dalla 3.5.0, `RebootInfo`): tre blocchi del firmware
    di fila (watchdog, eccezione o blocco software), ognuno entro 10 minuti dall'avvio, fanno partire
    la sessione senza MQTT, con un allarme; dopo 30 minuti stabili MQTT si riaccende da solo.
    Il controllo della temperatura non si spegne mai: si toglie il servizio opzionale più
    complesso, che è il sospettato più probabile.

---

## 2. Struttura dei moduli

Una sola camera di fermentazione per scheda (dalla 3.1.0; la versione a due camere è il tag
git `v3.0.0`).

```
include/
  hardware_map.h        — UNICA fonte dei pin/indirizzi; ogni riga con rif. schema o // UNVERIFIED
  ax_dimensions.h       — dimensionamento (una camera, 3 sonde)
  web_assets.h          — GENERATO a ogni build da tools/gzip_assets.py (non versionato)
  build_id.h            — GENERATO a ogni build da tools/build_id.py: commit e marcatore "-mod"
src/
  main.cpp              — setup/loop cooperativo, tick di controllo, failsafe su timer hardware,
                          comandi (web/MQTT/seriale), CLI seriale
  hal/
    OutputBus.h         — relè: REL1-3 su GPIO12/13/15, REL4-8 su MCP23008 (0x20); allOff(); test manuale
    Mcp23008.h          — driver dell'espansore I2C, con recupero
    ProbeBus.h          — OneWire su GPIO0: scan, conversione asincrona, CRC, offset, statistiche, fault
    RtcClock.h          — DS1307 0x68: validazione al boot (CH bit, data plausibile)
    At24c32.h           — EEPROM 4 KB del Tiny RTC (0x50): lettura e scrittura a pagine, ACK polling
  svc/
    AppConfig.h         — configurazione in RAM, con i default
    AppStatus.h         — fotografia dello stato per UI/OLED/MQTT, ricostruita ogni secondo
    ChamberCtx.h        — stato della camera: motore profilo, pipeline, allarmi, timer
    ConfigStore.*       — /config.json: lettura, scrittura atomica, validazione
    StateStore.*        — /state.json: stato runtime per la ripresa dopo un blackout
    ProfileStore.*      — /profile.json (profilo attivo) e libreria /profiles/p<id>.json
    CalibStore.*        — /calib.json: punti di calibrazione del densimetro
    DeviceRegistry.*    — /devices.json: densimetri già visti
    BootLog.*           — /boot.json: ultimi 8 riavvii (motivo, intento, durata della sessione precedente,
                          eccezione) e contatore totale
    DataLog.*           — registro binario /log/d<giorno>.bin, retention e tetto di dimensione
    FsCommit.h          — commit atomico (tmp + verifica + rename)
    HeapMon.h           — minimi di memoria dall'avvio (heap, blocco contiguo, stack, allocazioni fallite)
    LoopStats.h         — tick in ritardo e rinvii del failsafe dall'avvio
    RebootInfo.h        — memoria RTC: motivo dei riavvii voluti, durata della sessione, riavvii a catena
    TimeService.*       — epoch UTC da RTC/NTP, fuso e ora legale, salti d'orologio
    NetworkManager.*    — STA con retry; SoftAP di emergenza; mDNS
    WebApi.*            — REST JSON, login a sessione, UI gzippata, /ispindel, OTA
    MqttSvc.*           — MQTT opzionale + discovery Home Assistant
    OledUi.*            — display SSD1306/SH1106 opzionale: pagine stato, fermentazione, allarmi
    Alarms.*            — tabella unica degli allarmi (id, codice, testo OLED, priorità)
    EventLog.*          — registro degli ultimi 256 eventi nella EEPROM del Tiny RTC
lib/axcore/src/ax/      — C++ puro, ZERO dipendenze Arduino → unit test nativi
  ProfileEngine.*       — macchina a stati del profilo (12 step, 4 tipi, ripresa da epoch assoluti)
  ControlPipeline.h     — UNICO punto che decide caldo/freddo: controller → interlock → guardia
  HysteresisController.h, PidController.h, PeakEstimator.h (spegnimento predittivo)
  CompressorGuard.h     — riposo minimo e marcia minima del compressore, su epoch persistiti
  Interlock.h           — arbitro caldo/freddo con tempo morto
  SafetyMonitor.h       — aggrega i fault e decide il consenso alle uscite
  HeaterGuard.h         — rilevatori di guasto del riscaldatore (decidono il blocco del caldo)
  CoolWatch.h           — relè del frigo incollato (solo allarme)
  ProbeFault.h          — tipo di guasto di una lettura DS18B20 (scratchpad, CRC)
  RetryPenalty.h        — penalità sui tentativi sbagliati, per indirizzo IP
  FwImageCheck.h        — verifica di lunghezza e CRC del firmware ricevuto via OTA
  AirLimit.h            — limiti sull'aria della camera, riferiti al setpoint
  SlopeEstimator.h      — pendenza del mosto su 20 minuti e tempo d'arrivo al setpoint
  CrashMonitor.h        — discesa del cold crash nelle ultime 6 ore: stima d'arrivo, discesa lenta
  GravityTracker.h      — ultima lettura del densimetro + staleness
  MinMaxGroup.h         — riduzione dei punti del grafico che conserva minimi e massimi
  EventRing.h           — anello di record con sequenza e CRC su memoria a pagine; antirimbalzo allarmi
  GravityTrend.h        — andamento della densità (medie orarie, pendenza 24/48 h), conversioni SG/°P
  FanControl.h          — ventola: segue caldo e freddo con post-funzionamento, ricircolo, sempre accesa
  TimeUtil.h, Types.h
data-src/               — sorgente della web UI (index.html + icona) → gzip → PROGMEM
test/test_core/         — unit test nativi di lib/axcore (pio test -e native)
```

Dipendenze (in `platformio.ini`): ArduinoJson 6.x, OneWire (Stoffregen), DallasTemperature,
ThingPulse esp8266-oled-ssd1306 (SSD1306 e SH1106), PubSubClient. PID e isteresi scritti in casa
in `lib/axcore` (poche decine di righe, unit-testate). LittleFS, mDNS e SNTP dal core.

---

## 3. Macchina a stati del profilo

Modalità di marcia (mutuamente esclusive): `OFF` (uscite ferme), `THERMOSTAT` (setpoint fisso, nessun
profilo), `PROFILE` (macchina a stati sotto).

```
IDLE ──start──▶ RUNNING(step i) ──condizione di fine step──▶ RUNNING(step i+1) … ──ultimo step──▶ COMPLETED
                 │  ▲                                                                   
              pause  resume            comandi: stop (→IDLE), skip (→step i+1),        
                 ▼  │                  setpoint override (solo step corrente)          
               PAUSED                                                                  
```

Tipi di step e condizione di avanzamento (tempo sempre da **epoch RTC assoluti**, non da contatori):

| Tipo | Parametri | Setpoint istantaneo | Avanza quando |
|---|---|---|---|
| `fixed` | target, durata | target | `now ≥ stepStart + durata` |
| `ramp_time` | da, a, durata | interpolazione lineare su `now − stepStart` | `now ≥ stepStart + durata` |
| `ramp_rate` | a, °C/h | partenza = temperatura misurata a inizio step, poi ±rate·Δt, saturato al target | setpoint ha raggiunto il target **e** temperatura entro tolleranza |
| `gravity` | temperatura, condizione, timeout | temperatura fissa | condizione a densità soddisfatta **oppure** timeout scaduto (0 = nessuno). Condizioni (v2.12): **soglia** (densità ≤ target), **% attenuazione** (≥ target, da OG catturata; senza OG attende), **stabile** (entro ±banda per N ore, finestra persistita). Mai su dato assente o stantio (salvo timeout). |

Ogni step porta anche la **politica di attuazione** `allowHeat`/`allowCool` (v2.12): free-rise
(salita naturale con solo tetto frigo), salite "mai raffreddare" (saison), discese solo-frigo.
Applicata in `ControlPipeline` a valle del controller, a monte di interlock e guardia.
Freeze-guard: setpoint di step validato a ≥ -1 °C. Max 12 step. Libreria di modelli pronti
nella UI ("Nuovo da modello"): numeri e fonti in docs/PROFILI.md.

Ripresa dopo reset/blackout: al boot si ricarica `/state.json`; con RTC valido si ricalcola la posizione
dallo `stepStartEpoch` (le rampe a tempo riprendono dal punto corrispondente all'ora attuale; step a tempo
già scaduti avanzano). Con RTC inaffidabile: allarme, il profilo riprende in modo conservativo (setpoint
dello step corrente, rampe congelate al valore di ripresa) e il ritardo compressore si assume NON trascorso.

Fine profilo (`COMPLETED`): si mantiene il setpoint dell'ultimo step a oltranza.

Extra ereditati dall'originale: **partenza ritardata** del profilo (`delayedStartAt`, epoch assoluto —
presente nel binario 2.5.57) come opzione di start. Copertura dei comandi che l'originale riceveva dal
cloud (`toggle_run, go_to_next_step, set_current_target_temperature, set_running, get_steps, get_config,
save_config, reset, format_flash, update_firmware, rtc`): tutti presenti come endpoint REST locali in §7
(format flash → funzione "reset di fabbrica" in diagnostica; rtc → impostazione ora da UI).

---

## 4. Loop di controllo (tick 1 s)

```
tick_1s:
  ProbeBus.poll()            → temperature (conversione lanciata al tick precedente, non blocca)
  setpoint = ProfileEngine/Thermostat
  demand   = Controller(setpoint, T_controllo)     ← isteresi o PID (per-verso, vedi sotto)
  demand   = CompressorGuard.filter(demand, now)   ← riposo minimo sul freddo
  demand   = Interlock.arbitrate(demand)           ← mai heat+cool; priorità allo stato sicuro
  if (SafetyMonitor.fault() || !armed) demand = ALL_OFF
  OutputBus.apply(demand)                          ← unico punto che tocca i relè
```

- **Isteresi**: banda `tolleranza` attorno al setpoint, per verso (freddo eccitato sopra
  `set+toll`, spento sotto `set`; caldo speculare). Anti-chattering: tempo minimo di stato.
- **Spegnimento predittivo** (stile BrewPi, disattivabile da config): mentre l'attuatore è in
  marcia si stima la deriva di temperatura che seguirà lo spegnimento (inerzia del frigo) e si
  spegne in anticipo così che la deriva atterri sul setpoint invece di superarlo. Il coefficiente
  (°C per ora di marcia) si auto-apprende osservando il picco reale dopo ogni ciclo (EMA sulla
  deriva misurata — adattamento rispetto ai fattori ±20% di BrewPi) ed è persistito in
  `/state.json`. Attivo sul freddo sempre, sul caldo solo in modalità isteresi (col PID ci pensa
  la finestra). Paletti sull'apprendimento (un ciclo = al più una lezione): campione clampato a
  10 °C/h prima dell'EMA, coefficiente mosso al massimo di 1 °C/h per ciclo, osservazioni
  interrotte da una nuova marcia scartate sotto 180 s, campione scartato se l'attuatore opposto
  entra in funzione durante l'osservazione (deriva non più passiva). Vincoli di sicurezza: può
  solo ANTICIPARE uno spegnimento (mai prolungare la marcia né accendere), non agisce sotto
  120 s di marcia, deriva applicata max 2 °C, rispetta il tempo minimo di stato dell'isteresi;
  con coefficiente 0 (stato iniziale) è inerte e il comportamento è identico all'isteresi pura.
  Lo spegnimento anticipato azzera il latch dell'isteresi: la riaccensione richiede di nuovo la
  soglia piena `set+toll` (niente cicli a metà banda), sempre filtrata dalla guardia compressore.
  Gli stimatori osservano sempre lo stato fisico del relè (anche a predittivo spento o caldo in
  PID): i flag di configurazione decidono solo se onorare lo stop anticipato.
- **PID** (dal manuale originale): uscita 0–100 % applicata con finestra di time-proportioning
  (`windowSize` ms); sotto `set − sogliaBassa` piena potenza; sopra `set + sogliaAlta` tutto spento.
  PID applicato al riscaldamento; sul freddo (compressore) la scelta di default è isteresi +
  CompressorGuard (un compressore non si modula a finestre brevi) — configurabile.
- **CompressorGuard**: persiste l'epoch di ogni spegnimento/accensione del relè freddo; nuova accensione
  solo se `now − lastCompressorOff ≥ ritardo` (default dal manuale originale; configurabile). Vale anche
  attraverso i riavvii (epoch assoluti su LittleFS).
- **SafetyMonitor**: fault = sonda di controllo assente/CRC/85 °C spurio/-127, config corrotta,
  temperatura fuori range fisico. Fault ⇒ uscite spente, allarme in UI/OLED, il profilo resta in marcia
  ("regola appena la sonda torna") ma senza attuare.

Scheduler cooperativo in `main.cpp`: tick 1 s (controllo), 750 ms (conversione sonde), 5 min (checkpoint
+ campione log), eventi (web server), watchdog nutrito ad ogni giro; log del free heap ogni minuto.

---

## 5. Persistenza su LittleFS e stima di usura

| File | Contenuto | Quando si scrive |
|---|---|---|
| `/config.json` | configurazione completa (§6) | solo su salvataggio dall'utente |
| `/profile.json` | profilo attivo (copia in esecuzione) | avvio profilo, modifica dall'utente |
| `/profiles/p<id>.json` | libreria (fino a 20 ricette) | solo su modifica dall'utente |
| `/state.json` | schema v2: modalità, step, epoch del motore e della guardia compressore, mapping e armamento, OG, coefficienti predittivi | transizioni + checkpoint 5 min |
| `/log/d<giorno>.bin` | campioni da 16 B (epoch, 3 temp, setpoint, flag, densità, batteria) | 1 append ogni `logSampleMin` |
| `/calib.json`, `/devices.json`, `/boot.json` | calibrazione densimetro, densimetri visti, storico riavvii | su evento |

Stima usura: a profilo attivo ≈ 288 checkpoint + 288 campioni al giorno ≈ 210 k scritture/anno da ~16–200 B.
LittleFS fa wear-leveling dinamico sui blocchi liberi: con ~2 MB liberi (≈ 500 settori da 4 KB) e
100 k cicli di erase per settore, il budget è ≈ 50 M scritture-settore → **>10 anni** anche nel caso
peggiore.

---

## 6. Schema di configurazione (`/config.json`, versionato)

Lo schema reale è quello scritto da `ConfigStore::toJson` (`src/svc/ConfigStore.cpp`); `GET /api/config`
lo restituisce con i segreti mascherati. Sezioni: `system`, `network`, `time`, `probes`, `control`,
`chambers` (densimetro e taratura; la sezione `ispindel` ne è lo specchio), `ispindel`, `display`,
`log`, `mqtt`, `ota`, `safety` (tempi dei rilevatori), più `configRev` per rifiutare i salvataggi concorrenti (409).

Config corrotta/illeggibile ⇒ default di fabbrica + `armed=false` + allarme (mai attuare con config dubbia).

---

## 7. API REST (JSON; stessa origine della UI)

| Metodo e path | Funzione |
|---|---|
| `GET /` · `GET /icon.png` · immagini di avvio iOS | UI gzippata e icone (senza login) |
| `POST /api/login` · `POST /api/logout` | sessione (cookie `axs`) |
| `GET /api/status` | stato completo: temperature, setpoint, uscite, modalità, step, densità (+età), allarmi, heap (libero, frammentazione, blocco massimo e minimi dall'avvio), build, rssi, ora |
| `GET/PUT /api/config` | configurazione (PUT valida, salva e applica; rete/display/MQTT/ora al riavvio, che la UI propone) |
| `GET/PUT /api/profile` | profilo attivo |
| `GET /api/profiles` · `GET/PUT/DELETE /api/profiles/{id}` | libreria profili |
| `POST /api/profile/start\|stop\|pause\|resume\|skip` | comandi profilo |
| `POST /api/profile/coldcrash` | genera e avvia un cold crash |
| `POST /api/setpoint` | override del setpoint dello step corrente |
| `POST /api/thermostat` | `{on, setpoint}` — modalità termostato |
| `POST /api/gravity/og` | OG manuale (0 = ricattura automatica) |
| `POST /api/outputs/arm` · `POST /api/outputs/disarm` | conferma mapping e arma; disarma (spegne tutti gli 8 canali) |
| `POST /api/safety/reset` | sblocca il caldo dopo un blocco di sicurezza |
| `POST /ispindel` | ingest densimetro (Generic HTTP JSON: `name, ID, temperature, gravity, battery, RSSI…`), token opzionale |
| `GET/POST/DELETE /api/calib` | calibrazione del densimetro |
| `GET /api/log` · `GET /api/log.csv` | dati per il grafico (minimi e massimi per gruppo) / export CSV a pezzi |
| `GET /api/events?before=<seq>&n=<1..40>` | registro eventi, i più recenti per primi (codici: `svc/EventLog.h`) |
| `GET /api/wifi/scan` | reti visibili |
| `GET /api/diag/i2c` · `GET /api/diag/onewire` | scan bus |
| `GET /api/probes` · `POST /api/probes/calibrate` · `POST /api/probes/reset` | diagnostica e taratura sonde |
| `POST /api/diag/relay` · `POST /api/diag/testmode` | test manuale relè (**solo a sistema disarmato**) e sua chiusura |
| `POST /api/time` | imposta l'ora |
| `POST /api/reboot` · `POST /api/factory_reset` | riavvio, reset di fabbrica |
| `POST /api/update` | upload .bin OTA (se abilitato; password attuale in `X-Ax-Password`; lunghezza e CRC del file verificati) |

Web UI responsive (vanilla JS + grafico canvas leggero, niente framework): dashboard con grafico,
editor profilo, configurazione, diagnostica. mDNS: `http://<nome>.local` (`axhtherm-XXXX` sulle
schede configurate dalla 3.4.0, `axhtherm` su quelle precedenti).

---

## 8. Flash, asset e OTA

- Layout **4M2M** (default `d1_mini`): 1 MB sketch + 1 MB riservato OTA + ~2 MB LittleFS.
  Lo sketch resta sotto la metà della flash per costruzione; l'area dati (config/stato/log) **non viene
  toccata** dagli aggiornamenti.
- Asset UI **gzippati in PROGMEM** (raccomandato, vedi D10): un solo `.bin` aggiorna firmware *e* UI,
  nessun doppio upload, LittleFS intatto. Build step PlatformIO che comprime `data-src/` in header C.
- Flash reale 4 MB: il dump integrale del firmware originale (15/08/2026) è di 4.194.304 byte.

Budget heap: misurati 14.960 B liberi a due camere (2.24.0-rc2). A una camera (3.13.0, scheda
PCBWay, 03/11/2026): ~25 KB liberi a riposo, minimo 12,6 KB dopo l'uso della UI; stack libero
minimo 880 B, alzato nella 3.14.0 togliendo dal frame di `loop()` le parti incorporate. Server sincrono (niente ESPAsyncWebServer), documenti JSON dimensionati sul fabbisogno
reale, risposte in streaming, asset serviti da PROGMEM a chunk. Heap libero, frammentazione e blocco
contiguo massimo sono in `/api/status` e nel log seriale ogni minuto.
