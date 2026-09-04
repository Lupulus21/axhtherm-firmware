# AxHTherm 2.5 — Architettura del nuovo firmware

Firmware autonomo per WEMOS D1 mini (ESP8266, 4 MB flash): controllo, configurazione e monitoraggio
locali via web UI servita dal dispositivo. Nessun server esterno obbligatorio; unico host esterno
possibile: pool NTP, disattivabile.

## Sicurezza (LAN domestica, HTTP in chiaro — modello accettato)
- **Login a sessione**: cookie `axs` = MD5(uiPassword+chipId), Max-Age 1 anno, HttpOnly/SameSite=Lax;
  `POST /api/login` (delay 300 ms su errore), `/api/logout`. Fallback Basic auth `admin:uiPassword`
  per script. La pagina `/` è pubblica (solo UI, nessun dato); ogni `/api/*` è protetta; risposta 401
  in JSON senza `WWW-Authenticate` (niente popup del browser, in particolare nelle web-app iOS).
- **Password di default**: finché `uiPassword` è `axhtherm` l'OTA è disattivato e in dashboard compare
  un allarme "cambia la password" — chiude il takeover-via-OTA con credenziale nota.
- **OTA**: autorizzazione ricalcolata a ogni upload e verificata di nuovo alla richiesta finale; il flag
  d'autorizzazione è azzerato su upload annullato (niente reboot non autenticato via richiesta vuota).
- **/ispindel** resta senza login (è lo iSpindel a pubblicare), ma con **token opzionale** e **clamp di
  plausibilità** su densità/temperatura: una lettura falsa iniettata dalla LAN viene scartata e non può
  spostare l'avanzamento di uno step a densità.

Toolchain: PlatformIO, board `d1_mini`, core Arduino ESP8266 3.1.2 (platform espressif8266 4.2.1).
Progetto in `~/dev/axhtherm25` (path senza spazi, fuori iCloud).

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
   (file temporaneo + rename, che su LittleFS sovrascrive atomicamente) e saltate se il contenuto non cambia.
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
> Dettaglio in `docs/analysis/domande-aperte.md`, D5.
>
> ⚠️ **Va ri-verificato se si ricablano i carichi o si cambiano i moduli relè.**

10. **Un attuatore che non attua è invisibile al controllo.** Il firmware rileva lo scostamento
    termico (>5 °C senza progresso per 3 h) e alza l'allarme, ma non può correggere un carico
    scollegato o un relè guasto — è successo il 25-26/08/2026 (prova a banco, cavo riscaldante
    staccato: 14 h di comando caldo con la temperatura in discesa). Per questo il collaudo
    prevede una **prova di accettazione degli attuatori prima di ogni cotta**, non solo al primo
    montaggio: `docs/COLLAUDO_2CAMERE.md`, sezione C prova 8.

---

## 2. Struttura dei moduli

```
src/
  main.cpp              — setup/loop cooperativo, scheduler dei tick
  hardware_map.h        — UNICA fonte dei pin/indirizzi; ogni riga con rif. schema o // UNVERIFIED
  hal/
    OutputBus.*         — relè: REL1-3 su GPIO12/13/15, REL4-8 su MCP23008 (0x20); allOff(); arming gate
    ProbeBus.*          — OneWire su GPIO0: scan, conversione asincrona (750 ms), CRC, offset, fault
    RtcClock.*          — DS1307 0x68: validazione al boot (CH bit, data plausibile, monotonia vs
                          timestamp persistito), fallback millis(), SNTP opzionale
    Oled.*              — opzionale; driver selezionabile SSD1306/SH1106 @0x3C (default SSD1306)
  core/                 — C++ puro, ZERO dipendenze Arduino → unit test nativi
    ProfileEngine.*     — macchina a stati profilo (5 step, 4 tipi, ripresa da epoch assoluti)
    HysteresisController.*
    PidController.*     — P/I/D, finestra time-proportioning, soglie di piena potenza, anti-windup
    CompressorGuard.*   — riposo minimo compressore su timestamp assoluti persistiti
    Interlock.*         — arbitro unico caldo/freddo + stato sicuro
    SafetyMonitor.*     — aggrega i fault e impone lo stato sicuro
    GravityTracker.*    — ultima lettura iSpindel valida + staleness (default 60 min)
  svc/
    ConfigStore.*       — /config.json su LittleFS, versione schema, default, validazione
    StateStore.*        — /state.json: indice step, epoch inizio step, ultimo intervento compressore,
                          flag armed, modalità termostato
    ProfileStore.*      — /profile.json: definizione degli step
    DataLog.*           — log circolare binario su LittleFS, 1 campione/5 min, retention configurabile
    NetworkManager.*    — STA con retry a backoff; SoftAP 192.168.4.1 (con password) dopo N min di STA giù,
                          senza mai smettere di ritentare la STA; mDNS
    WebApi.*            — REST JSON su ESP8266WebServer + asset UI gzippati
    IspindelIngest.*    — POST /ispindel (formato Generic HTTP JSON di iSpindel)
    TimeService.*       — epoch UTC da RTC, timezone/DST per la visualizzazione, SNTP opzionale
    Diagnostics.*       — scan I2C, scan OneWire, test manuale relè (solo a sistema disarmato), free heap
    OtaUpdate.*         — opzionale: upload .bin dalla web UI
    MqttClient.*        — opzionale: MQTT + Home Assistant discovery
data-src/               — sorgenti web UI (html/css/js) → gzip → PROGMEM (vedi §8)
test/                   — unit test nativi (pio test -e native)
docs/
```

Dipendenze (mature, pinnate in `platformio.ini`): ArduinoJson 6.x, OneWire (Stoffregen),
DallasTemperature (async), RTClib (Adafruit), ThingPulse esp8266-oled-ssd1306 (supporta SSD1306 *e* SH1106),
PubSubClient (solo se MQTT attivo). PID e isteresi scritti in casa in `core/` (poche decine di righe,
completamente unit-testate: più verificabile di una libreria esterna). LittleFS, mDNS, DNSServer, SNTP dal core.

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
Freeze-guard: setpoint di step validato a ≥ -1 °C. Max 12 step (era 8). Libreria di 13 modelli
pronti nella UI ("Nuovo da modello"): numeri e fonti in docs/PROFILI.md.

Ripresa dopo reset/blackout: al boot si ricarica `/state.json`; con RTC valido si ricalcola la posizione
dallo `stepStartEpoch` (le rampe a tempo riprendono dal punto corrispondente all'ora attuale; step a tempo
già scaduti avanzano). Con RTC inaffidabile: allarme, il profilo riprende in modo conservativo (setpoint
dello step corrente, rampe congelate al valore di ripresa) e il ritardo compressore si assume NON trascorso.

Fine profilo (`COMPLETED`): comportamento configurabile — vedi domanda D7.

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
| `/config.json` | configurazione completa (schema §6) | solo su salvataggio dall'utente |
| `/profile.json` | definizione step | solo su modifica dall'utente |
| `/state.json` | modalità, indice step, `stepStartEpoch`, epoch ultimo intervento compressore, flag `armed`, setpoint termostato | transizioni + checkpoint 5 min (skippato se invariato) |
| `/log.bin` | ring buffer campioni (16 B: epoch, 3 temp, setpoint, uscite, densità) | 1 append/5 min |

Stima usura: a profilo attivo ≈ 288 checkpoint + 288 campioni al giorno ≈ 210 k scritture/anno da ~16–200 B.
LittleFS fa wear-leveling dinamico sui blocchi liberi: con ~2 MB liberi (≈ 500 settori da 4 KB) e
100 k cicli di erase per settore, il budget è ≈ 50 M scritture-settore → **>10 anni** anche nel caso
peggiore. I checkpoint invarianti non scrivono affatto (confronto prima di scrivere).

---

## 6. Schema di configurazione (`/config.json`, versionato)

```jsonc
{ "version": 1,
  "system":  { "hostname": "axhtherm", "uiPassword": "…", "language": "it" },
  "network": { "ssid": "…", "password": "…", "dhcp": true, "ip": "", "mask": "", "gw": "", "dns": "",
               "softApPassword": "…", "softApAfterMin": 5 },
  "time":    { "ntpEnabled": true, "ntpServer": "pool.ntp.org", "tz": "CET-1CEST,M3.5.0,M10.5.0/3" },
  "probes":  { "control": {"id": "28FF…", "offset": 0.0},
               "aux":     [{"id": "…", "offset": 0.0, "label": "camera"},
                           {"id": "…", "offset": 0.0, "label": "ambiente"}] },
  "control": { "mode": "hysteresis|pid", "predictive": true, "tolerance": 0.5,
               "pid": { "kp": 2, "ki": 1, "kd": 5, "windowMs": 5000,
                        "fullPowerBelow": 5.0, "cutAbove": 0.5 },
               "compressorRestMin": 5, "minStateSec": 60 },
  "outputs": { "armed": false,
               "map": { "cool": "REL1|REL2|REL3|none", "heat": "…", "aux": "…" } },
  "ispindel":{ "staleMin": 60, "batteryAlert": 3.3, "gravityUnit": "sg|plato", "offset": 0.0 },
  "display": { "enabled": true, "driver": "ssd1306|sh1106", "address": "0x3C", "flip": false },
  "log":     { "sampleMin": 5, "retentionDays": 14 },
  "mqtt":    { "enabled": false, "host": "", "port": 1883, "user": "", "pass": "", "haDiscovery": true },
  "ota":     { "enabled": true, "password": "…" } }
```

Config corrotta/illeggibile ⇒ default di fabbrica + `armed=false` + allarme (mai attuare con config dubbia).

---

## 7. API REST (JSON; stessa origine della UI)

| Metodo e path | Funzione |
|---|---|
| `GET /api/status` | stato completo: temperature, setpoint, uscite, modalità, step, densità (+età), allarmi, heap, rssi, ora |
| `GET/PUT /api/config` | configurazione (PUT valida e riavvia i servizi toccati) |
| `GET/PUT /api/profile` | definizione step |
| `POST /api/profile/start|stop|pause|resume|skip` | comandi profilo |
| `POST /api/setpoint` | override del setpoint dello step corrente |
| `POST /api/thermostat` | `{on, setpoint}` — modalità termostato |
| `POST /api/outputs/arm` | conferma mapping e arma; `POST /api/outputs/disarm` |
| `POST /ispindel` | ingest iSpindel (Generic HTTP JSON: `name, ID, temperature, gravity, battery, RSSI…`) — path storico mantenuto |
| `GET /api/log?from=&to=` / `GET /api/log.csv` | dati per il grafico / export CSV |
| `GET /api/diag/i2c` · `GET /api/diag/onewire` | scan bus |
| `POST /api/diag/relay` | `{n, on}` test manuale — **solo a sistema disarmato/fermo** |
| `POST /api/update` | upload .bin OTA (se abilitato; protetto da password) |

Web UI responsive (vanilla JS + grafico canvas leggero, niente framework): dashboard con grafico,
editor profilo, configurazione, diagnostica. mDNS: `http://axhtherm.local`.

---

## 8. Flash, asset e OTA

- Layout **4M2M** (default `d1_mini`): 1 MB sketch + 1 MB riservato OTA + ~2 MB LittleFS.
  Lo sketch resta sotto la metà della flash per costruzione; l'area dati (config/stato/log) **non viene
  toccata** dagli aggiornamenti.
- Asset UI **gzippati in PROGMEM** (raccomandato, vedi D10): un solo `.bin` aggiorna firmware *e* UI,
  nessun doppio upload, LittleFS intatto. Build step PlatformIO che comprime `data-src/` in header C.
- `esptool flash_id` in Fase 3 confermerà i 4 MB reali (il tool di installazione originale usava 32 Mbit → coerente).

Budget heap (~40 KB liberi attesi): server sync (niente ESPAsyncWebServer), JSON con buffer statici
dimensionati, risposte in streaming, asset serviti da PROGMEM a chunk. Free heap loggato e in `/api/status`.

---

## 9. Milestone (ognuno compila con `pio run`)

- **M1 — HAL + diagnostica**: hardware_map, OutputBus (disarmato), ProbeBus, RtcClock, scan I2C/OneWire,
  test relè da seriale, watchdog, heap log. → *primo collaudo al banco senza carichi*.
- **M2 — Motore di controllo**: `core/` puro + test nativi (`pio test -e native`): rampe (tempo/rate),
  avanzamento step, ripresa da epoch con riavvio simulato, interlock, CompressorGuard attraverso riavvio,
  sonda guasta ⇒ uscite spente, densità stantia ⇒ mai avanzare.
- **M3 — Rete + persistenza + API**: ConfigStore/StateStore/ProfileStore/DataLog, NetworkManager
  (STA/SoftAP/mDNS), TimeService, API REST complete, iSpindel ingest.
- **M4 — UI + opzionali**: web UI gzippata, OLED (driver selezionabile), OTA da UI, MQTT/HA, export CSV.

Definizione di fatto come da mandato: `pio run` pulito con report flash/RAM, test nativi verdi,
`grep -ri iobrewing src/ lib/ include/ data*/` vuoto, elenco host esterni = solo NTP disattivabile,
ogni assunzione tracciata con opzione di configurazione.

---

## 10. Domande aperte (decisioni dell'utente — unico stop di Fase 1)

- **D1 — Carichi su REL1–3**: quale polo del connettore RELAY pilota il frigo e quale il riscaldatore
  (e il terzo?). Se non lo sai a memoria va bene: si verifica al banco in M1; serve solo sapere *che carichi esistono* (frigo + riscaldatore? solo frigo?).
- **D2 — Opzionali da includere**: MQTT/Home Assistant? OTA via upload dalla web UI? (entrambi disattivabili da config; consiglio: OTA sì, MQTT sì ma spento di default).
- **D3 — Lingua della UI**: solo italiano, solo inglese, o bilingue (consiglio: solo italiano, più semplice e leggero).
- **D4 — OLED**: è fisicamente collegato un display? Se sì, sai già se SSD1306 o SH1106? (default: supporto attivo, SSD1306@0x3C, cambiabile da config).
- **D5 — Password**: password per web UI/OTA e per il SoftAP di emergenza (l'originale usava `AXHTH`/`DEADBEEF`). Dimmi i valori o lascio placeholder da cambiare al primo avvio.
- **D6 — Timezone**: Europe/Rome con ora legale automatica? (default proposto: sì, TZ POSIX `CET-1CEST,M3.5.0,M10.5.0/3`).
- **D7 — Fine profilo**: all'ultimo step completato: (a) mantieni l'ultimo setpoint a oltranza, (b) spegni le uscite, (c) passa in termostato a un setpoint configurato. (Consiglio: (a), è il comportamento più sicuro per una fermentazione.)
- **D8 — Log**: campione ogni 5 min e retention 14 giorni vanno bene? (configurabili comunque).
- **D9 — Controller di default**: isteresi semplice su entrambi i versi, oppure PID sul caldo + isteresi sul freddo (consiglio, ricalca l'originale)? Il PID sul freddo resta possibile ma sconsigliato per un compressore.
- **D10 — Strategia asset UI**: gzip in PROGMEM dentro il .bin (consiglio: un solo file da flashare/OTA, filesystem mai toccato) oppure doppio upload firmware + immagine LittleFS.
- **D11 — Unità di densità**: SG (1.0xx) o °Plato, per dashboard e step a densità (l'iSpindel invia ciò che è configurato a bordo suo: dimmi come ce l'hai).
- **D12 — NTP**: attivo di default (`pool.ntp.org`, disattivabile) per tenere allineato il DS1307, o spento (ora solo da RTC/impostazione manuale)?
