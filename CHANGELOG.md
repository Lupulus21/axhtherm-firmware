# CHANGELOG — AxHTherm 2.5

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

*Il dettaglio completo, con scenari di innesco, è in `docs/analysis/fase4-passi1-4.md`,
`fase4-passi5-7.md` e `audit-sicurezza.md`.*

### Cosa resta da verificare sul dispositivo

1. Che a una camera il comportamento sia **davvero** identico (finora provato per costruzione).
2. Che la **migrazione** di `/state.json` v1→v2 funzioni su un file reale.
3. Il **free heap effettivo**: è l'unico numero del cancello di verifica che è stato stimato e
   non misurato. Atteso ≥ 10 KB; sotto quella soglia il piano prevede di ripiegare su due
   controllori separati.
4. Il **failsafe su timer** non è mai scattato su hardware, e la sua verifica di vitalità
   non è mai stata osservata (al primo avvio il log deve restare muto: nessun allarme
   "failsafe non attivo").

Procedura completa in `docs/COLLAUDO_2CAMERE.md`.

---

## 2.23.2 e precedenti

Vedi `docs/revisione-2026-08-23/CORREZIONI.md` per la campagna di revisione avversariale che ha
portato il firmware alla qualità di produzione a una camera.
