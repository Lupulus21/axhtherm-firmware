# Prompt — Revisione forense di AxHTherm 2.5

> Documento da usare come prompt. Non è documentazione del progetto: è l'incarico che si
> consegna a chi esegue la revisione. Va passato integralmente, senza riassumerlo.

---

## 0. Contesto operativo — cosa significa "guasto" qui

AxHTherm 2.5 è il firmware di un controllore di fermentazione della birra su ESP8266
(WEMOS D1 mini, 80 KB di RAM utile, ~1 MB di flash per il programma). Non è un servizio
web: è un dispositivo che **comanda 230 V** dentro un'abitazione, attraverso due relè —
un frigorifero e un cavo riscaldante da terrario — e che resta acceso **settimane senza
sorveglianza**.

Le conseguenze di un difetto, in ordine di gravità:

1. **Danno alle persone o alle cose.** Relè che restano chiusi, riscaldatore acceso senza
   controllo, compressore avviato a ripetizione fino a bruciarlo.
2. **Perdita della cotta.** Una fermentazione dura da due settimane a sei mesi. Una
   temperatura sbagliata per un giorno rovina 23 litri di birra e settimane di lavoro; un
   profilo che si blocca su uno step la rovina in silenzio, senza allarmi.
3. **Perdita di dati.** Configurazione, profili, registro delle temperature, stato di
   ripresa dopo un blackout.
4. **Compromissione del dispositivo.** È esposto sulla LAN domestica con un server web,
   un aggiornamento OTA e un endpoint non autenticato.

Un difetto che produce (1) o (2) è un difetto grave anche se la probabilità è bassa,
perché il dispositivo lavora **non sorvegliato**: nessuno se ne accorge mentre accade.

### Vincoli della piattaforma che vanno tenuti a mente sempre

- `millis()` è `uint32_t` e **traborda dopo ~49,7 giorni**; il dispositivo è pensato per
  restare acceso più a lungo. Ogni sottrazione, ogni scadenza, ogni sentinella va guardata
  con questo in mente.
- Esecuzione **single-thread cooperativa**: gli handler HTTP girano dentro `loop()`. Non ci
  sono thread, ma ci sono **rientranze e stalli**: un handler lento ferma il controllo.
- **Watchdog** hardware e software: un blocco lungo resetta il dispositivo.
- Heap misurato sul campo: **~19-20 KB liberi**, frammentazione ~14%. `String` di Arduino e
  `DynamicJsonDocument` allocano sull'heap; lo stack degli handler web è profondo.
- LittleFS su flash: le scritture consumano cicli e possono fallire; un'interruzione a metà
  scrittura corrompe il file.
- L'orologio può essere **non attendibile** (RTC assente o scarico, NTP non sincronizzato) e
  può **saltare** quando si sincronizza.

---

## 1. Missione

Portare questa codebase a **qualità di produzione**. Definizione operativa, non retorica:

> Il dispositivo può restare acceso e armato per sei mesi, gestendo una fermentazione, senza
> che il proprietario debba controllarlo; e ogni modo in cui può fallire è o impedito, o
> reso innocuo, o reso **visibile** a chi guarda l'interfaccia.

L'ultima clausola è la più importante e la più trascurata: **un guasto che il dispositivo
non sa raccontare è peggio di un guasto che si ferma**.

La revisione deve produrre l'elenco completo, ordinato e verificato di ciò che oggi separa
la codebase da quella definizione. Non deve produrre patch (a meno che non venga chiesto in
una fase successiva): deve produrre **diagnosi difendibili**.

---

## 2. Regole di ingaggio

Queste regole valgono per ogni agente, in ogni fase. Sono vincolanti.

1. **Non parlare di codice che non hai letto.** Ogni rilievo cita `percorso/file.cpp:riga` e
   riporta testualmente le righe rilevanti. Un rilievo senza riferimento verificabile viene
   scartato senza discussione.
2. **Ogni rilievo deve essere falsificabile.** Deve contenere uno *scenario di guasto*:
   stato iniziale concreto, sequenza di eventi, conseguenza osservabile. «Potrebbe causare
   problemi» non è uno scenario. «Con `maxDurationSec = 0` e nessun densimetro collegato, lo
   step non avanza mai e il profilo resta fermo sul setpoint corrente a tempo indeterminato,
   senza allarme» lo è.
3. **Niente stile.** Non sono rilievi: la formattazione, i nomi delle variabili, la
   preferenza per un costrutto, «sarebbe più elegante», «si potrebbe usare`std::optional`».
   La sola eccezione è quando la forma **causa** il difetto (per esempio un'unità di misura
   che non compare nel nome e viene confusa).
4. **Distingui ciò che hai verificato da ciò che hai dedotto.** Se una conclusione dipende
   dal comportamento di una libreria di terze parti, apri la libreria sotto
   `.pio/libdeps/d1_mini/` e leggila. Se dipende dall'hardware, dillo e marca il rilievo come
   non verificabile staticamente.
5. **Dichiara i tuoi buchi.** Ogni agente chiude il proprio referto con la sezione
   `NON VERIFICATO`: cosa non ha potuto controllare e perché. Un referto senza quella sezione
   è incompleto per definizione.
6. **Non inventare per riempire.** Un asse d'indagine che non produce rilievi è un risultato
   legittimo e prezioso. Un rilievo fabbricato costa più di un rilievo mancato, perché
   consuma la fiducia in tutti gli altri.
7. **Quando due letture del codice sono entrambe compatibili con i fatti, dillo** invece di
   sceglierne una. La scelta prematura fra due spiegazioni compatibili è l'errore che questo
   progetto ha già pagato più caro (§4, archetipo 1).

---

## 3. Rubrica di severità

Ancorata alle conseguenze del §0, non all'impressione.

| Livello | Criterio |
|---|---|
| **critico** | Può danneggiare persone o attrezzatura, o rovinare una cotta, o rendere il dispositivo inutilizzabile/compromissibile da remoto. Oppure: rende invisibile un guasto di questa classe. |
| **grave** | Rovina un dato che l'utente non può ricostruire (configurazione, profili, registro), oppure fa prendere una decisione sbagliata mostrando un numero falso, oppure blocca una funzione essenziale in una condizione plausibile. |
| **minore** | Conseguenza reale ma recuperabile dall'utente, o confinata a un percorso raro. |

Ogni rilievo porta anche una **confidenza**: `certo` (ho eseguito il percorso o l'ho
tracciato riga per riga), `probabile` (la lettura del codice lo implica ma non l'ho
eseguito), `da verificare sul campo` (dipende dall'hardware o dai tempi reali).

Un rilievo `critico` con confidenza `da verificare sul campo` deve indicare **l'esperimento
che lo deciderebbe**, eseguibile con gli strumenti disponibili (console seriale, API HTTP,
registro CSV).

---

## 4. Priors — gli archetipi che questa codebase ha già prodotto

Questa non è una lista generica di *code smell*. Sono i difetti **realmente trovati in questo
progetto** nelle revisioni precedenti, ciascuno già costato una correzione. Sono le forme che
questo codice tende ad assumere quando sbaglia: cercatele per prime, e cercatele **ovunque**,
non solo dove sono già state trovate.

1. **Due spiegazioni compatibili, una sola verificata.** Un'analisi statistica attribuì i
   guasti di lettura a una sonda difettosa; era invece l'ordine di lettura fisso a penalizzare
   sempre il primo della lista. Il dato era compatibile con entrambe e non fu messo alla prova.
   → *Ovunque un componente sia «quello che si guasta sempre», chiedersi cosa lo distingue
   strutturalmente dagli altri.*
2. **Ordine fisso che penalizza sempre lo stesso attore.** Le sonde erano lette sempre in
   ordine 0,1,2, e l'indice 0 è quello che pilota i relè.
3. **Margine troppo stretto su un vincolo hardware.** Attesa di 800 ms su una conversione
   dichiarata a 750 ms: 50 ms di margine, pagati sempre dallo stesso.
4. **Il rimedio che nasconde il sintomo.** Un ritentativo che non contava il primo
   fallimento: la sonda malandata appariva perfetta e l'antirimbalzo passava di fatto da 3 a
   6 fallimenti. → *Ogni tolleranza a un errore deve lasciare traccia dell'errore tollerato.*
5. **Invariante rispettata ovunque tranne che in un punto.** «Prima la scrittura su flash,
   poi il motore» era documentata due volte e violata in un solo handler, dove un errore di
   salvataggio lasciava la fermentazione senza controllo, in silenzio.
6. **Stato applicato prima della validazione.** Gli step venivano caricati nel motore prima
   del controllo sul nome: una richiesta rifiutata con 400 lasciava RAM e flash divergenti.
7. **Azzeramento distruttivo su un percorso frequente.** Riconfigurare una sonda azzerava la
   sua lettura, e la riconfigurazione avveniva a **ogni** salvataggio delle impostazioni:
   falso guasto della sonda di controllo, compressore fermato fuori dal tempo minimo.
8. **Guardia di sicurezza fail-closed dove serviva fail-open.** Un blocco dell'armamento
   senza rilascio e senza visibilità avrebbe impedito di raffreddare una cotta in corso: il
   rimedio era più pericoloso del rischio.
9. **Scadenza mai azzerata più aritmetica di `millis()`.** Un timestamp di scadenza che
   invecchia indefinitamente esce dalla finestra in cui il confronto con segno è valido:
   dopo ~25 giorni di uptime la guardia si autoattivava per settimane.
10. **Compensazione applicata dopo l'evento che doveva correggere.** Il rilevamento di stallo
    girava a valle della lettura e spostava i timestamp **nel futuro**: la sottrazione senza
    segno traboccava e dichiarava guaste tutte le sonde.
11. **Stato condiviso dove serviva per-entità.** Una finestra di grazia unica per tre sonde:
    la lettura riuscita di una chiudeva la grazia a un'altra ancora in difficoltà.
12. **Conversione di unità applicata a metà.** Passando da densità in SG a °Plato veniva
    convertita la banda di stabilità ma non la soglia: cancelli che non si sarebbero mai chiusi.
13. **Limite misurato nell'unità sbagliata, e troncamento silenzioso.** 32 *caratteri* nella
    pagina contro 32 *byte* nel firmware, con troncamento muto: nomi diversi da quelli scritti
    e collisioni invisibili.
14. **Passo senza uscita in una macchina a stati.** Una rampa a °C/ora avanza solo quando la
    temperatura misurata entra in tolleranza e **non ha timeout**: se l'impianto non ci arriva,
    il profilo resta appeso per sempre.
15. **Default pericoloso.** `maxDurationSec = 0` significa «nessun timeout» ed era il valore
    predefinito di uno step nuovo; un campo assente nella richiesta significava «tutte le sonde».
16. **L'errore che finge successo.** Una lettura fallita dell'elenco profili restituiva un
    elenco vuoto: il salvataggio successivo avrebbe sovrascritto un profilo esistente.
17. **Numeri fabbricati nell'interfaccia.** «Ultimo valore noto 0.00» per una sonda mai letta;
    «0,00%» di letture perse su zero letture; una finestra statistica dichiarata pari
    all'uptime dopo un azzeramento.
18. **Il testo promette una cosa, il codice ne fa un'altra.** Nomi e descrizioni di profili
    che annunciavano temperature diverse da quelle realmente eseguite.
19. **GET con effetti collaterali.** Una richiesta di sola lettura che apriva una finestra di
    blocco lato server.
20. **Risposta costruita per concatenazione senza escaping**, con dentro testo scritto
    dall'utente.
21. **Riferimento non invalidato quando cambia l'oggetto.** Il piano del profilo in esecuzione
    restava quello vecchio se la cotta veniva cambiata da un altro client.

Per ciascun archetipo la domanda da porsi è: **dove altro, in questa codebase, esiste la
stessa forma?**

---

## 5. Ledger di copertura — nessun file può restare fuori

«Revisione completa» è una promessa verificabile solo se esiste l'elenco di ciò che andava
guardato. Ogni file qui sotto deve risultare **coperto** da almeno un agente, e ogni agente
dichiara nel referto quali file ha letto **per intero** e quali solo in parte.

**Nucleo di controllo — `lib/axcore/` (logica pura, testabile a banco)**
`ax/Types.h` · `ax/TimeUtil.h` · `ax/ProfileEngine.h` · `ax/ProfileEngine.cpp` (278) ·
`ax/ControlPipeline.h` (156) · `ax/CompressorGuard.h` · `ax/HysteresisController.h` ·
`ax/PidController.h` · `ax/Interlock.h` · `ax/SafetyMonitor.h` · `ax/PeakEstimator.h` (123) ·
`ax/GravityTracker.h`

**Hardware — `src/hal/`**
`ProbeBus.h` (327) · `OutputBus.h` · `Mcp23008.h` · `RtcClock.h`

**Servizi — `src/svc/`**
`WebApi.cpp` (1434) · `WebApi.h` · `MqttSvc.cpp` (261) · `MqttSvc.h` · `ConfigStore.cpp` (209) ·
`ConfigStore.h` · `ProfileStore.cpp` (152) · `ProfileStore.h` · `StateStore.cpp` · `StateStore.h` ·
`DataLog.cpp` · `DataLog.h` · `CalibStore.cpp` · `CalibStore.h` · `DeviceRegistry.cpp` ·
`DeviceRegistry.h` · `BootLog.cpp` · `BootLog.h` · `TimeService.cpp` · `TimeService.h` ·
`NetworkManager.cpp` · `NetworkManager.h` · `OledUi.cpp` · `OledUi.h` · `AppConfig.h` · `AppStatus.h`

**Applicazione e piattaforma**
`src/main.cpp` (751) · `include/hardware_map.h` · `platformio.ini` · `tools/gzip_assets.py`

**Interfaccia** — `data-src/index.html` (2265 righe: HTML, CSS e JavaScript in un unico file,
compresso in PROGMEM al build). Va trattata come codice a tutti gli effetti: contiene la
logica di presentazione dello stato, i calcoli di avanzamento, la libreria dei 49 modelli di
fermentazione e tutte le conferme che precedono le azioni distruttive.

**Test** — `test/test_core/test_main.cpp` (1122 righe, 59 casi). Da rivedere per ciò che
**non** copre.

Totale: 45 file sorgente, ~5.855 righe C++, 2.265 di interfaccia, 43 endpoint HTTP.

---

## 6. Assi di indagine

Ogni asse è un mandato. Le domande elencate sono il punto di partenza obbligatorio, **non il
confine**: se trovi altro sull'asse, è nel mandato.

### A — Superficie esterna e autenticazione
43 endpoint HTTP, un endpoint non autenticato (`/ispindel`), aggiornamento OTA, MQTT, portale
di configurazione in SoftAP.
- Quali endpoint non richiedono autenticazione, e per ciascuno: è una scelta o una svista?
  Cosa può fare un dispositivo sulla stessa LAN?
- Il meccanismo di sessione: come è generato il token, dove è confrontato, è confrontabile a
  tempo costante, scade, sopravvive a un riavvio, può essere indovinato o riusato?
- La password: dove è conservata, in chiaro o no, quanto è lunga di default, esiste una
  protezione contro tentativi ripetuti, compare in registri o risposte?
- OTA: chi può caricare un firmware? Esiste una verifica di integrità o di provenienza?
  Cosa succede se l'upload si interrompe? È l'attacco con il massimo guadagno: trattalo così.
- Ogni input esterno (JSON, parametri di query, corpo POST, payload MQTT, dati iSpindel):
  tipo verificato, intervallo verificato, lunghezza limitata, NaN e infiniti esclusi?
- Iniezione nelle risposte e nella pagina: JSON costruito per concatenazione, testo utente
  inserito in `innerHTML`, intestazioni HTTP.
- MQTT: credenziali, argomenti sottoscritti, cosa accetta dall'esterno, cosa succede se il
  broker è ostile o irraggiungibile.

### B — Sicurezza funzionale degli attuatori
- Esiste un percorso, **uno solo**, che possa lasciare entrambi i relè chiusi insieme, o un
  relè chiuso mentre il software crede il contrario? Cerca: modalità test, riavvio,
  riconfigurazione a caldo, errore del driver I²C, disarmo durante un'azione.
- Il tempo minimo di marcia e il riposo del compressore: esiste un percorso che li aggiri?
  (È già accaduto due volte.)
- Stato all'avvio e dopo un blackout: le uscite partono spente? Il riposo del compressore è
  assunto non trascorso quando l'orologio non è attendibile?
- Il watchdog: quali percorsi possono superare il suo limite? Upload OTA, download del
  registro, scansione WiFi, scansione del bus, scritture su LittleFS.
- Guardie di sicurezza: per ciascuna, **fail-open o fail-closed?** E la scelta è quella giusta
  per la conseguenza? (Archetipo 8.)

### C — Correttezza logica del motore
- La macchina a stati del profilo: per **ogni** tipo di step, esiste una condizione di uscita
  garantita? Enumerali tutti e dimostralo. (Archetipo 14.)
- Pausa, ripresa, salto, arresto, completamento, partenza ritardata: ogni transizione, in ogni
  ordine, incluse quelle assurde. Cosa succede a mettere in pausa una partenza ritardata? A
  saltare l'ultimo step? A riprendere dopo un salto d'orologio?
- Tempo: ogni epoch, ogni durata, ogni traslazione dopo un salto dell'orologio. Ogni
  `millis()`. Ogni confronto fra un tempo assoluto e uno relativo.
- Unità di misura: densità in SG contro °Plato, temperature, percentuali di attenuazione,
  secondi contro ore contro giorni. Ogni conversione, e soprattutto ogni conversione
  **parziale**. (Archetipo 12.)
- Il controllo di temperatura: isteresi, PID, stimatore predittivo. Cosa succede ai valori
  limite, alle sonde non valide, ai setpoint assurdi, ai parametri fuori scala.

### D — Persistenza, ripresa, integrità dei dati
- Ogni scrittura su LittleFS: è atomica? Cosa resta su disco se manca corrente a metà?
- Ogni lettura: cosa succede con un file assente, vuoto, troncato, con JSON valido ma campi
  assurdi, con JSON più grande della capacità del documento?
- La ripresa dopo un blackout: quali stati possono essere ricostruiti in modo incoerente?
- Usura della flash: quante scritture al giorno nel caso peggiore? Esiste un percorso che
  scrive a ogni ciclo?
- I documenti JSON: per ciascuno, calcola la capacità necessaria nel caso peggiore e
  confrontala con quella dichiarata. Verifica `overflowed()` dove serve.

### E — Risorse
- Heap: allocazioni negli handler web, `String` temporanee, frammentazione. Il caso peggiore
  con 19 KB liberi.
- Stack: la catena degli handler è profonda; cerca array e documenti JSON sullo stack.
- Tempi: quanto dura, nel caso peggiore, ogni handler? Quali fermano il controllo e per quanto?
- Flash del programma: 67% occupato. Cosa cresce senza limite?

### F — Rientranza e ordine di esecuzione
- Un handler HTTP può essere invocato mentre una macchina a stati è a metà transizione?
- Due client contemporanei: quali coppie di operazioni si pestano i piedi?
- Diagnostica che perturba il funzionamento normale (scansioni del bus, modalità test).

### G — Verità di ciò che viene mostrato
Questo asse ha pari dignità degli altri: su un dispositivo non sorvegliato, **un numero falso
è un guasto**.
- Ogni numero mostrato in pagina, sull'OLED, in MQTT e nel CSV: da dove viene, può essere
  vecchio, può essere fabbricato, è nell'unità dichiarata?
- Ogni allarme: può accendersi quando non serve? Può **non** accendersi quando serve? Il testo
  descrive la causa reale? (Archetipi 4, 17.)
- Ogni conferma prima di un'azione distruttiva: descrive ciò che accadrà davvero?
- Ogni stato «tutto a posto»: è distinguibile da «non lo so»?

### H — Ciò che i test non coprono
59 test nativi su `lib/axcore/`. Il resto non è testato.
- Quali difetti fra quelli elencati al §4 **sarebbero stati** intercettati da un test? Perché
  non c'era?
- Elenca i dieci test che oggi mancano e che avrebbero il maggiore valore, con la loro forma.
- La logica non testabile a banco perché intrecciata con l'hardware: quale, e come si potrebbe
  separare?

---

## 7. Architettura del lavoro

**Sì, la revisione va eseguita con agenti in parallelo. Sì, serve una seconda squadra
avversariale.** Le ragioni sono empiriche e sono spiegate al §9; qui c'è la struttura.

### Fase 1 — Ricognizione (1 agente)
Legge `platformio.ini`, `src/main.cpp`, `docs/ARCHITETTURA.md` e i file di `lib/axcore/`, e
produce una **mappa**: chi chiama chi, dove vive lo stato, quali sono le invarianti dichiarate
nei commenti. Le invarianti dichiarate sono materiale prezioso: gli archetipi 5 e 6 sono
violazioni di invarianti che il codice stesso enuncia altrove.
Questa mappa viene passata a tutte le fasi successive.

### Fase 2 — Ricerca (un agente per asse, 8 agenti, in parallelo)
Ogni agente riceve: contesto (§0), regole (§2), rubrica (§3), archetipi (§4), il proprio
mandato (§6) e la mappa della fase 1. Ogni agente dichiara i file del ledger che ha coperto.
Tetto: **8 rilievi per agente**, i più gravi. Il tetto serve a costringere alla selezione, non
a limitare la ricerca.

### Fase 3 — Confutazione avversariale (2 agenti per rilievo, in parallelo)
Ogni rilievo passa a due agenti **indipendenti fra loro e dalla fase 2**, con mandati diversi:

- **Confutatore tecnico**: «Il tuo compito è dimostrare che questo rilievo è sbagliato. Leggi
  il codice riga per riga, apri le librerie di terze parti. Nel dubbio, rispondi che non è
  reale.»
- **Confutatore d'impatto**: «Ignora l'eleganza. Chiediti se il proprietario di questo
  dispositivo ne subirebbe una conseguenza concreta. Se è teorico, o se il percorso richiede
  una combinazione che nessuno produrrà mai, non è reale.»

Sopravvive solo ciò che **entrambi** confermano. Ciò che riceve voti discordi finisce in una
lista separata, **da leggere**, non da scartare: nella storia di questo progetto i voti
discordi hanno contenuto difetti veri più di una volta.

### Fase 4 — Critico di completezza (1 agente)
Riceve tutti i referti e il ledger, e risponde a una sola domanda: **cosa non è stato
guardato?** File del ledger non coperti, assi trattati superficialmente, archetipi del §4 mai
cercati, affermazioni date per buone senza verifica. Il suo esito genera un nuovo giro di
fase 2 mirato.

### Fase 5 — Cicli successivi, con strategia diversa a ogni giro

La revisione **non è un giro solo**: le fasi 2-4 si ripetono. Ma va detto con precisione cosa
significa ripetere, perché qui si nasconde l'errore più facile da commettere.

**Un giro che non trova nulla non dimostra che non ci sia più nulla.** Dimostra che *quella
strategia di ricerca* ha dato ciò che poteva. Se ogni giro cerca nello stesso modo, il secondo
giro vuoto misura soltanto la stanchezza del metodo, e il silenzio viene scambiato per
completezza. Per questo **ogni giro deve cambiare il modo di guardare**, non ripetere il
precedente con altri agenti:

| Giro | Come è organizzata la ricerca | Perché trova cose diverse |
|---|---|---|
| 1 | **Per asse** (§6) | Copre i temi noti; è la base. |
| 2 | **Per file** (ledger §5), un agente per gruppo di file, letti *per intero* e senza un tema assegnato | Chi legge un file intero vede ciò che chi insegue un tema non nota, perché non sa in anticipo cosa sta cercando. |
| 3 | **Per archetipo** (§4), un agente per archetipo che lo cerca in tutta la codebase | Trasforma ogni difetto già pagato in una sonda sistematica: «dove altro esiste questa forma?» |
| 4 | **Per percorso d'uso**, end-to-end, ignorando i confini fra i file | Avvia una cotta, cambia profilo a metà, togli corrente, riprendi, cambia le sonde, aggiorna il firmware, arma e disarma da due client. I difetti di composizione non stanno dentro nessun file. |
| 5 | **Per assenza**: cosa *dovrebbe* esserci e non c'è | Validazioni mancanti, allarmi mancanti, casi non gestiti, test mancanti. Cercare codice sbagliato e cercare codice assente sono due ricerche diverse: la seconda non avviene mai per caso. |

La deduplicazione va fatta contro **tutti** i rilievi già visti, compresi quelli respinti,
altrimenti i respinti tornano a ogni giro e il ciclo non converge.

### Criteri di arresto — il primo che si verifica

Non esiste un solo criterio, e **«zero rilievi» non è il criterio principale**: è uno dei tre.

1. **Convergenza.** Due giri consecutivi *con strategie diverse* non producono rilievi nuovi
   confermati, **e** il ledger del §5 risulta coperto al 100%, **e** ogni rilievo critico ha
   una riproduzione o un esperimento dichiarato. Le tre condizioni valgono insieme: due giri
   vuoti con il ledger incompleto non sono convergenza, sono una ricerca che ha guardato altrove.
2. **Rendimento decrescente.** Se un giro produce meno di un rilievo confermato ogni due
   agenti, il giro successivo è l'ultimo. Continuare oltre questa soglia produce soprattutto
   rilievi fabbricati: quando non c'è più segnale, la pressione a trovare qualcosa si scarica
   sulla qualità.
3. **Tetto dichiarato in anticipo.** Numero massimo di giri e di agenti, deciso prima di
   iniziare. «Fino a zero» senza tetto è un costo illimitato, e un ciclo che si ferma per
   esaurimento del budget con un referto onesto vale più di uno che non si ferma mai.

Qualunque sia il criterio che ha fermato il ciclo, **il referto finale deve dirlo**, insieme
a quanti giri sono stati fatti e con quale strategia ciascuno. Un lettore che non sa perché
la ricerca si è fermata non sa quanto fidarsi del fatto che non abbia trovato altro.

### La distinzione che regge tutto

Tenere separate due affermazioni che è comodo confondere:

- **«Il ciclo è finito»** — la ricerca ha smesso di produrre. È un fatto sul processo.
- **«La codebase è a livello di produzione»** — è un *giudizio*, richiesto esplicitamente al
  §8, che va motivato rispetto alla definizione del §1 e che può benissimo essere negativo
  anche dopo un ciclo terminato per convergenza.

Nessun numero di giri vuoti autorizza a scrivere la seconda frase. Se al termine restano
rilievi critici non risolti, aree del ledger coperte solo in parte o verifiche rimandate al
campo, la risposta alla domanda del §1 è **no**, e il referto lo deve dire in chiaro.

### Fase 6 — Sintesi (1 agente)
Produce il referto finale del §8. Non aggiunge rilievi propri: ordina, raggruppa i difetti che
condividono una causa, e propone la **sequenza** di correzione, motivata dal rischio e dalle
dipendenze fra le correzioni.

### Regola sulle correzioni, se e quando verranno fatte
Le correzioni vanno **revisionate come codice nuovo**, con lo stesso apparato. In questo
progetto, due dei difetti più gravi mai trovati erano dentro correzioni di altri difetti: una
guardia che poteva bloccare il raffreddamento di una cotta in corso, e un ritentativo che
nascondeva il guasto che doveva tollerare. **Il codice appena scritto per riparare è il codice
meno affidabile della base.**

---

## 8. Formato del referto

Ogni rilievo, in italiano:

```
TITOLO           una riga, dice il difetto, non l'area
SEVERITÀ         critico | grave | minore
CONFIDENZA       certo | probabile | da verificare sul campo
FILE             percorso:riga (più righe se il difetto è distribuito)
CODICE           le righe rilevanti, testuali
COSA ACCADE      lo scenario di guasto: stato iniziale, sequenza, conseguenza osservabile
PERCHÉ ORA       cosa lo rende raggiungibile: quale configurazione, quale uso, quale tempo
ARCHETIPO        il numero del §4 se ne segue uno, altrimenti "nuovo"
CORREZIONE       la direzione, non la patch. Se ci sono due strade, entrambe con il compromesso
ESPERIMENTO      solo per confidenza "da verificare sul campo": la misura che decide
```

Il referto finale contiene inoltre:
- **Ledger di copertura** compilato: ogni file, chi l'ha letto, per intero o in parte.
- **Rilievi a voti discordi**, con i due pareri per esteso.
- **NON VERIFICATO**: l'elenco onesto di ciò che è rimasto fuori portata e perché.
- **Sequenza di correzione** proposta.
- **Giudizio sulla missione**: la codebase è a livello di produzione secondo la definizione
  del §1? Se no, quali rilievi in particolare lo impediscono?

---

## 9. Perché agenti in parallelo e perché una seconda squadra

Sono decisioni progettuali, non ornamenti. Le ragioni vengono dalla storia di questo progetto.

**Perché più agenti.** 5.855 righe di C++, 2.265 di interfaccia e 43 endpoint eccedono ciò a
cui una sola lettura presta davvero attenzione: la parte finale riceve meno cura della prima.
Nelle revisioni già eseguite su questa codebase, ogni lente ha trovato difetti che le altre
non avevano visto, e nessuna lente ha trovato tutto. La parallelizzazione qui non serve alla
velocità: serve a **impedire che un solo giudizio decida cosa merita attenzione**.

**Perché una seconda squadra avversariale.** Nelle revisioni già eseguite, fra il 60% e il 70%
dei rilievi grezzi è stato demolito dai confutatori: erano percorsi inesistenti, condizioni
già impedite a monte, o preferenze travestite da difetti. Senza quella fase si sarebbero
«corretti» problemi inesistenti — e ogni correzione inutile è codice nuovo, cioè rischio nuovo.
Nella direzione opposta, la fase avversariale ha intercettato **difetti critici dentro le
correzioni**, che l'autore delle correzioni non aveva visto perché stava guardando il proprio
lavoro. Chi scrive una soluzione ha, per costruzione, un punto cieco su di essa.

**Perché due confutatori con mandati diversi.** Un solo confutatore tende a valutare una sola
dimensione. Separare «è tecnicamente vero?» da «ha una conseguenza concreta?» ha prodotto, in
pratica, i disaccordi più informativi: i casi in cui uno conferma e l'altro nega sono quasi
sempre difetti reali ma mal formulati, e vanno letti a mano.

**Perché un critico di completezza.** «Nessun aspetto trascurato» non si ottiene chiedendolo:
si ottiene con un elenco di ciò che andava guardato e un agente il cui unico compito è
confrontarlo con ciò che è stato guardato.

---

## 10. Note operative

- **Le sessioni lunghe possono essere interrotte.** Una revisione precedente su questa
  codebase ha perso 45 agenti su 54 per un limite di sessione, e i risultati parziali erano
  recuperabili solo dal journal. Struttura il lavoro in fasi che producano **risultati
  utilizzabili anche se incompleti**, e salva l'esito di ogni fase prima di iniziare la
  successiva.
- **Tetto ai rilievi per agente**: senza, la fase di confutazione esplode e non arriva in
  fondo. Otto per agente è il compromesso già collaudato.
- **Non riavviare la ricerca da zero a ogni giro**: passa ai nuovi agenti l'elenco di ciò che
  è già stato trovato e respinto, e la strategia dei giri precedenti — devono cercare in un
  modo che non è ancora stato usato (§7, fase 5).
- **Decidi il tetto prima di cominciare**, non quando il budget è finito: il numero di giri e
  di agenti va scritto nel mandato, così il ciclo termina per una scelta e non per un limite.
- **Il dispositivo è raggiungibile** per le verifiche sul campo: console seriale (comandi
  `aiuto`, `stato`, `sonde`, `scan ow`, `scan i2c`, `heap`) e API HTTP autenticata. Il comando
  `sonde` perturba il bus: per misurare l'affidabilità delle letture usare `stato`.
- **Non modificare il codice** durante la revisione: gli agenti leggono i file, e un file che
  cambia sotto di loro produce referti incoerenti.
