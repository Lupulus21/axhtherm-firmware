# AxHTherm 2.5 — Analisi Fase 0 (hardware, manuale, binario)

Fonti: `Schematics_AxHTherm2-5V.pdf` (rev 1.0, 2018-10-12, EasyEDA — **fonte di verità hardware**),
`SW_Configurazione.pdf` (pagina blog iobrewing.net "AxHTherm V2.0", 15 pagine — **fonte di verità funzionale**),
`install.bin` (575 968 byte, SHA-256 `bc83dc16dae2666a590109b74d48284d6fa6d5742f9b0d926d3ec1712ca2801b`, analizzato solo via `strings`/`xxd`, lasciato intatto),
`ANALISI_FIRMWARE_ORIGINALE.md` (analisi precedente, verificata in questa sessione).

Legenda confidenza:
- **[SCHEMA]** — letto direttamente dallo schema elettrico (massima confidenza).
- **[PINOUT]** — dato standard della board WEMOS D1 mini (mappa Dx→GPIO), non specifico dello schema.
- **[BINARIO]** — stringa presente in `install.bin` (evidenza citata).
- **[MANUALE]** — dichiarato nel manuale utente.
- **[UNVERIFIED]** — non ricavabile dai file: serve verifica al banco; ogni voce ha procedura di verifica e va resa configurabile da UI.

---

## 1. Hardware (dallo schema, rev 1.0 del 2018-10-12)

### 1.1 Pin del WEMOS D1 mini

| Pin | GPIO | Rete schema | Uso | Confidenza |
|---|---|---|---|---|
| RST | — | `WRST` | Reset condiviso con RESET del MCP23008 (pin 6) e jumper IPE verso GND | [SCHEMA] |
| A0 | ADC0 | `WA0` | Su P1 pin 17, con R9 1 kΩ verso `W3V3` | [SCHEMA] |
| D0 | GPIO16 | `WD0` | Libero, esposto su P1 pin 16 | [SCHEMA]/[PINOUT] |
| D1 | GPIO5 | `WD1SCL` | **I2C SCL** (MCP23008, RTC DS1307, connettore OLED) | [SCHEMA]/[PINOUT] |
| D2 | GPIO4 | `WD2SDA` | **I2C SDA** (idem; sull'RTC la label è `WD2SCA`, refuso dello schema) | [SCHEMA]/[PINOUT] |
| D3 | GPIO0 | `WD3` | **OneWire DS18B20** (connettore PROBE), pull-up R4 4,7 kΩ verso VCC. Pin di strapping: deve essere alto al boot — garantito dal pull-up | [SCHEMA]/[PINOUT] |
| D4 | GPIO2 | `WD4` | Libero, esposto su P1 pin 14. Strapping (alto al boot); porta il LED blu on-board del WEMOS | [SCHEMA]/[PINOUT] |
| D5 | GPIO14 | `WD5` | Libero, esposto su P1 pin 15 | [SCHEMA]/[PINOUT] |
| D6 | GPIO12 | `WD6` | **ULN I1 → REL1** e **LED2** (via LDC2, R2 1 kΩ) | [SCHEMA]/[PINOUT] |
| D7 | GPIO13 | `WD7` | **ULN I2 → REL2** e **LED1** (via LDC1, R1 1 kΩ) | [SCHEMA]/[PINOUT] |
| D8 | GPIO15 | `WD8` | **ULN I3 → REL3** e **LED3** (via LDC3, R3 1 kΩ). Strapping (basso al boot — garantito dal carico d'ingresso dell'ULN) | [SCHEMA]/[PINOUT] |
| TX/RX | GPIO1/3 | `WTX`/`WRX` | Connettore SERIAL "BT Bus" (con `W3V3` e GND) | [SCHEMA] |
| 5V | — | `VCC` | Alimentazione 5 V dal connettore POWER; C1 47 pF | [SCHEMA] |
| 3V3 | — | `W3V3` | Su P1 e SERIAL | [SCHEMA] |

### 1.2 MCP23008 (espansore I2C)

- A0, A1, A2 = reti `AD0/AD1/AD2`, **tutte a massa** → indirizzo **0x20**. [SCHEMA] (lo scan I2C in diagnostica lo confermerà a runtime)
- RESET (pin 6) sulla rete `WRST` → **si resetta insieme all'ESP8266**; al reset i GP tornano input Hi-Z → ULN spento → REL4–8 rilasciati. Failsafe hardware utile. [SCHEMA]
- INT → P1 pin 4. GP0–GP4 → ULN I4–I8 (**REL4–REL8**); GP5/GP6/GP7 → `IOE1/IOE2/IOE3` su P1. [SCHEMA]
- VDD = VCC = **5 V** (bus I2C a livelli 5 V-tolleranti di fatto; l'ESP8266 in pratica li regge — fatto hardware esistente, nessuna modifica). [SCHEMA]

### 1.3 ULN2803 e uscite

| Ingresso ULN | Sorgente | Uscita | Rete | Dove |
|---|---|---|---|---|
| I1 | D6 (GPIO12) | O1 | **REL1** | Connettore RELAY pin 1 |
| I2 | D7 (GPIO13) | O2 | **REL2** | Connettore RELAY pin 2 |
| I3 | D8 (GPIO15) | O3 | **REL3** | Connettore RELAY pin 3 |
| I4–I8 | MCP GP0–GP4 | O4–O8 | REL4–REL8 | P1 pin 9,8,7,6,5 |

Tutto [SCHEMA]. Uscite open-collector verso massa; `CD+` (comune dei diodi di ricircolo) sulla rete `CD` → P1 pin 10: va al +V delle bobine relè. **CD non è un'uscita.**
Logica non invertente: GPIO alto → uscita ULN attiva → relè eccitato. Al reset tutti i pin sono input/bassi → **tutti i relè diseccitati per hardware**.

### 1.4 LED — ⚠ vincolo critico

| LED | Rete di pilotaggio | Condivide con |
|---|---|---|
| LED1 | `WD7` (D7/GPIO13) | **REL2** |
| LED2 | `WD6` (D6/GPIO12) | **REL1** |
| LED3 | `WD8` (D8/GPIO15) | **REL3** |

Tutto [SCHEMA] (connettore LED a 6 poli: WD7/LDC1, WD6/LDC2, WD8/LDC3; resistenze 1 kΩ verso GND).
**È VIETATO qualunque lampeggio software su D6/D7/D8**: commuterebbe i relè di potenza. I LED indicano *di fatto* lo stato dei relè e nient'altro; lo stato di sistema va su OLED/web UI.

### 1.5 Altri blocchi

- **PROBE** (3 poli): dato = D3 con pull-up 4,7 kΩ a VCC (5 V), più alimentazione e GND. Bus per 1–3 DS18B20. [SCHEMA]
- **RTC**: modulo "Tiny RTC I2C" (DS1307, indirizzo fisso **0x68**); `bat/ds/sq` esposti su P1 (pin 11/12/13). I moduli Tiny RTC portano tipicamente anche una EEPROM AT24C32 a **0x50**: potrà comparire nello scan I2C. [SCHEMA]; la EEPROM è [UNVERIFIED].
- **Connettore I2C "OLED bus"** (4 poli: SCL, SDA, GND, VCC). Il **controller del display non è indicato**: SSD1306 o SH1106 a 0x3C/0x3D → **[UNVERIFIED]**, selezionabile da configurazione (default SSD1306 @ 0x3C). Verifica: scan I2C + prova dei due driver dalla pagina di diagnostica.
- **Connettore SERIAL "BT Bus"** (TX/RX/3V3/GND): predisposizione per modulo Bluetooth seriale. Condivide la UART dei log. [SCHEMA]
- **P1** (20 poli): 1 IOE3, 2 IOE2, 3 IOE1, 4 INT, 5 REL8, 6 REL7, 7 REL6, 8 REL5, 9 REL4, 10 CD, 11 RTCBAT, 12 RTCDS, 13 RTCSQ, 14 WD4(D4), 15 WD5(D5), 16 WD0(D0), 17 WA0(A0); pin 18–20 (GND/3V3/VCC) da confermare al banco — irrilevanti per il firmware. [SCHEMA], pin 18–20 a confidenza media.
- **IPE**: jumper che porta `WRST` a GND (reset manuale/programmazione). [SCHEMA]

### 1.6 Assegnazione carichi su REL1–3 — **[UNVERIFIED], la sola vera incognita**

Lo schema non etichetta le uscite per funzione (frigo/riscaldatore). Dal manuale il sistema pilota **sia riscaldamento che raffreddamento**.
- Verifica al banco: pagina di test manuale "accendi/spegni relè N" (in milestone 1 via seriale) con i carichi scollegati, poi tester/lampadina.
- Mapping funzione→relè **configurabile da UI**; le uscite nascono **DISARMATE** (flag persistito) finché il mapping non è confermato dall'utente.

---

## 2. Requisiti funzionali (dal manuale)

Il PDF è la pagina blog ufficiale "AxHTherm V2.0" (iobrewing.net, salvata 03-09-2019). Descrive il funzionamento
del sistema originale (app Android + firmware); ciò che segue è ciò che il firmware nuovo deve coprire in locale.

### 2.1 Profilo di fermentazione
- Fino a **5 step**, avanzamento automatico al termine di ciascuno. [MANUALE]
- **4 modalità di controllo** per step: [MANUALE]
  1. **Temperatura fissa** — mantiene un valore entro una tolleranza configurabile.
  2. **Rampa a tempo** — porta la temperatura da un valore iniziale a un target entro un tempo massimo.
  3. **Rampa a step di gradi** — varia il target di *n* °C ogni ora per un periodo determinato (rampa °C/ora).
  4. **Controllo della densità** — mantiene la temperatura fissa finché la densità del mosto non raggiunge un target (richiede densimetro a immersione: nel nostro caso solo **iSpindel**, via POST HTTP su `http://IP_dispositivo/ispindel` — è lo iSpindel a pubblicare). [MANUALE]

### 2.2 Sonde
- Fino a **3 sonde DS18B20**: 1 **attiva** (controllo) + 2 **passive** (monitor, es. camera e ambiente). [MANUALE]
- Sonda attiva: **ID univoco, correzione, tolleranza, ritardo compressore frigo**. [MANUALE]
- Sonde passive: **ID e correzione**. La correzione è algebrica, sommata alla lettura. [MANUALE]
- **Tolleranza**: intervallo simmetrico attorno al set point entro cui la lettura vale come "set point raggiunto". [MANUALE]
- **Ritardo compressore**: tempo di **riposo tra 2 interventi successivi del compressore**, per prevenire accensioni ravvicinate che lo danneggerebbero. [MANUALE]

### 2.3 Regolazione
- Il sistema pilota **sia riscaldamento che raffreddamento** (relè distinti). [MANUALE]
- **PID** con parametri: **kP, kI, kD**; **finestra di attivazione** dell'uscita in ms (time-proportioning);
  **distanza sotto il set point** oltre la quale il PID non interviene e si scalda a piena potenza;
  **tolleranza sopra il set point** oltre la quale il PID non interviene e il riscaldamento è spento del tutto. [MANUALE]
- L'isteresi con tolleranza è il comportamento base sul freddo/caldo (dettagli non esplicitati nel manuale: il nuovo firmware la offre come modalità esplicita accanto al PID, come da requisiti).

### 2.4 Rete e configurazione
- **SoftAP** con IP **192.168.4.1** per la prima configurazione. [MANUALE]
- WiFi STA con **DHCP o IP statico** (IP, netmask, gateway). [MANUALE]
- Configurazione di **data/ora dell'RTC** ("Set RTC to current date and time" — modulo RTC dichiarato opzionale nel manuale, presente nello schema 2.5). [MANUALE]
- Menu device: **riavvio, format flash, salvataggio config, ripristino config, aggiornamento firmware**. [MANUALE]
- Dashboard originale: nome/ID dispositivo, versione FW, numero sonde, **free heap**, step attivi, temperature S1/S2/S3, pulsanti CONFIGURE / DEFINE STEPS / START. [MANUALE]

### 2.5 Integrazioni del firmware originale (da NON riportare nel nuovo)
- ThingSpeak (API key, channel key, channel ID), IFTTT (webhook `step_changed`, messaggio
  `[AxHTherm] {value2} Passaggio allo step {value1} avvenuto!`), BrewFather (endpoint brewpiless), Tilt (selezione colore),
  toggle "Send data through I2C/LoRaWAN". [MANUALE]
- Densimetri supportati in origine: iSpindel **e** Tilt; nel nuovo firmware resta solo iSpindel (requisito).
- Soglia di allarme batteria iSpindel presente nella UI originale (campo batteria nella pagina HYDROMETER). [MANUALE]

### 2.6 Installazione originale (utile per la Fase 3)
- Flash consigliato previo **erase_flash** con esptool (Linux/macOS) o ESP8266 Download Tool (Windows). [MANUALE]
- Lo screenshot del tool Windows mostra: **FLASH SIZE 32 Mbit (4 MB)**, SPI MODE **DIO**, con file `blank_1MB.bin`
  caricati a 0x000000/0x100000/0x200000/0x300000 → la scheda è trattata come 4 MB. [MANUALE]
- L'header di `install.bin` (`E9 01 02 40`) conferma modalità **DIO** e nibble dimensione flash = 4 MB. [BINARIO]

---

## 3. Fatti dal binario (`install.bin`, solo `strings`/`xxd`)

Analisi condotta con 5 lenti parallele (endpoint, chiavi config, librerie, hardware/sicurezza,
verifica adversariale dell'analisi precedente). Tutte le voci hanno la stringa esatta come evidenza
(riferimenti in `ANALISI_FIRMWARE_ORIGINALE.md` + risultati del workflow di questa sessione).

### 3.1 Identità e piattaforma
- **Versione firmware originale: 2.5.57** (stringa `2.5.57`; il manuale descrive la V2.0 → usarlo come guida funzionale, non come specifica esatta).
- Arduino core ESP8266 **2.5.2** (path di build `/home/carmine/.arduino15/...`), SDK NONOS **2.2.1(cfd48f3)**, board `ESP8266_WEMOS_D1MINI` (TXT record ArduinoOTA).
- Filesystem **SPIFFS** (0 occorrenze di LittleFS — coerente col core 2.5.2); config in `/config.json`; UI da `index.htm` via handler statico.
- Immagine: eboot + sketch a 0x1000, **DIO, header 4 MB/40 MHz**.
- Librerie certe: core (WiFi, WebServer, HTTPClient, httpUpdate, Updater, ArduinoOTA, mDNS), **arduinoWebSockets 2.2.1** (client *e* server). Librerie "mute" non dimostrabili dalle stringhe: ArduinoJson, OneWire/Dallas, PID, LoRa (sandeepmistry), driver display.

### 3.2 Rete: cosa serviva e chi contattava
- Endpoint locali: **`/ispindel`** (risposta letterale `gotcha`), **`/plaato`** (`gotcha_plaato`), **`/get_state`**, file statici SPIFFS, **WebSocket server locale** per la GUI (messaggi `{"action": "updateSystem"|"log"|"updateConfig"|"config/configSaved"...}`), **ArduinoOTA**.
- **Unici host esterni nel binario**: `hub.iobrewing.it` (WebSocket Phoenix: topic `axhtherm2:devices/%s`, `phx_join` con `just_booted`/`elapsed_seconds`) e `www.iobrewing.it` (`/updates/axhtherm25.bin|.version`, OTA HTTP). **Nessun NTP, nessun IP hardcoded, nessun captive portal/DNSServer, nessun MQTT.**
- **ThingSpeak/IFTTT/BrewFather NON erano contattati direttamente**: eventi WS (`thingspeak_update`, `ifttt_update`, `brewfather_update`) inoltrati all'hub con le credenziali nel payload — l'hub faceva da relay. Con il cloud morto, quelle integrazioni erano già tutte inservibili.
- Comandi ricevuti dal cloud (evento `execute`): `get_system, get_config, save_config, rtc, connection_completed, toggle_run, reset, format_flash, go_to_next_step, update_firmware, get_steps, set_current_target_temperature, set_running` → è l'elenco funzionale che la nostra API REST deve coprire in locale.
- SoftAP di fallback: SSID **`AXHTH`**, password **`DEADBEEF`**, netmask 255.255.255.0 (adiacenza forte nelle stringhe); IP 192.168.4.1 solo dal manuale (default ESP8266).
- Costante hex `86c9de269689427fb7a07fe8ae1bd583` nel blocco config: probabile api_key/uid di default (scopo non dimostrato).

### 3.3 Configurazione e stato
- Sezioni top-level di `/config.json`: `system, wifi, probes, stepRunner, pid, httpServices, thingspeak, hydrometers, lora`.
- Chiavi con doppia lingua: `correction/correzione`, `tolerance/tolleranza`, `compressorDelay/ritardoFreddo`, `nome`; varianti di case coesistenti (`setPointReached`/`setpointReached`, `tsApiKey`/`TSApiKey`…).
- Step: `typeR` (rampa) / `typeG` (densità) [semantica da adiacenza, ipotesi solida], `duration`, `endsAt`, `degreesPerHour`, `descendingRamp`, `ramp`, `targetTemperature`, **`delayedStart`/`delayedStartAt`** (partenza ritardata del profilo — feature non documentata nel manuale), `timeScaleFactor` (accelerazione del tempo, probabile uso di test).
- Regolatore: `output`, `windowSize`, `pidAggressiveLowerBound/UpperBound` (+ varianti LB/UB), `cooling`, `regulating`, `setPointReachedAtLeastOnce`, **`compressorProtectionOn`/`compressorProtectionCountdown`**. Esiste `cooling` ma **nessuna stringa `heating`** (stato caldo derivato dall'output PID). **Le chiavi `kP/kI/kD` non esistono nel binario** (guadagni hardcoded o con altri nomi; la UI del manuale li mostra con default 2 / 1 / 5).
- Orologio: chiavi `seconds, minutes, hours, day, month, year` (impostazione manuale da UI; **nessun NTP**), formato `%02u/%02u/%04u %02u:%02u:%02u`; `[SetupRTC]` presente ma il chip RTC non è identificabile dalle stringhe (lo è dallo schema: DS1307).
- Payload compatti per dispositivo esterno (LoRa/BT/widget): `%d|%.1f|%.1f|%.1f|%.1f`, `%d|%d|%d|%d|%d|%d|%.0f|%.0f`, JSON a chiavi `A..O`; chiave `sendingState`.

### 3.4 Sonde, densimetri, sicurezza
- Modulo sonde con `[Probes::setup/scan/checkProbeIDsConsistency]`, stati `[noProbesFound]`/`[probeIDsError]`; senza sonde: messaggio a display `*** NO PROBES FOUND ***` e **il boot continua dopo 10 s** (il nuovo firmware farà lo stesso: allarme + uscite in sicurezza, mai un blocco).
- iSpindel: campi parsati `temperature, gravity, battery, angle`; `iSpindelBatteryAlertThreshold`; `gravityCorrection`, `currentGravity`, `gravityUpdated`.
- **Tilt letto via modulo BLE HM-10 sul connettore seriale "BT Bus"**: sequenza AT nel binario (`AT+RENEW, AT+BAUD0, AT+ROLE1, AT+IMME1, AT+RESET, AT+SHOW1, AT+SCAN1, AT+DISI?, AT+START`) + prefisso iBeacon Tilt `:A495BB` + qualità segnale (`excellent…unusable`). Questo spiega il connettore BT dello schema. (Nel nuovo firmware: fuori scope.)
- Telemetria: `beerTemp, beerSet, fridgeTemp, roomTemp` + `extVolt, tiltValue, gravity`.
- Macchina a stati con transizioni tracciate: `[enterIdle]`, `[enterRunning]`, `[bootComplete]`.
- Messaggi display/seriale per OTA ed errori: `* DO NOT POWER OFF *`, `HTTP OTA ERROR!`, `SPIFFS: INIT ERROR`, `Reboot in 5 seconds`, `SoftAP ERROR!` (larghezza ≤ ~25 caratteri, coerente con OLED 128×64 — ipotesi).

### 3.5 Correzioni all'analisi precedente (`ANALISI_FIRMWARE_ORIGINALE.md`)
1. **PID asimmetrico**: sotto `lowerBound` → riscaldamento a piena potenza; sopra `upperBound` → riscaldamento completamente **spento** (non "piena potenza" su entrambi i lati).
2. `sketch, spiffs, board, upgrade, reconnect, local, position` **non sono chiavi di configurazione**: sono stringhe di librerie/SDK (httpUpdate, mDNS, WebSockets, File API). `smart` è una chiave delle **sonde**, non di rete.
3. Il cloud era su **iobrewing.it** (hub + updates); iobrewing.net è solo il blog del manuale.
4. Messaggio IFTTT: "**Passaggio** allo step", non "Passo" (e vive nell'applet IFTTT, non nel firmware).
5. La telemetria completa include anche `extVolt`, `tiltValue`, `gravity` (omessi).
6. `month`+`local` come "chiavi orologio": le vere chiavi sono `seconds/minutes/hours/day/month/year`.
7. ArduinoJson, OneWire/Dallas, RTC DS1307, "asset gzippati": plausibili ma **non dimostrabili** dalle stringhe (erano dati per certi).

---

## 4. Assunzioni aperte (ognuna → opzione di configurazione o verifica al banco)

| # | Assunzione | Come si scioglie |
|---|---|---|
| A1 | **Quale carico (frigo/riscaldatore) su ciascun polo di REL1–3** | **VERIFICATO AL BANCO (15/08/2026), test a clic ritmici con carichi staccati: REL1 → cavo riscaldante, REL2 → non cablato, REL3 → frigo.** Sistema armato con freddo=REL3, caldo=REL1 |
| A2 | Controller OLED: SSD1306 o SH1106, 0x3C o 0x3D | Scan I2C + toggle driver da config (default SSD1306@0x3C); nessuna evidenza né da schema né da binario |
| A3 | MCP23008 risponde a 0x20 | **VERIFICATO AL BANCO (15/08/2026): non risponde ad alcun indirizzo — sull'esemplare reale il chip non è montato.** Bus I2C funzionante (0x3C OLED, 0x50 EEPROM, 0x68 DS1307). REL4–8 non disponibili; irrilevante: i carichi stanno su REL1–3 (GPIO diretti) |
| A4 | Presenza EEPROM AT24C32 @0x50 sul modulo Tiny RTC | Scan I2C; irrilevante per il firmware (non la usiamo) |
| A5 | Flash reale da 4 MB | `esptool flash_id` in Fase 3 (header binario e tool originale dicono 4 MB) |
| A6 | Boot pulito con GPIO15 (D8/REL3) tenuto basso dall'ingresso ULN | Primo avvio al banco senza carichi; se la scheda originale bootava, è già dimostrato dall'uso reale |
| A7 | Default operativi originali (ritardo compressore, kP/kI/kD=2/1/5 da screenshot manuale) | Non estraibili dal binario: il nuovo firmware definisce i propri default, tutti configurabili |
| A8 | Pull-up OneWire a 5 V su GPIO0 (fatto hardware, non modificabile) | Nessuna azione: funzionava così; annotato per la diagnostica sonde |
| A9 | Comportamento esatto di ripresa dello step dopo blackout nel firmware originale | Non ricostruibile dalle stringhe; il nuovo firmware implementa la semantica definita in ARCHITETTURA.md §3 (epoch assoluti RTC) |
| A10 | OneWire su GPIO0 (strapping): un reset che cade dentro uno slot di traffico OneWire con la linea bassa può far entrare il chip in bootloader (probabilità ≪1 % per reset) | Limite dello schema, non mitigabile in software; in quel raro caso serve un power-cycle. Identico rischio nel firmware originale |

**Ambiente verificato**: PlatformIO 6.1.19 in venv (`~/.venvs/platformio`), platform espressif8266 4.2.1,
core Arduino 3.1.2, toolchain scaricata; scheletro `d1_mini` compilato — RAM 34,3 % (28 096 B), Flash 25,4 %
(265 399 B su 1 044 464). Progetto in `~/dev/axhtherm25`.
