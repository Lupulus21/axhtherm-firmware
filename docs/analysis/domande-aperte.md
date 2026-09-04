# AxHTherm 2.5 — Domande aperte

> Consegnate separatamente come richiesto. Sono i punti che **non** sono determinabili dai file
> presenti in repository: non vanno colmati con assunzioni plausibili.
> Aggiornato il 2026-09-02.

---

## Stato: 11 risolte su 13, 2 aperte

Le due rimaste sono **ispezioni visive sulla scheda fisica** e restano sospese finché
l'hardware non sarà sul banco. Nessuna delle due blocca lavoro firmware.

---

### ✅ Risolte ai gate e in corso d'opera

- **GATE 1** — Topologia = **due frigo separati** (camere indipendenti, nessun arbitraggio) ·
  alimentazione = **5 V dedicato** · hardware aggiuntivo **ammesso**.
- **GATE 2** — Opzione **A** (refactor parametrico su `N` camere), consegnata funzionante a
  `N=1` con l'hardware rimandato; poi portata a `N=2` e rilasciata come 2.24.0-rc2.
- **D11 (versionamento)** — `git init` autorizzato, baseline v2.23.2 nel commit `ad1b50d`.
- **D12 (esito test)** — `pio test -e native` → **81/81 PASSED**.
- **D13 (cotta attiva)** — prassi confermata e applicata a ogni flash: `GET /api/status`,
  controllo di `mode` e `profile.running` prima di qualunque riavvio.

---

## Hardware

### ✅ D1 — Corrente dell'alimentatore 5 V — **RISOLTA 2026-09-02**
**≥ 2 A dichiarati.** Il caso peggiore calcolato è ~740 mA: margine oltre il doppio, valido
qualunque tipo di moduli relè si scelga. Resta consigliato l'elettrolitico di bulk, ma come
prudenza contro i transitori di commutazione, non come necessità di consumo medio.

### ⏳ D2 — Il footprint/zoccolo DIP-18 del MCP è saldato sulla scheda? — **APERTA**
BOM e CPL lo prevedono, ma `docs/ANALISI.md:188` afferma solo che *il chip* non è montato.
Serve un'**ispezione visiva**: ci sono i fori metallizzati e le piste, o manca anche quello?
→ *Sospesa: l'hardware non verrà montato ora.* Non blocca firmware.

### ⏳ D4 — Revisione della scheda — **APERTA**
Il PDF rev 1.0 e il layout EasyEDA pubblicato **divergono**: `POWER` a 2 o 3 poli, presenza del
jumper `UPWR`, ordine dei pin di P1. Da questo dipende **da dove si preleva il comune bobine**.
→ *Ispezione visiva: il morsetto POWER ha 2 o 3 poli? C'è un jumper siglato `UPWR`?*
→ *Sospesa insieme a D2.* Non blocca firmware.

> ⚠️ **Regola operativa che vale comunque**: sul connettore P1 fidarsi **solo dei nomi
> serigrafati**, mai dei numeri di `docs/ANALISI.md:74`, che sono errati rispetto al layout.

### ⚠️ D3 — Moduli relè: auto-alimentati o presi dai 5 V della scheda? — **NON ANCORA SCELTI**
Non ancora acquistati. **Raccomandazione registrata nel BOM delta**: sceglierli **opto-isolati
con `JD-VCC` separato**. Due motivi, entrambi concreti:
1. toglie 280-360 mA di bobine dal rail dell'MCU e con essi i transitori di commutazione,
   su una scheda il cui unico condensatore è `C1 = 47 pF`;
2. **elimina il rischio R3**: oggi il comune dei diodi di ricircolo (`CD`) è una rotaia unica
   condivisa da tutte le bobine di *entrambe* le camere, senza separazione elettrica.

Se invece si useranno moduli alimentati dalla scheda: bobine della **stessa tensione**
obbligatorie, oppure jumper `UPWR` aperto e `CD` portato dall'esterno.

### ✅ D5 — Contatti NO o NC? — **RISOLTA 2026-09-02, per misura**
**NO — bobina diseccitata = carico spento.** Non per ispezione ma per analisi dei 30 giorni di
registro in `backups/fs-2026-09-01-post-2.24.0-rc2/ch1-registro-720h.csv`:

- Le due uscite sono **entrambe a riposo nel 93 % dei campioni** (2426/2605).
- In quel 93 % la camera sta a **28,4 °C di mediana** (min 25,0 al 5º percentile), cioè ad
  ambiente, con sonda di controllo e ausiliaria che concordano entro **0,77 °C** di mediana.

Se `REL3` fosse NC il frigo girerebbe quasi sempre e la camera starebbe a temperatura di
frigorifero; se `REL1` fosse NC il cavo riscaldante scalderebbe quasi sempre e la sonda di
controllo, che gli sta vicino, si staccherebbe dall'ausiliaria. **Non accade né l'uno né
l'altro.** Entrambe le ipotesi NC sono refutate dai dati.

> **Perché contava**: l'intera catena di stato sicuro — `safeInitEarly`, ULN2803 non
> invertente, reset dell'MCP via `WRST`, e il failsafe hardware che scrive `GPOC` sui pin
> relè — assume *diseccitato = spento*. Con contatti NC ogni protezione si sarebbe rovesciata
> e il failsafe sarebbe diventato un **amplificatore di guasto**: durante uno stallo avrebbe
> acceso compressore e riscaldante insieme, lasciandoli accesi fino al reset.
> Ora è un **fatto misurato**, non un'assunzione implicita.

### ✅ D6 — Lunghezza e tipo del bus OneWire — **RISOLTA 2026-09-02**
**Frighi affiancati, tratte sotto i 2 m.** Un solo bus regge senza problemi: le sonde della
camera 2 vanno in parallelo sul morsetto PROBE, nessuna modifica hardware né firmware.
L'ipotesi di un secondo bus dedicato su GPIO14 **non serve** e resta archiviata.

---

## Scelte di progetto

### ✅ D7 — Densimetro: legame con la camera — **IMPLEMENTATA 2026-09-02 (2.24.0-rc3)**
**Uno per camera, legato per nome.** Con un solo iSpindel la camera 2 resta senza densimetro e
i suoi step a densità si bloccano in modo esplicito, invece di leggere il valore sbagliato.

> 🔴 **Non è una preferenza: chiude un difetto latente.** Oggi `src/main.cpp:539` passa
> `gravity.gravity()` — un tracker **unico di dispositivo** — al motore di profilo di
> *entrambe* le camere. L'OG è già per camera (`pstate.ch[idx].ogGravity`, riga 544) ma la
> densità corrente no, e la cattura automatica dell'OG è cablata alla sola camera 0
> (riga 332). Uno step «attesa densità» o «attesa attenuazione» sulla camera 2 avanzerebbe
> leggendo il densimetro della camera 1: una camera passerebbe a cold crash con la
> fermentazione ancora in corso. Non si manifesta finché la camera 2 usa solo step a tempo o
> a rampa.
>
> **Fatto**, e più ampio di come l'avevo previsto. La ricognizione avversariale ha mostrato
> che l'instradamento per camera **da solo non bastava**: `ProfileEngine::stepDone`
> (`lib/axcore/src/ax/ProfileEngine.cpp:209`) valuta il timeout dello step **prima** della
> validità del dato, e tutti e nove gli step a densità della libreria reale hanno un limite
> fra 2 e 7 giorni. Una camera senza densimetro sarebbe avanzata comunque, al cold crash,
> senza misurare e senza allarme. Consegnato: tracker e taratura per camera, instradamento a
> una sola camera per lettura (con le letture anonime che continuano ad arrivare alla camera 1,
> perché il Tilt documentato non manda `name`), `TickInput::gravitySourceMissing` che
> disabilita la scorciatoia del timeout, rifiuto 409 all'avvio, azzeramento del tracker alla
> riassegnazione, e una riga per camera nell'interfaccia. Dettaglio in `CHANGELOG.md`.

### ✅ D8 — Alias Home Assistant — **RISOLTA in Fase 3**
La camera 1 mantiene i topic e gli `entity_id` storici; la camera 2 ha il suffisso. Le
automazioni esistenti non si rompono.

### ✅ D9 — Layout OLED — **RISOLTA in Fase 3**
Pannello unico con marcatore `C1`/`C2` sulla riga di modalità (`src/svc/OledUi.cpp:74-77`).
A camera singola il pannello resta **identico alla v2.23.2**, senza indicatori che non
servirebbero a nulla.

### ✅ D10 — Gate del test relè — **RISOLTA in Fase 3: globale**
Il test manuale è vietato con **qualunque** camera armata (`src/hal/OutputBus.h:74`). Scelta
conservativa deliberata: gli 8 canali fisici sono una risorsa di **dispositivo**, non di
camera (`OutputBus.h:25`), e un test sulla camera A mentre la B fermenta manipolerebbe una
risorsa condivisa.

---

## Rilievi emersi dall'analisi del registro (2026-09-02)

### ✅ Riscaldante inerte il 25-26/08 — **SPIEGATO: prova a banco senza carico**
Per 14 ore (25/08 21:43 → 26/08 11:47), camera armata, setpoint 35 °C, il firmware ha
comandato caldo su **124 campioni consecutivi** e la temperatura è **scesa** da 29,2 a
24,8 °C, seguita dall'ausiliaria. L'allarme di scostamento termico è scattato su 92 campioni.

**Il firmware si è comportato correttamente**: ha chiesto calore, ha rilevato che non
arrivava, e dopo tre ore ha alzato l'allarme — che è tutto ciò che può fare, non potendo
correggere un guasto a valle.

Causa confermata dall'utente: **prova a banco, cavo riscaldante non collegato**. Coerente con
il resto del registro (il 26/08 alle 17:28 le sonde leggono 1,00 e 0,25 °C, tipico distacco al
banco). **Nessun difetto firmware.** → Aggiunta al collaudo una prova di accettazione del
riscaldante, perché con una birra dentro 14 ore a 25 °C invece di 35 sono una fermentazione
Kveik andata storta, e il firmware può solo segnalarlo.
