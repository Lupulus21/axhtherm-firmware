# AxHTherm 2.5 — Studio di fattibilità a 2 camere (FASE 2)

> Analisi **read-only**. Presuppone il report forense in `docs/analysis/forensic-report.md`.
> Premessa dichiarata dall'utente: **due frigoriferi separati**, ciascuno con compressore e
> riscaldante propri; alimentatore 5 V dedicato; hardware aggiuntivo ammesso.
> Prodotto il 2026-08-31.

---

**Premessa dalle risposte al GATE 1:** **due frigoriferi separati**, ciascuno con compressore e
riscaldante propri; alimentatore 5 V dedicato; ammesso hardware aggiuntivo.

Questo semplifica radicalmente il problema: **le camere sono termicamente e meccanicamente
indipendenti**. Non serve arbitraggio, non serve coda di priorità, non c'è camera che ruba freddo
all'altra. L'opzione "condivisione del gruppo frigo" e quella "multiplexing temporale degli
attuatori" — che avrei dovuto valutare con un frigo solo — **non sono applicabili** e le escludo
esplicitamente invece di ometterle.

## 2.1 — Opzioni architetturali

### Opzione A — Contesto per camera su una sola scheda (`N_CHAMBERS` parametrico) ⭐

Un `ChamberCtx` che raccoglie `ProfileEngine` + `ControlPipeline` + `SafetyMonitor` per-camera +
i timer d'allarme oggi `static` + indici sonda + coppia di uscite; array `ChamberCtx ch[N]`.

| Aspetto | Valutazione |
|---|---|
| **Impatto codice** | Alto ma **strutturalmente favorevole**: il motore (`lib/axcore`) è già a oggetti senza stato globale — si istanzia due volte **senza modificarlo**. Il lavoro è tutto in `main.cpp`, negli store e nella UI |
| **Impatto hardware** | **Montare il MCP23008-E/P** nel footprint DIP-18 già cablato (~2 €), + 1 canale relè esterno, + 1÷3 sonde DS18B20. **Nessuna modifica al PCB** |
| **Comportamento termico** | Camere indipendenti: nessuna interferenza da modellare |
| **Rischi** | ① heap (~17,6 KB) e JSON di config/stato; ② un solo `loop()` cooperativo serve entrambe (nessun isolamento temporale); ③ un guasto I2C tocca la camera 2; ④ bus OneWire più lungo |
| **Sforzo** | Il maggiore, ma è l'unico che dà **una UI, una libreria profili, un log, un dispositivo** |

**Allocazione uscite raccomandata** (mantiene entrambi i compressori su GPIO diretti):

| | Freddo | Caldo |
|---|---|---|
| **Camera 1** | REL3 — GPIO15 diretto *(già cablato al frigo)* | REL1 — GPIO12 diretto *(già cablato al riscaldante)* |
| **Camera 2** | **REL2 — GPIO13 diretto** *(oggi libero)* | **REL4 — via MCP23008** |

Il razionale è di sicurezza: se l'I2C cade, la camera 2 perde **il caldo, non il freddo**. Una
birra che non viene riscaldata rallenta; una birra che non viene raffreddata si rovina o esplode il
gorgogliatore. E `OutputBus::mcpNeeded()` (`OutputBus.h:74`) esiste già per distinguere il caso.

### Opzione B — Due controllori indipendenti (una scheda per camera)

Si produce una seconda scheda (Gerber pronti, ~2-5 $ per 5 pezzi) e si esegue il firmware
**attuale, invariato**, con hostname e base-topic MQTT diversi.

| Aspetto | Valutazione |
|---|---|
| **Impatto codice** | **Praticamente nullo** |
| **Impatto hardware** | Una PCB + WEMOS + ULN2803 + modulo RTC + alimentatore + morsetti (~15-20 €) |
| **Rischi** | **I più bassi in assoluto: isolamento totale dei guasti.** Nessun rischio heap, nessuna regressione sul firmware già indurito, nessun single point of failure condiviso |
| **Contro** | Due IP, due UI, due configurazioni, due OTA; **libreria profili non condivisa**; nessuna vista d'insieme; due orologi da tenere sincronizzati |
| **Sforzo** | Minimo lato software, maggiore lato assemblaggio |

**Va detto onestamente: dal solo punto di vista safety questa è l'opzione migliore.** Il costo è
ergonomico e gestionale, non tecnico.

### Opzione C — Migrazione a ESP32

Risolve heap, GPIO e RAM in un colpo solo, e `lib/axcore` (C++ puro, nessuna dipendenza Arduino)
si porta **invariato**. Ma butta via tutto l'HAL, il mapping hardware, la PCB, e soprattutto
**invalida la campagna di hardening** che era mirata a comportamenti specifici dell'ESP8266
(frammentazione heap, stack da 4 KB, soft-WDT). **Sproporzionata** rispetto all'obiettivo:
la sconsiglio, e la elenco solo per completezza.

### Raccomandazione

**Opzione A**, con un **cancello di verifica misurabile**: completato il refactor a contesto
per-camera con `N_CHAMBERS=1` (comportamento invariato), si misura il free heap sul dispositivo
via `/api/status`. **Se scende sotto ~10 KB, si ripiega sull'Opzione B** invece di forzare.
Il numero non è arbitrario: oggi sono ~17,6 KB e il picco di una `PUT /api/config` ne chiede ~6.

## 2.2 — Gap analysis hardware

Fabbisogno per 2 camere: **4 uscite relè**, **≥4 sonde**, **1 bus I2C**, **1 alimentazione**.

| Categoria | Risorsa | Dettaglio |
|---|---|---|
| ✅ **Già disponibile** | Canali ULN2803 | 8 canali, non invertenti; 4 usati su 8. Con 4 bobine da ~80 mA e V<sub>CE(sat)</sub> ~1,1 V la dissipazione è ~0,35 W, ben sotto il limite del DIP-18 |
| ✅ | Indirizzi I2C | Occupati 0x20/0x3C/0x50/0x68. Liberi in abbondanza |
| ✅ | Bus OneWire | Regge N sonde per indirizzo ROM; le nuove vanno in parallelo sul morsetto PROBE |
| ✅ | Flash sketch | 716.664 / 1.044.464 B → **31 % libero** |
| ✅ | LittleFS | ~1,95 MB, con il log già limitato a 1,4 MB (`DataLog.h:25`) → ~0,5 MB per config/stato/profili |
| ✅ | Failsafe hardware | Il RESET dell'ESP resetta anche il MCP → GP in Hi-Z → ULN spento → relè rilasciati |
| ⚠️ **Da riassegnare / firmware** | REL2 (GPIO13) | Fisicamente presente e **non cablato**: diventa il freddo della camera 2. Solo cablaggio |
| ⚠️ | `ProbeBus::kProbes = 3` | Limite **software**, non hardware. Va portato a 6 (~+363 B RAM) |
| ⚠️ | Ruolo sonda ↔ indice | `control()` ritorna `st_[0]` cablato: serve mappa ruolo→indice per camera |
| ⚠️ | Jumper `UPWR` / rete `CD` | Il comune dei diodi di ricircolo è **unico per tutti gli 8 canali**: le bobine delle due camere devono avere la **stessa tensione**, o `UPWR` va aperto e `CD` portato dall'esterno |
| ⚠️ | Pull-up I2C | **Non ci sono sul PCB**: vivono sui moduli RTC/OLED. Da non peggiorare |
| ❌ **Mancante** | **MCP23008-E/P** | **Non montato** (verificato al banco). Senza, esistono 3 uscite e ne servono 4. Gli ingressi ULN I4-I8 sono hardwired ai suoi GP0-GP4 e **non raggiungibili altrimenti** |
| ❌ | 4° canale relè esterno | I relè sono tutti esterni: serve un canale in più |
| ❌ | Sonde DS18B20 | Almeno 1 (controllo camera 2); consigliate 2 (controllo + camera) |
| ❌ | **Condensatore di bulk sulla 5 V** | **C1 = 47 pF è l'unico condensatore della scheda.** Con 4 bobine commutate il rail dell'ESP è esposto ai transitori → rischio brownout/reset |
| ❌ | Margine alimentatore | **Da verificare** (vedi calcolo sotto) |

### Margine dell'alimentatore con due carichi attivi

| Carico | Corrente |
|---|---|
| ESP8266 in TX WiFi (picco) | ~350 mA |
| OLED 128×64 | ~20 mA |
| DS1307 + EEPROM + MCP23008 | ~5 mA |
| **4 bobine relè da 5 V** (tipiche 70-90 mA/canale) | **280-360 mA** |
| **Totale caso peggiore** | **~700-740 mA** |

→ **Un 5 V/1 A è marginale** (margine <30 %, e i transitori di commutazione non sono nel conto).
**Raccomando 5 V / 2 A** e **l'aggiunta di un elettrolitico da 470-1000 µF** vicino al morsetto
POWER. Se i moduli relè hanno alimentazione propria (opto-isolati con `JD-VCC` separato) il
problema sparisce: **è la configurazione da preferire**, perché toglie anche i transitori di
bobina dal rail dell'MCU.

> Il dato di targa del tuo alimentatore resta da confermare: il calcolo sopra è parametrico e
> l'ho svolto sul caso peggiore, non su un valore che non conosco.

## 2.3 — BOM delta

| # | Componente | Q.tà | Perché | Innesto |
|---|---|---|---|---|
| 1 | **MCP23008-E/P, DIP-18** | 1 | Sblocca REL4-REL8. **Senza, il progetto non ha abbastanza uscite** | Footprint `MCP` già cablato (0x20). Consiglio uno zoccolo DIP-18 |
| 2 | Canale relè esterno (o modulo 4 canali) | 1÷4 | 4ª uscita di potenza | REL4 su **P1, pad 12** (leggere la **serigrafia**, non i numeri di `ANALISI.md`) |
| 2b | **Adattatore/breakout per P1** (femmina 2,54 mm → morsettiera a vite) | 1 | ⚠️ **REL4-8 escono SOLO su un pettine maschio 2,54 mm, non su morsettiera.** Non è un punto di attestazione per cavi di potenza | Su P1 |
| 3 | **DS18B20** | 1÷3 | Sonda di controllo camera 2 (+ ausiliaria) | In parallelo sul morsetto **PROBE** (VCC/GND/dato) |
| 4 | **Elettrolitico 470-1000 µF / 10 V** | 1 | Serbatoio sulla 5 V, oggi assente | Fra VCC e GND al morsetto POWER |
| 5 | Alimentatore 5 V ≥ 2 A | 0÷1 | Solo se l'attuale è < 1,5 A | Morsetto POWER |
| 6 | Zoccolo DIP-18 | 1 | Montaggio non distruttivo del MCP | — |

**Costo indicativo: 10-25 €. Nessuna revisione del PCB è necessaria.**

## 2.4 — Rischi residui dell'Opzione A (da portare in Fase 4)

Emersi dal giro avversariale; li elenco ora perché **cambiano il progetto**, non solo la verifica.

| # | Rischio | Fonte | Mitigazione proposta |
|---|---|---|---|
| R1 | **Riscaldanti sincronizzati.** `nowMs % windowMs` sincronizza le finestre PID delle due camere → accensione simultanea a ogni finestra | `PidController.h:44` | ✅ **Fatto** (`setPhaseFraction`, commit `4d46361`). ⚠️ **Mitigazione parziale per costruzione**: a duty ≥99 % `windowOutput` ignora la fase e ritorna sempre ON, quindi due camere a piena potenza restano accese insieme — inevitabile, non si multiplexa il 100 %. **L'alimentatore va comunque dimensionato per tutti i carichi contemporanei** (il calcolo di §2.2 lo fa già) |
| R2 | **Nessun failsafe su stallo del loop.** Durante uno stallo i relè restano latchati; con 2 camere l'esposizione raddoppia | residuo dichiarato | Valutare un WDT hardware esplicito, o accettarlo documentandolo (com'è oggi) |
| R3 | **`CD+` è la rotaia comune di TUTTE le bobine**, di entrambe le camere: nessuna separazione elettrica | schema, ULN pin 10 | Bobine con la stessa tensione, o moduli relè auto-alimentati (opto-isolati) |
| R4 | **Il RESET dell'MCP è legato a quello dell'ESP** (rete `WRST`): ogni riavvio (OTA, `riavvia`, WDT) **rilascia in hardware tutti i REL4-8** | `docs/ANALISI.md:41` | Argomento in più per tenere il **freddo** su GPIO diretti |
| R5 | **GPIO0 è strapping e porta il OneWire.** Passare da 3 a 6 sonde **allunga la finestra** in cui la linea è bassa durante un reset → cresce il rischio (dichiarato ≪1 %/reset) di entrare in bootloader | `docs/ANALISI.md:195` (A10) | Non mitigabile in software. Da conoscere e sorvegliare |
| R6 | **Usura flash raddoppiata.** La stima dichiarata (≈210 k scritture/anno, margine 10 anni) è per **una** cotta | `ARCHITETTURA.md:197-200` | Checkpoint per-camera solo su transizione; valutare cadenza differenziata |
| R7 | **Nessun LED per la camera 2.** I 3 LED sono legati 1:1 a REL1/2/3 e il blink software è vietato | schema | Segnalazione solo via OLED/web |
| R8 | **Zero test con due istanze simultanee.** I 74 test provano il comportamento di **una** istanza; le uniche seconde istanze simulano un riavvio in sequenza | `test_main.cpp` | Nuovi test a 2 istanze sono un requisito, non un extra |
| R9 | **L'invariante di mutua esclusione è dichiarata a livello di DISPOSITIVO**, non di camera | `ARCHITETTURA.md:29-30` | Va riformulata per-camera e il documento aggiornato |
| R10 | **Sessione e credenziale uniche**; un solo nome mDNS | `ARCHITETTURA.md:8-11, 250` | Accettabile (utente singolo), ma da dichiarare |
| R11 | **Entity_id Home Assistant piatti** (`sensor.axhtherm_*`): le automazioni documentate si romperebbero | `docs/HOME_ASSISTANT.md` | Mantenere alias per la camera 1, aggiungere suffisso per la 2 |

---
