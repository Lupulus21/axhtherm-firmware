# AxHTherm 2.5 — Report forense (FASE 0 + FASE 1)

> Analisi **read-only** del firmware v2.23.2, propedeutica al passaggio da 1 a 2 camere di
> fermentazione. Ogni affermazione è ancorata a `file:riga` (codice) o al documento hardware.
> Prodotto il 2026-08-31. Baseline git: commit `ad1b50d`.
> Riferimenti: `docs/analysis/feasibility-2ch.md` (Fase 2), `docs/analysis/domande-aperte.md`.

---

# FASE 0 — Inventario

| Voce | Rilievo | Fonte |
|---|---|---|
| **Repo** | `.` — **NON è un repository git** (`git status` → *fatal: not a git repository*) | verificato in sessione |
| **Build system** | PlatformIO, 2 environment: `d1_mini` (firmware) e `native` (unit test su host) | `platformio.ini:5,29` |
| **Target** | **WEMOS D1 mini — ESP8266**, 4 MB flash, layout **4M2M** (1 MB sketch + 1 MB OTA + ~2 MB LittleFS) | `platformio.ini:2,8,13,14` |
| **Framework** | Arduino ESP8266, platform `espressif8266@4.2.1` (core 3.1.2) | `platformio.ini:7,9`; `docs/ARCHITETTURA.md:20` |
| **Concorrenza** | **Bare-metal superloop cooperativo. Nessun RTOS, nessun task, nessuna ISR applicativa.** Tutto gira in `loop()` con scheduling a confronto di `millis()`; `delay(2)` finale nutre il soft-WDT | `src/main.cpp:964-1030` |
| **Dipendenze** | ArduinoJson ^6.21.5, OneWire ^2.3.8, DallasTemperature ^3.11.0, ESP8266/ESP32 OLED SSD1306 ^4.6.1, PubSubClient ^2.8 | `platformio.ini:16-21` |
| **Pre-build** | `tools/gzip_assets.py` comprime `data-src/index.html` in `include/web_assets.h` (PROGMEM) | `platformio.ini:22` |
| **Test** | **74 test nativi** sul motore di controllo (`lib/axcore`), Unity | `test/test_core/test_main.cpp` (1397 righe, 74 `RUN_TEST`) |
| **Copertura test** | Solo `lib/axcore` (isteresi, PID, interlock, guardia compressore, profilo, stimatore predittivo, SafetyMonitor). **Zero test su `src/` — HAL, persistenza, web, MQTT non sono testati** | `platformio.ini:31` (`test_build_src = false`) |

**Comandi di build** — `pio` NON è nel PATH: vive in un venv dedicato.

```bash
~/.venvs/platformio/bin/pio run  -e d1_mini    # firmware
~/.venvs/platformio/bin/pio test -e native     # test del motore di controllo (-e native OBBLIGATORIO)
~/.venvs/platformio/bin/pio run  -e d1_mini -t upload --upload-port /dev/cu.usbserial-11340
```

> ✅ **Verificato il 2026-08-31**: `pio test -e native` → **74/74 PASSED** in 1,43 s.
> L'ultimo esito tracciato nei documenti era 72/72 (`CORREZIONI.md:378`): i 2 test aggiunti
> successivamente passano. Questo è il riferimento da mantenere verde a ogni commit.

**Prova che il build funziona**: artefatti presenti e coerenti del 26/08/2026 —
`.pio/build/d1_mini/firmware.elf` (2.167.268 B) e `firmware.bin` (**720.816 B**).

**Documentazione hardware presente:**

| File | Copre |
|---|---|
| `pcb/Schematics_AxHTherm2-5V.pdf` | **Schema elettrico completo, 1 pagina, rev 1.0 del 2018-10-12** — è la fonte primaria |
| `pcb/BOM_AxHTherm_2V5.csv` | Distinta ufficiale, 17 righe / 22 componenti |
| `pcb/CPL_AxHTherm_2V5.csv` | Pick&place, 22 footprint, coordinate mm |
| `pcb/EasyEDA_schematic_*.json` / `_pcb_*.json` | Sorgenti EasyEDA (netlist: REL1-8, I1-8, O1-8, IOE1-3, WD0-WD8) |
| `pcb/Gerber_*` | Set Gerber 2 layer, 93,98 × 58,42 mm |
| `include/hardware_map.h` | Unica fonte di verità del mapping pin lato firmware, con voci `// UNVERIFIED` |

---

# FASE 1 — Analisi forense

## 1.1 — Catena di controllo

Cadenza: **1 Hz**, guidata da `loop()`; il `dt` reale (non 1 s assunto) è misurato e clampato a 10 s.

| # | Stadio | Dove | Note |
|---|---|---|---|
| 1 | **Lettura sonde** — OneWire GPIO0, conversione **asincrona** (richiesta → 1000 ms → lettura), periodo 5 s | `src/hal/ProbeBus.h:127-215`, chiamato da `src/main.cpp:967` | 3 sonde (`kProbes=3`): idx 0 = controllo/mosto, 1-2 ausiliarie. L'ordine di lettura **ruota** a ogni ciclo (`primaSonda_`, riga 159/201) per non concentrare la marginalità del bus sulla sonda 0 |
| 2 | **Validazione/filtro** — range fisico [-20, +60] °C, retry singolo su -127, fault dopo **3** letture cattive consecutive, rientro dopo **2** buone, scadenza per età a 30 s, finestra di grazia su stallo del loop | `ProbeBus.h:164-214` | Non è un filtro passa-basso: il controllo usa il valore istantaneo + offset. La media a 12 campioni (60 s) serve **solo alla taratura** (`rawAverage`, riga 69) |
| 3 | **Motore di profilo** → setpoint | `ax::ProfileEngine::tick`, `lib/axcore/src/ax/ProfileEngine.cpp`; invocato da `src/main.cpp:421` | 4 tipi di step (Fixed / RampTime / RampRate / Gravity), max 12 (`ax::kMaxSteps`, `Types.h:6`). Tempi in **epoch UTC assoluti** |
| 4 | **Clamp di attuazione** — setpoint forzato in **[-1 °C, +40 °C]**, NaN-safe | `src/main.cpp:430-435` | Unico choke point: copre termostato, override, MQTT e valori persistiti da firmware vecchi |
| 5 | **Regolazione** — isteresi a doppia soglia (freddo ON a `set+tol`, OFF a `set`; caldo speculare) + **PID opzionale sul caldo** (finestra a duty) + **spegnimento predittivo** che impara l'inerzia (°C/h) | `HysteresisController.h:16-34`, `PidController.h`, `PeakEstimator.h`; orchestrati in `ControlPipeline::tick`, `ControlPipeline.h:76-155` | `usePid` sceglie PID vs isteresi sul caldo (`AppConfig.h:33`) |
| 6 | **Politica di step** — `allowHeat`/`allowCool` (free-rise, cold crash naturale) | `ControlPipeline.h:120-124` | |
| 7 | **Interlock** — mai caldo+freddo insieme, + tempo morto `minSwitchSec` sull'inversione | `Interlock.h:19-43` | Priorità strutturale al freddo (`ControlPipeline.h:125-128, 140`) |
| 8 | **Guardia compressore** — riposo minimo fra avvii (`compressorRestSec`) e marcia minima (`compressorMinOnSec`), su epoch **persistiti** | `CompressorGuard.h:30-44` | Vale anche attraverso i riavvii |
| 9 | **Attuazione** — unico punto di scrittura dei relè | `OutputBus::applyAutomation` → `writeRel`, `src/hal/OutputBus.h:32-38, 86-98`; chiamato da `src/main.cpp:439` | REL1-3 = `digitalWrite` diretto; REL4-8 = MCP23008 via I2C |
| 10 | **Feedback/stato** — `buildStatus()` una volta al secondo → OLED, MQTT, `/api/status`, log | `src/main.cpp:263-372, 1005` | |
| 11 | **Persistenza** — su **transizione** (cambio step, fine profilo, cambio stato freddo) + checkpoint ogni 5 min | `src/main.cpp:553-558, 1011-1014` | |

**Punto chiave del refactor:** l'intera catena 3→8 è già composta da **oggetti senza stato globale**
(`ProfileEngine`, `ControlPipeline` e i suoi membri `HysteresisController`, `PidController`,
`CompressorGuard`, `Interlock`, `PeakEstimator`). Sono già istanziabili N volte: nessuno usa
variabili globali, `static` di funzione o singleton. Questo è il maggior punto di forza per il
porting a 2 camere.

## 1.2 — Assunzioni mono-camera cablate nel codice

Censimento eseguito da 5 finder per area + 1 cacciatore di lacune avversariale:
**448 punti** in cui l'unicità della camera è cablata. Ripartizione:

| Categoria | N | Categoria | N |
|---|---|---|---|
| `api-shape` | 79 | `singleton` | 31 |
| `global-state` | 64 | `static-buffer` | 30 |
| `protocol` | 40 | `shared-timer` | 23 |
| `ui-string` | 32 | `config-macro` | 19 |
| `persistence-layout` | 29 | `hardcoded-index` | 21 |
| `other` | 32 | `logging` | 3 |

La maggioranza (`api-shape`, `ui-string`) è lavoro **meccanico e a basso rischio**. Le voci
pericolose sono `shared-timer` e `static-buffer`: sono quelle che **non danno errore di
compilazione** e diventano condivise in silenzio. Elenco delle trappole vere:

### 🔴 Trappole: stato che diventerebbe condiviso senza accorgersene

| Simbolo | Riga | Perché è una trappola |
|---|---|---|
| **`PidController::windowOutput` — fase = `nowMs % cfg_.windowMs`** | `PidController.h:44` | **La più insidiosa.** La fase della finestra di time-proportioning deriva dal `millis()` globale, non da un'origine per-istanza. Due camere con lo stesso `windowMs` (che verrebbe dallo stesso default) avrebbero le finestre **perfettamente sincronizzate**: i due riscaldanti si accenderebbero **nello stesso istante, a ogni finestra**. È uno spunto di corrente evitabile con uno sfasamento per-camera |
| `devSp`, `devBestGap`, `devStallMs`, `devStalling` | `main.cpp:467-470` | Timer dell'allarme di scostamento termico (soglia 3 h). Unici: il tempo maturato da una camera verrebbe attribuito all'altra |
| `probeSuspectMs` | `main.cpp:526` | Idem per il cross-check sonda sospetta (soglia 30 min) |
| `static ax::StepDef steps[12]` in `startProfileCmd` | `main.cpp:155` | Buffer statico condiviso, giustificato dal commento con *"esecuzione single-thread: nessun problema di rientranza"*. **L'argomento regge finché esiste una sola camera** |
| `lastCtlMs`, `lastCkptMs`, `lastLogMs` | `main.cpp:74` | Cadenze uniche di tick, checkpoint e log |
| `OutputBus::loop()` — recovery MCP | `OutputBus.h:66` | Il recovery riuscito azzera **in blocco** `states_[3..7]`: lo stato ombra dei relè di *entrambe* le camere |
| Costanti `-1` / `+40` del clamp setpoint | duplicate letteralmente in ≥5 punti | Duplicandole per camera si moltiplica il rischio di divergenza |
| `ProfileEngine::overrideSetpoint` | `ProfileEngine.h:54` | **Non clampa**: la protezione dipende interamente dai chiamanti e dal choke point di attuazione. Replicarla senza il choke point aprirebbe un buco reale |
| Cross-check sonda: `for i=1..kProbes`, `nAux>=2` | `main.cpp:531-547` | Assume che **tutte** le sonde ≠0 siano ausiliarie della stessa camera. Con 2 camere confronterebbe il mosto della camera A con la sonda della camera B |
| `MqttSvc` doc di stato `StaticJsonDocument<1024>` | `MqttSvc.cpp:229` | Il codice **prevede già il traboccamento e SALTA la pubblicazione** (`:275`). Raddoppiare le chiavi lo fa traboccare |
| `handleStatus` `doc(3072)` | `WebApi.cpp:222` | Il commento dichiara che il caso peggiore di **una** camera supera già 2 KB e ArduinoJson **tronca in silenzio** |
| `CalibStore::kMaxPoints = 12` | `CalibStore.h:29` | Tetto condiviso da *tutti* i densimetri: con 2 iSpindel si esaurisce a 6 punti ciascuno |
| `DeviceRegistry::kMax = 4` con sfratto LRU | `DeviceRegistry.h:18` | Con 2 camere servono 2 strumenti stabilmente in elenco |
| `ProfileStore::readFile` | `ProfileStore.cpp:84` | La **scrittura** è già in streaming (fix v2.22.1), ma la **lettura** alloca ancora 6144 in un colpo |

### Stato globale in `src/main.cpp` (una istanza sola di ogni cosa)

| Simbolo | Riga | Natura |
|---|---|---|
| `engine` (`ax::ProfileEngine`) | `main.cpp:50` | **per-camera** — 636 B |
| `pipeline` (`ax::ControlPipeline`) | `main.cpp:51` | **per-camera** — 204 B |
| `safety` (`ax::SafetyMonitor`) | `main.cpp:54` | **per-camera** (contiene però anche fault globali: config, storage, RTC) |
| `curStatus` (`AppStatus`) | `main.cpp:56` | **per-camera** — 176 B |
| `activeProfileName` | `main.cpp:57` | **per-camera** |
| `lastTick`, `lastPipe`, `prevCool` | `main.cpp:59-61` | **per-camera** |
| `pstate` (`PersistState`) | `main.cpp:42` | misto: `engine`/`guard`/`coolRel`/`heatRel`/`batchName`/`ogGravity`/`coolEstCph`/`heatEstCph` sono per-camera; `lastSeenEpoch`/`epochsTrusted`/`armed` sono globali |
| `probes` (`ProbeBus`) | `main.cpp:33` | **condiviso** (un solo bus fisico) — 384 B |
| `outputs` (`OutputBus`) | `main.cpp:32` | **condiviso** (un solo ULN2803) |
| `gravity`, `gravityAny` (`GravityTracker`) | `main.cpp:52-53` | oggi globali; con 2 camere servono 2 densimetri → **per-camera** |
| `mcp`, `rtc`, `timeSvc`, `net`, `oled`, `mqtt`, `web`, `dataLog`, `cfgStore`, `stateStore`, `profStore`, `calibStore`, `devices`, `bootLog` | `main.cpp:31-49` | **condivisi** (corretto così) |
| `static` di funzione nel tick termico | `main.cpp:467-470` (`devSp`, `devBestGap`, `devStallMs`, `devStalling`), `main.cpp:526` (`probeSuspectMs`) | **per-camera**: oggi sono `static` locali → diventerebbero silenziosamente condivisi fra le camere. **Trappola classica del porting** |
| `static ax::StepDef steps[12]` in `startProfileCmd` | `main.cpp:155` | buffer condiviso 576 B, riusato: va verificato che resti sicuro con 2 chiamanti |

### Indici e costanti di dimensionamento

| Simbolo | Riga | Vincolo |
|---|---|---|
| `ProbeBus::kProbes = 3` | `ProbeBus.h:12` | dimensiona **tutti** gli array interni (`cfg_`, `st_`, `stat_`, `raw_[3][12]`, …, `ProbeBus.h:313-324`) |
| `probes.control()` → `st_[0]` | `ProbeBus.h:246` | **sonda di controllo hardcoded all'indice 0** |
| `probes.reading(0/1/2)` in `buildStatus` | `main.cpp:265` | mapping ruolo→indice fisso |
| ciclo ausiliarie `for i=1..kProbes` | `main.cpp:531` | "ausiliaria" = "non indice 0" |
| `AppConfig::probeId/probeOffset/probeLabel[3]` | `AppConfig.h:28-30` | array a 3, iterati con letterale `3` in `main.cpp:102, 636, 640` |
| `outputs.setMapping(cool, heat)` — **una** coppia | `OutputBus.h:24-27, 101` | `coolRel_`/`heatRel_` singoli |
| `OutputBus` supporta **8 relè** (`states_[8]`, `writeRel` 1..8) | `OutputBus.h:41, 75, 83, 103` | ✅ già dimensionato per più camere |

### Persistenza (LittleFS)

| File | Contenuto | Impatto 2 camere |
|---|---|---|
| `/config.json` | oggetto `control` singolo + array `probes[3]` | `ConfigStore.cpp:63-84` — serve un array `chambers[]`; `fromJson` usa ovunque l'idioma `x \| default` (`ConfigStore.cpp:124+`) → **una config vecchia si carica senza errori**, i campi nuovi prendono i default |
| `/state.json` | un `PersistState` | `StateStore.h:9-22` — serve stato per camera |
| `/profile.json` | **un solo** profilo attivo | `ProfileStore.h:13` |
| `/profiles/pN.json` (N=1..20) | libreria condivisa | `ProfileStore.h:14` — resta condivisa (corretto) |
| `/log/d<day>.bin` | record **packed da 16 byte**: `tControl, tAux1, tAux2, setpoint, flags, gravity, battery` | `DataLog.h:9-19` + `static_assert` riga 19 — **il formato non ha un campo camera**. Estenderlo rompe i file esistenti: serve versionamento o file per camera |
| `/calib.json`, `/devices.json`, `/boot.json` | globali | corretto così |
| **Versionamento schema** | **assente** — nessun campo `schemaVersion` in nessuno store | residuo già noto (`memory: axhtherm-prodready-campaign`) |

### API e UI

- **~44 endpoint** `/api/*`, tutti a camera implicita: `/api/status`, `/api/profile`,
  `/api/profile/{start,stop,pause,resume,skip,coldcrash}`, `/api/setpoint`, `/api/thermostat`,
  `/api/outputs/{arm,disarm}`, `/api/gravity/og`, `/api/probes*` (`src/svc/WebApi.cpp:56-137`).
  Servirà un discriminante camera (path `/api/ch/{n}/...` o parametro), mantenendo gli alias
  attuali sulla camera 1 per retrocompatibilità.
- **UI web**: 5 tab (`dash`, `trend`, `prof`, `conf`, `sys`) in `data-src/index.html` (144.537 B
  sorgente), tutti a camera singola.
- **OLED 128×64**: il layout occupa **interamente** lo schermo per una camera — T grande (24 px),
  setpoint, riga modo, riga uscite, riga allarmi (`src/svc/OledUi.cpp:45-105`). Due camere
  richiedono layout diviso o pagine alternate.
- **MQTT**: topic `<base>/state` e `<base>/status`, discovery HA su
  `homeassistant/{sensor,binary_sensor,button,climate}/<devId>_<key>/config`
  (`src/svc/MqttSvc.cpp:114, 131, 147, 181, 278`) — un solo dispositivo climate.
- **Comandi seriali**: `arma <coolRel> <heatRel>`, `stato`, `sonde` — camera implicita
  (`main.cpp:610, 704-714`).

## 1.3 — Budget risorse

**Misure dal binario reale** (`.pio/build/d1_mini/firmware.elf`, `xtensa-lx106-elf-size -A`):

| Sezione | Byte | Dove |
|---|---|---|
| `.text` + `.text1` (IRAM) | 29.316 | RAM istruzioni |
| `.data` | 3.152 | DRAM inizializzata |
| `.rodata` | 13.824 | DRAM |
| `.bss` | 30.344 | DRAM azzerata |
| **DRAM statica totale** | **47.320** | su 80 KB di DRAM → **~32,7 KB** per heap+stack |
| `.irom0.text` | 670.372 | flash |
| **`firmware.bin`** | **720.816** | su ~1.044.464 B di sketch → **~324 KB liberi (31 %)** |

Heap libero misurato sul dispositivo: **~17,6 KB** (`memory: axhtherm-profile-save-heap-fix`),
coerente con il calcolo sopra al netto dei buffer dinamici dell'SDK WiFi.

**Costo RAM di una seconda camera** (`xtensa-lx106-elf-nm -S`, valori esatti dal binario):

| Oggetto | Byte | Per-camera? |
|---|---|---|
| `engine` (`ProfileEngine`) | 636 | sì |
| `pipeline` (`ControlPipeline`) | 204 | sì |
| `curStatus` (`AppStatus`) | 176 | sì |
| `probes` (`ProbeBus`, 3 sonde) | 384 | condiviso; **~121 B per sonda aggiuntiva** |
| `cfg` (`AppConfig`) | 400 | parzialmente |
| `startProfileCmd::steps` | 576 | condiviso (statico) |
| `calibStore` | 772 | condiviso |
| `web` | 556 | condiviso |

→ **~1,0 KB di RAM statica per la seconda camera**, +~0,4 KB se le sonde passano da 3 a 6.
**Totale ~1,4–1,6 KB su ~17,6 KB di heap: fattibile, ma è ~8-9 % del margine.**

**⚠️ Il vero collo di bottiglia non è lo stato di controllo: è la serializzazione JSON.**
Il percorso config alloca **due `DynamicJsonDocument(6144)`** (`ConfigStore.cpp:10,21` +
`WebApi.cpp:331,355`) e quello profili altrettanto (`WebApi.cpp:439,477,502,546`;
`ProfileStore.cpp:84`). È **esattamente** il difetto già occorso in campo (profili `{}` /
"salvataggio fallito", fw 2.22.1) e già corretto con serializzazione streaming in
`ProfileStore::writeFile` (`ProfileStore.cpp:96-127`). Raddoppiare i campi di `/config.json`
in un unico documento **ri-attiva quella classe di bug**.

**Risorse hardware.** Netlist ricostruita programmaticamente da `EasyEDA_schematic_*.json` e
**verificata incrociandola** con il campo `net` di ogni PAD in `EasyEDA_pcb_*.json` e con la
serigrafia: le tre fonti coincidono al 100 %.

> 🔴 **Rettifica importante.** Lo schema mostra 8 canali relè, ma **il MCP23008 NON è montato
> sull'esemplare fisico** — verificato al banco il 15/08/2026: lo scan I2C non trova 0x20
> (`docs/ANALISI.md:188` assunzione A3; `docs/COLLAUDO.md:13-15, 24-26`). E gli ingressi ULN
> I4-I8 sono **hardwired ai GP0-GP4 del MCP e non toccano P1** (reti `GP0..GP4`: esattamente 2
> pad ciascuna). Quindi **oggi esistono fisicamente 3 sole uscite** (REL1-3) e **ne servono 4**.
> Le 5 uscite bufferate inutilizzate si sbloccano **solo montando il chip** nel footprint DIP-18
> già cablato: nessuna modifica al PCB, ma **un componente da acquistare**.

| Risorsa | Stato |
|---|---|
| **GPIO occupati** | GPIO5/4 = I2C (SCL/SDA); GPIO0 = OneWire (pull-up R4 4,7 kΩ **verso 5 V**); GPIO12/13/15 = REL1/2/3 **e LED1-3** (blink software vietato) |
| **GPIO liberi** | **solo 3 digitali + 1 ADC**, e nessuno è bufferato dall'ULN: GPIO14 (WD5, P1 pad 1) — *unico senza vincoli*; GPIO16 (WD0, P1 pad 2); GPIO2 (WD4, P1 pad 4) — strapping + LED on-board; A0 (WA0, P1 pad 3, con pull-down 1 kΩ). GPIO6-11 non hanno pad: **il conteggio è chiuso** |
| **Cablaggio attuale (banco 15/08/2026)** | **REL1 → cavo riscaldante, REL2 → NON cablato (libero), REL3 → frigo** (`docs/ANALISI.md:185` A1) |
| **ADC / Timer / PWM / DMA / SPI** | 1 ADC libero; **nessun timer HW** usato (il PID usa una finestra software, non PWM); **DMA non applicabile** su ESP8266; SPI non usato |
| **Interrupt** | **nessuna ISR applicativa**. L'INT del MCP è portato su P1 ma non usato. Solo l'SDK WiFi |
| **I2C** | Rilevati al banco: 0x68 DS1307, 0x50 EEPROM (modulo Tiny RTC), 0x3C OLED. 0x20 assente. Clock **100 kHz** (limite DS1307). ⚠️ **Nessun pull-up I2C sul PCB**: le uniche sono sui moduli RTC/OLED — staccare l'RTC può uccidere il bus. ⚠️ Livelli misti: VDD periferiche a 5 V, SDA/SCL da GPIO 3,3 V, **nessun level shifter** |
| **OneWire** | 1 bus, morsetto PROBE 3 poli (1=VCC, 2=GND, 3=dato). **Supporta N sonde per ROM**: il limite è firmware (`kProbes=3`), non hardware. Le sonde della 2ª camera vanno **in parallelo sullo stesso morsetto** |
| **ULN2803APG** | 8 canali Darlington non invertenti. I1-I3 ← GPIO12/13/15; **I4-I8 ← MCP GP0-GP4, non esposti su alcun header** |
| **Uscite** | REL1-3 sul morsetto **RELAY** (che porta *solo* i collettori aperti: né VCC né GND né CD). REL4-8 su **P1 pad 12-16**. Comune diodi `CD` su P1 pad 17 / morsetto POWER pad 1 (serigrafia `ULNCD`), legabile a VCC col jumper **UPWR** |
| **MCP23008** | Indirizzo **0x20 inchiodato** (A0/A1/A2 a GND nel layout: nessun jumper) → non se ne può montare un secondo su quel footprint. GP5-GP7 libere (IOE1-3 su P1), a livello 5 V, non bufferate |
| **Alimentazione** | **Nessun convertitore a bordo**: BOM e CPL sono esaustivi (22 componenti) e non contengono alimentatori, regolatori, induttori o ponti. Unico ingresso: morsetto POWER (1=CD, 2=VCC 5 V, 3=GND). La corrente disponibile **non è una proprietà della scheda** |
| **Disaccoppiamento** | ⚠️ **C1 = 47 pF è l'unico condensatore della scheda**. Nessun bulk elettrolitico sulla 5 V: aggiungendo bobine commutate il rail dell'ESP è esposto ai transitori |
| **Sicurezza lato potenza** | ⚠️ **Nulla a bordo**: nessun fusibile, MOV, snubber, optoisolatore, TVS; nessuna fresatura di isolamento (contorno Gerber rettangolare pieno). Tutti i relè sono **esterni** |

### ⚠️ Due trappole di cablaggio da non ignorare

1. **La numerazione dei pin di P1 in `docs/ANALISI.md:74` è SBAGLIATA** rispetto al layout
   pubblicato e alla serigrafia. Il doc dice `1=IOE3 … 5=REL8`; il layout dice
   `1=WD5, 4=WD4, 8=IOE3, 12=REL4, 16=REL8, 17=CD, 20=VCC`. Non è un semplice ribaltamento.
   **Regola operativa: sul P1 fidarsi solo dei nomi serigrafati, mai dei numeri.**
2. **Discrepanza di revisione PDF ↔ layout.** Il PDF rev 1.0 disegna `POWER` a 2 poli, non ha il
   jumper `UPWR`, ha un connettore `LED` a 6 poli e un ordine di P1 diverso; il progetto EasyEDA
   pubblicato ha `POWER` a 3 poli con `ULNCD` e il jumper `UPWR`. La scheda fisica è dichiarata
   rev 1.0 (`pcb/LEGGIMI.md:42`). **Va stabilito quale revisione si ha in mano prima di cablare**,
   perché cambia da dove si preleva il comune bobine.

## 1.4 — Protezioni di sicurezza esistenti

| Protezione | Presente | Dove | Comportamento in guasto |
|---|---|---|---|
| **Uscite spente prima di tutto** | ✅ | `main.cpp:752` → `OutputBus::safeInitEarly` (`OutputBus.h:12-19`) | primissima istruzione di `setup()`, prima di Serial/I2C/FS |
| **Stato sicuro di default** | ✅ | ULN2803 **non invertente**: ingresso basso = uscita spenta = bobina diseccitata (`OutputBus.h:22`, schema) | pin flottante al boot → ULN non conduce. GPIO15 è tenuto basso dal carico d'ingresso dell'ULN (`hardware_map.h:19-21`) |
| **Flag `armed` persistito** | ✅ | `main.cpp:390, 869-870`; `SafetyMonitor::outputsAllowed` (`SafetyMonitor.h:21`) | finché l'utente non conferma il mapping relè, **l'automazione non pilota nulla** |
| **Qualunque dubbio → tutto spento** | ✅ | `ControlPipeline.h:80-91` | setpoint assente, sonda non valida, config corrotta, MCP KO, non armato |
| **Sonda guasta/scollegata** | ✅ | `ProbeBus.h:164-214` | DS18B20 restituisce **-127** (nessuna risposta/CRC) o **85** (power-on); entrambi fuori dal range [-20,60] → non valido → dopo 3 letture `controlProbeFault` → **uscite spente** |
| **Clamp setpoint** | ✅ | **all'attuazione**, `main.cpp:430-435`: `[-1, +40] °C`, NaN-safe | unico choke point per tutte le sorgenti |
| **Anti-corto-ciclo compressore** | ✅ | `CompressorGuard.h:30-44`; default riposo 5 min / marcia min 60 s (`AppConfig.h:40-41`) | epoch **persistiti** → vale attraverso i riavvii |
| **Anti-chattering** | ✅ | `minStateSec` (60 s) nel latch d'isteresi (`HysteresisController.h:25-32`) | |
| **Tempo morto inversione** | ✅ | `minSwitchSec` (120 s) in `Interlock` (`Interlock.h:29-40`) | |
| **Mai caldo+freddo insieme** | ✅ | `Interlock.h:21-25` (conflitto → **entrambi spenti** + flag) + priorità freddo (`ControlPipeline.h:126, 140`) | doppia barriera |
| **Power-on / brownout** | ✅ | `main.cpp:827-866` | compressore che risultava in marcia → riposo pieno; orologio inaffidabile → ritardo assunto **non** trascorso; rampa ri-ancorata alla T reale |
| **Salti d'orologio** | ✅ | `main.cpp:990-999` → `shiftClock` su motore, pipeline, guardia, latch | consumato **prima** di `controlTick` |
| **Watchdog** | ⚠️ **solo SDK** | `delay(2)` a `main.cpp:1029` | **Nessuna gestione esplicita del watchdog in tutto il firmware**: `grep` di `ESP.wdtEnable/wdtDisable/wdtFeed/ets_wdt` su `src/ lib/ include/` → **zero occorrenze**. Il soft-WDT è nutrito *esclusivamente* dal `delay(2)` finale. **Durante uno stallo i relè restano latchati nell'ultimo stato** — nessuno li riporta a OFF fino al reset. È un **residuo dichiarato e non corretto** della campagna di revisione |
| **`safeInitEarly` copre solo i GPIO** | ⚠️ | `OutputBus.h:12-19` | Mette a LOW **solo** GPIO12/13/15. **REL4-8 (via MCP) NON sono coperti**: dipendono da `mcp.begin()` più avanti in `setup()` e dal reset hardware del MCP. Finestra indeterminata più lunga per le uscite sull'espansore |
| **Il profilo NON si ferma su fault sonda** | ℹ️ scelta | `main.cpp:421` | `engine.tick()` è chiamato incondizionatamente: gli step **a tempo continuano a scadere**; solo l'attuazione è tagliata |
| **Allarme fuga termica** | ✅ (solo allarme) | `main.cpp:441-514` — scarto >5 °C senza progresso ≥0,5 °C per 3 h | **non taglia mai le uscite** (scelta deliberata) |
| **Sonda di controllo sospetta** | ✅ (solo allarme) | `main.cpp:516-551` — divergenza >15 °C da **tutte** le ausiliarie per 30 min; richiede ≥2 ausiliarie valide | decisione utente registrata: allarme, mai taglio uscite |
| **Guasto driver uscite** | ✅ | `outputs.mcpFault() && mcpNeeded()` (`main.cpp:389`), retry 5 s (`OutputBus.h:62-68`) | revoca il consenso alle uscite **solo se il mapping usa REL4-8** |
| **Test relè manuale** | ✅ | vietato con sistema armato, timeout 10 min (`OutputBus.h:46-51, 61`) | |
| **Scritture atomiche** | ✅ | tmp + rename + verifica flush (`FsCommit.h`, `ConfigStore.cpp:26-38`) | esito verificato → `alarmStorage` |
| **Tetto dimensione log** | ✅ | `DataLog::kMaxTotalBytes = 1.400.000` (`DataLog.h:25`) | il log non può affamare config/stato |

---
