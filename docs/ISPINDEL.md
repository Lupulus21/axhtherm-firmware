# ISPINDEL.md — Configurazione e calibrazione dell'iSpindel per AxHTherm

Guida ricavata dai sorgenti ufficiali del firmware iSpindel 7.3.3 (github.com/universam1/iSpindel),
dalla documentazione ispindel.de e da ~20 issue reali. Verificata contro il codice di AxHTherm
(`src/svc/WebApi.cpp` handleIspindel) il 20/08/2026.

Endpoint del termostato: **POST http://192.168.1.50/ispindel** — risposta misurata: 200 in 0,06 s.
Unita': il termostato e' configurato in **SG** (verificato: un valore 18.5 in Plato viene rifiutato
con 400 'valori fuori range plausibile').

## Sintesi

GUIDA OPERATIVA — Collegare l'iSpindel al termostato AxHTherm (POST http://192.168.1.50/ispindel, formato Generic HTTP JSON).

L'iSpindel non misura la densità: misura un ANGOLO di galleggiamento e lo converte con un POLINOMIO che devi ricavare tu e scrivere nel portale. Da questo dipendono gli step "attesa densità" di AxHTherm, quindi la configurazione va fatta in questo ordine: assetto meccanico → calibrazione → endpoint → verifica.

TRE COSE CHE SBAGLIANO QUASI TUTTI, da leggere prima di toccare qualsiasi cosa:
1) UNITÀ DI MISURA. Il polinomio di fabbrica (-0.00031*tilt^2+0.557*tilt-14.054) restituisce gradi PLATO, ma AxHTherm ha gravityUnit="sg" e accetta solo 0.90–1.30. Il tuo polinomio deve uscire in SG. Trappola silenziosa: a fine fermentazione un valore in Plato di 1.0–1.3 cade DENTRO il range SG, passa il controllo senza errori e può far avanzare uno step su un numero privo di senso.
2) POLINOMIO VUOTO O CON ERRORE DI SINTASSI → il firmware invia gravity = 0 senza alcun segnale d'errore in rete. Uno step "densità sotto soglia" scatterebbe subito. L'errore compare SOLO sul monitor seriale ("Parse error at N").
3) SERVER ADDRESS SENZA "http://". Va scritto 192.168.1.50 e basta: il prefisso serve solo alla voce "HTTPS Post", che qui non si usa.

Da sapere sul comportamento reale del dispositivo: non ritenta MAI un invio fallito e non bufferizza nulla (una POST persa = un campione perso), il termostato deve rispondere entro 5 secondi, e in esercizio normale l'iSpindel è irraggiungibile in rete — trasmette e basta, il ritmo lo detta solo l'Update Interval.

Tempo richiesto: ~30 minuti per la configurazione, mezza giornata per una calibrazione fatta bene (7 punti di diluizione).

## Procedura


## 1. COSA SERVE PRIMA DI INIZIARE

- MATERIALE: iSpindel assemblato con batteria 18650 carica a 4.2 V (peso della cella >= 42 g: le celle 'da 9800 mAh' sono false e leggere e falsano l'assetto); un densimetro tarato a 20 gradi C (o rifrattometro, valido solo su soluzioni senza alcol); 100 g di zucchero e ~4 L d'acqua; un cilindro alto e stretto per le misure; un telefono o PC per il portale WiFi; opzionale ma consigliato un cavo micro-USB e un monitor seriale a 115200 baud (unico modo per vedere gli errori).
- RETE: verifica di avere un SSID a 2.4 GHz. L'ESP8266 non vede i 5 GHz e non supporta WPA3-only. Se il router usa un SSID unico 2.4+5 GHz, separa temporaneamente le bande o usa l'SSID a 2.4 GHz. L'iSpindel deve finire sulla STESSA rete/VLAN del termostato 192.168.1.50.
- IP FISSO: il portale dell'iSpindel NON ha i campi per l'IP statico (non esistono, non cercarli). Funziona solo in DHCP: se ti serve un indirizzo stabile fai una prenotazione DHCP sul router legata al MAC. Il campo 'iSpindel Name' diventa anche l'hostname DHCP, quindi lo ritrovi nella lista client.
- DATI DA AVERE SOTTO MANO PRIMA DI ENTRARE NEL PORTALE (la finestra dura solo 5 minuti): IP 192.168.1.50, porta 80, path /ispindel, SSID e password WiFi, il polinomio gia' calcolato, il token AxHTherm se lo usi.
- VERIFICA VERSIONE FIRMWARE: entra nel portale, premi 'Maintenance' e leggi 'Version:'. L'ultima e' la 7.3.3 (8 ago 2023). Sotto la 7.x aggiorna con firmware.bin dal form 'Firmware Update' della stessa pagina; se vieni da una versione < 6.x fai anche 'Factory Reset' dopo l'aggiornamento, perche' il filesystem e' cambiato e la configurazione resta incastrata.
- TEST DEL RISVEGLIO (falla ora, e' il guasto piu' frequente): collega l'USB, monitor seriale a 115200, metti Update Interval a 30 s e guarda se il dispositivo si risveglia da solo piu' volte. Se logga una volta sola e poi tace, il problema e' hardware: diodo Schottky BAT43 tra D0 e RST (catodo/anello verso D0) o resistenza da 330 ohm, oppure — causa numero uno nelle segnalazioni reali — un Wemos D1 mini contraffatto (gli originali riportano il marchio LOLIN; i falsi noti stampano 'D1 mimi'/'D1 m1n1').

## 2. ENTRARE IN CONFIGURAZIONE

- Accendi l'iSpindel e premi il tasto Reset 3-4 volte in rapida successione, a meno di 1 secondo l'una dall'altra (la finestra del doppio reset e' esattamente 1 s: pause piu' lunghe non vengono riconosciute). In alternativa, sul firmware attuale basta anche un singolo reset o un semplice spegni/riaccendi con l'interruttore.
- CONFERMA VISIVA: guarda il LED attraverso il tubo. In config mode lampeggia a intervalli di 1 secondo. Se non lampeggia con quella cadenza NON sei in configurazione: ripeti il reset.
- Dal telefono o dal PC cerca la rete WiFi 'iSpindel_XXXXXX' (6 cifre esadecimali del chip ID) oppure 'iSpindel_<Nome>' se il nome e' gia' stato impostato. La rete e' APERTA, senza password.
- Il captive portal dovrebbe aprirsi da solo; se non succede apri il browser su http://192.168.4.1 scrivendo esplicitamente 'http://' (altrimenti il browser fa una ricerca o forza HTTPS). Una finestra in incognito evita cache e redirect HSTS.
- SUL TELEFONO: disattiva i dati mobili prima di collegarti e disattiva 'Passa automaticamente a reti migliori' / 'Wi-Fi intelligente' / Private DNS, altrimenti Android abbandona l'AP perche' non ha Internet e il portale si chiude.
- ATTENZIONE ALLA FINESTRA: il portale resta attivo 300 secondi (5 minuti) esatti, poi l'iSpindel va in deep sleep e devi rifare il reset. Per questo i valori vanno preparati prima.
- SE L'AP NON COMPARE: (a) ripeti i reset piu' rapidamente; (b) prova il power-cycle completo con l'interruttore; (c) controlla la batteria, sotto 3.3 V il firmware va in modalita' di emergenza — ricarica e riprova; (d) aggiorna la lista reti e prova con un secondo dispositivo; (e) ultima risorsa: apri il tubo, collega via USB e leggi il log seriale a 115200 baud.
- NELLA HOME DEL PORTALE trovi: 'iSpindel Info', 'Configuration', 'Maintenance', 'Information', 'Exit Portal'. Per i parametri clicca 'Configuration'. Se la tendina dei servizi appare VUOTA, il browser non sta eseguendo il JavaScript: usa un browser desktop moderno.

## 3. CAMPI DA COMPILARE — VALORI PRECISI PER AxHTherm

- Compila la pagina 'Configuration' esattamente cosi' (le etichette sono quelle vere del portale):

| Campo nel portale | Valore da inserire |
|---|---|
| SSID / Password WiFi | la tua rete a 2.4 GHz (seleziona dalla lista o digita) |
| iSpindel Name | Fermentatore1 (breve, SENZA spazi ne' accenti) |
| Update Interval (s) | 900 |
| Service Type (tendina 'API') | HTTP — QUARTA voce dell'elenco |
| Token/ API key | VUOTO (oppure esattamente la stringa ispindelToken di AxHTherm) |
| Server Address | 192.168.1.50 |
| Server Port | 80 |
| Path / URI | /ispindel |
| Connect to server via HTTPS | NON selezionata |
| Unit of temperature | Celsius |
| Polynominal (Gravity conversion) | il TUO polinomio di 3 grado, in SG (vedi sezione 4) |
| Battery conversion factor | 191.8 (default, salvo taratura del partitore) |
- SERVICE TYPE: scegli la voce che si chiama esattamente 'HTTP'. L'ordine della tendina e': Ubidots, empty, CraftBeerPi, HTTP, TControl, FHEM, TCP, iSpindel.de, InfluxDB, Prometheus, MQTT, ThingSpeak, Blynk, Brewblox, AWSIOTMQTT, HTTPS Post, BierBot Bricks. NON scegliere 'HTTPS Post' (richiede TLS, che AxHTherm non espone) ne' 'CraftBeerPi' (endpoint e formato diversi).
- CONTROLLO IMMEDIATO: appena selezioni 'HTTP' devono comparire esattamente QUATTRO campi aggiuntivi — Token, Server Address, Path/URI, Server Port — e devono sparire Username, Password, Channelnumber, Prometheus job/instance. Se non vedi questi quattro campi hai selezionato la voce sbagliata.
- SERVER ADDRESS: scrivi 192.168.1.50 e nient'altro. SENZA 'http://', senza porta, senza path, senza slash finale. Il firmware passa questo valore come host puro; il prefisso 'https://' serve solo alla variante HTTPS Post.
- PATH / URI: scrivi /ispindel con lo slash iniziale. Senza slash la richiesta HTTP risulta malformata (riceverai un 404 o nulla).
- TOKEN: lascialo VUOTO se in AxHTherm il campo ispindelToken e' vuoto. Se lo valorizzi, il firmware lo inserisce come campo 'token' DENTRO il corpo JSON, non come header Authorization e non come query string. Se e' vuoto la chiave 'token' e' proprio ASSENTE dal JSON (non stringa vuota).
- UPDATE INTERVAL: 900 s (15 min) e' il default ed e' l'intervallo giusto per l'esercizio. Non scendere sotto i 15 minuti in servizio: intervalli brevi falsano la temperatura per autoriscaldamento dell'ESP8266 dentro il tubo sigillato. Se vuoi 1800 s per l'autonomia, devi anche alzare gravityStaleMin su AxHTherm (vedi passo dedicato).
- SALVA con il pulsante 'save' in fondo al form. ATTENZIONE: subito dopo il salvataggio l'iSpindel chiude il portale e va IMMEDIATAMENTE in deep sleep — la prima POST arrivera' dopo un intero intervallo, non subito. Non e' un guasto.

## 4. CALIBRAZIONE (obbligatoria: senza, il dato non ha significato)

- FASE A — ASSETTO MECCANICO. Monta la 18650 (>= 42 g), chiudi il PETling, metti l'iSpindel in acqua di rubinetto e leggi l'angolo nella pagina 'iSpindel Info' del portale. Aggiungi zavorra sul FONDO (dadi, pallini di piombo, una moneta incollata e centrata, il piu' lontano possibile dall'imboccatura) finche' l'angolo in acqua e' ~25 gradi dalla verticale. Servono tipicamente ~20 g. Tolleranza 18–27 gradi. Sotto 15 gradi non leggi piu' densita' basse, sopra 30 gradi la OG risulta imprecisa. Peso in alto = piu' inclinato, peso in basso = piu' verticale.
- FASE B — INCOLLA TUTTO. Scheda, batteria e zavorra vanno bloccati definitivamente: se qualcosa si sposta l'angolo cambia e la calibrazione decade. Vale anche per un semplice smontaggio e rimontaggio.
- FASE C — AZZERAMENTO ACCELEROMETRO (prima dei punti di densita'). Entra nel portale, appoggia il dispositivo con la linea centrale ORIZZONTALE (puntella il fondo arrotondato), poi Maintenance > Offset Calibration > 'calibrate'. I 6 offset (X/Y/Z accelerometro + giroscopio) finiscono in config.json; se riflashi azzerando il filesystem li perdi e vanno rifatti.
- FASE D — INTERVALLO CORTO. Metti temporaneamente Update Interval a 20–60 s per vedere l'angolo aggiornarsi mentre lavori. Alla fine rimettilo a 900 s: non dimenticarlo.
- FASE E — PUNTO ZERO. Immergi in acqua a ~20 gradi C, in un cilindro alto e stretto, dai un colpetto per staccare le bolle, aspetta che l'angolo si stabilizzi e annota: angolo <-> 1.000 SG.
- FASE F — SOLUZIONE MADRE. 400 ml d'acqua + 100 g di zucchero = ~1.085 (20 gradi Plato), cioe' pari o superiore alla OG massima che brassi. Sciogli completamente e riporta a ~20 gradi C prima di misurare.
- FASE G — SERIE DI DILUIZIONI, 7 punti: 1.000, 1.010, 1.020, 1.030, 1.040, 1.061, 1.085. Diluizioni pratiche partendo da 500 ml a 20 gradi P: +166 ml -> 15 P; +333 ml -> 10 P; +333 ml -> 7.5 P; +666 ml -> 5 P; +2000 ml -> 2.5 P. A ogni gradino annota DUE cose: densita' col densimetro e angolo dell'iSpindel. Minimo assoluto 5 punti, meglio 6–7 (il 3 grado richiede almeno 4 punti e con 4 esatti interpola senza margine di controllo).
- FASE G-bis — CORREZIONE DI TEMPERATURA: il densimetro e' tarato a 20 gradi C. O porti tutto a 20 gradi C o applichi la correzione: una lettura 1.050 fatta a 30 gradi C vale in realta' 1.0526 (+2.6 punti), piu' di tutta la tolleranza che ti puoi permettere. Il tool ufficiale ha la spunta 'Temperature Corrected' con 'Wort Temperature' e 'CAL@' che fanno il calcolo da sole.
- FASE H — CALCOLO DEL POLINOMIO. Apri https://www.ispindel.de/tools/calibration/calibration.htm, imposta 'Gravity Unit: Specific Gravity' e 'Measurement: Hydrometer', inserisci per ogni riga densita' + temperatura + Tilt e premi 'Add Data'. Il tool calcola le regressioni di grado 1, 2 e 3 e mostra 'Degree N Value' e 'Degree N Error' per ogni punto.
- FASE I — PRENDI IL GRADO 3 E VERIFICA I RESIDUI. Criterio di accettazione: ogni valore della colonna 'Degree 3 Error' deve stare entro ~0.002 SG (2 punti) e nessun punto deve essere un outlier isolato. Sul dataset ufficiale il grado 1 lascia 4.1 punti di errore, il grado 2 ne lascia 3.2, il grado 3 solo 1.3: il grado 2 non basta per pilotare uno step 'attesa densita''. Se UN punto ha residuo molto peggiore degli altri, quella misura e' sbagliata (bolla, zucchero non sciolto, temperatura diversa): rifalla, NON alzare il grado.
- FASE L — SCRIVI IL POLINOMIO NEL PORTALE, campo 'Polynominal' (etichetta sopra: 'Gravity conversion'). Le uniche variabili ammesse sono 'tilt' e 'temp' ('temp' e' SEMPRE in gradi Celsius, anche se invii in Fahrenheit). Sintassi: punto decimale, '*' esplicito per le moltiplicazioni, '^' per le potenze, notazione scientifica ammessa (1.0E-6). Il campo del form accetta max 250 caratteri: se il tool ti da' la forma lunga 'c*tilt*tilt*tilt', riscrivila come 'c*tilt^3'. Esempio ufficiale di forma corretta IN SG: (1.0000E-6*(tilt^3))-(1.3137E-4*(tilt^2))+(6.9680E-3*tilt)+(8.9493E-1).
- FASE M — CONTROLLO UNITA' (il punto che rompe piu' spesso l'integrazione): il polinomio DEVE uscire in SG, perche' AxHTherm ha gravityUnit='sg' e scarta con HTTP 400 tutto cio' che non sta fra 0.90 e 1.30. Sanity check: valuta il tuo polinomio a tilt=25 gradi, deve dare ~1.00. Se preferisci lavorare in Plato, devi metterlo esplicitamente (gravityUnit='plato', range accettato -1..45) — ma non lasciare le due cose disallineate.
- FASE N — SUL SERIALE, prima di chiudere il portale, verifica che la riga 'Gravity:' mostri un valore plausibile. Un gravity costantemente a 0 significa polinomio scritto male (tipicamente asterischi mancanti: '0.557tilt' invece di '0.557*tilt' -> sul log appare 'Parse error at 1'), non un problema di rete.
- NOTA SULLA CO2: nelle prime 24–48 ore di fermentazione la densita' letta puo' SALIRE (fino a +15 punti in 4 ore in casi documentati). Non e' un guasto: sono le bolle di CO2 attaccate al tubo che aumentano il galleggiamento. Se questo ti manda fuori tempo gli step a soglia, la calibrazione col 'metodo II' (durante una fermentazione veloce reale, descritta in docs/Calibration_en.md) incorpora questo errore nel polinomio ed e' quella raccomandata dall'autore.
- RICALIBRA OGNI VOLTA CHE: apri il tubo e sposti scheda o batteria, cambi batteria con una di peso diverso, modifichi la zavorra, o riflashi azzerando il filesystem. NON serve per una semplice ricarica o sanificazione.

## 5. VERIFICA CHE I DATI ARRIVINO

- PRIMA PROVA, SENZA iSPINDEL — collauda l'endpoint da solo e cronometra la risposta:
curl -v -X POST http://192.168.1.50/ispindel -H 'Content-Type: application/json' -H 'User-Agent: iSpindel' -H 'Connection: close' -d '{"name":"Fermentatore1","ID":16578179,"angle":64.93,"temperature":20.5,"temp_units":"C","battery":4.07,"gravity":1.048,"interval":900,"RSSI":-62}'
Deve rispondere {"result":"ok"} in MENO DI 5 SECONDI: oltre quel tempo la POST dell'iSpindel fallisce e il campione e' perso per sempre. Se AxHTherm scrive su LittleFS o aggiorna lo stato, deve rispondere 200 PRIMA di farlo.
- SECONDA PROVA, CON L'iSPINDEL — rientra in config mode, metti Update Interval a 60 s, salva, e controlla i log di AxHTherm: devi vedere arrivare POST /ispindel con Content-Type application/json e User-Agent 'iSpindel'. Sul monitor seriale devi leggere 'calling HTTP' seguito da 'code: 200'. Un valore negativo = errore di trasporto (server irraggiungibile o timeout); un 404 = path sbagliato.
- CONTROLLA I VALORI, non solo l'arrivo: 'gravity' deve essere plausibile (non 0, non negativo, non centinaia); 'angle' deve corrispondere all'angolo atteso. Regola diagnostica: se angle e' giusto ma gravity no, l'errore e' nel polinomio; se angle e' sbagliato, l'errore e' meccanico (zavorra, offset accelerometro, bolle).
- VERIFICA DI CALIBRAZIONE END-TO-END (prima di lanciare un profilo vero): (a) iSpindel in acqua a 20 gradi C -> AxHTherm deve mostrare 1.000 +/- 0.002; (b) in una soluzione zuccherina nota (es. 1.040) lo scarto dal densimetro deve essere <= 0.002; (c) durante questa verifica gravityCorrection deve essere 0, altrimenti stai leggendo un offset e non la calibrazione.
- PROVA DI CAMPO WIFI — metti l'iSpindel nella posizione reale, chiudi il coperchio del fermentatore E la porta del frigo, poi leggi l'RSSI. Il portale converte RSSI in qualita' con 2*(RSSI+100): -50 dBm = 100%, -75 dBm = 50%. Sotto i -80 dBm il collegamento e' a rischio concreto, perche' la finestra per agganciare il WiFi al risveglio e' di soli 10 secondi. Rimedi in ordine di efficacia: ripetitore/AP accanto al frigo; isolare elettricamente il coperchio dalla pentola con un cordolo di silicone; antenna passiva lambda/4 in cavo schermato spellato 32 mm per lato, un capo dentro e uno fuori.
- ALLINEA I PARAMETRI SU AxHTherm: gravityStaleMin (default 60 min) deve essere MAGGIORE dell'Update Interval, regola pratica >= 3x. Con 900 s vanno bene 45–60 min; se porti l'iSpindel a 1800 s DEVI alzare staleMin ad almeno 90 min, altrimenti due sole trasmissioni perse fanno scattare l'allarme 'densita' stantia' e il profilo si ferma.
- ALZA batteryAlertV da 3.3 a 3.5 V. A 3.3 V esatti l'iSpindel e' gia' in safe mode (potenza TX azzerata + sleep forzato a 1 ora): l'allarme a 3.3 arriverebbe quando i dati hanno gia' smesso di arrivare. A 3.5 V hai il preavviso utile per ricaricare senza interrompere il profilo.
- CONFIGURA L'ANTI-FALSI-POSITIVI: poiche' una singola lettura sbagliata fa avanzare il profilo, pretendi N letture consecutive sotto soglia prima di far scattare lo step (es. 3, cioe' 45 minuti a 900 s), scarta i gravity == 0 e i valori fuori dall'intervallo di plausibilita'. Usa il campo 'interval' ricevuto nel JSON per calcolare la staleness in modo adattivo, e logga l'RSSI: e' l'indicatore precoce di perdita del collegamento.
- RIMETTI L'INTERVALLO A 900 s e ricarica la batteria a 4.2 V pieni prima di iniziare la cotta. Non partire da 3.8 V pensando di avere autonomia.
- MONITORAGGIO NELLE PRIME 24 h: densita' aggiornata ogni ~15 min con eta' del dato sempre sotto i 30 min, RSSI stabile, tensione in calo lento e lineare (~0.01 V/h e' gia' troppo e indica un guasto). Se compare 'densita' stantia' a fermentatore chiuso, le cause in ordine di probabilita' sono: WiFi debole, batteria sotto 3.3 V, endpoint che risponde lento.
- DALLA 3.16.0 — nel registro (grafico e CSV) la densita' si scrive solo finche' e' fresca: un
  densimetro muto non lascia piu' una riga piatta con l'ultimo valore. Ogni campione porta la
  sua unita' (SG o °P), quindi cambiare unita' non stravolge lo storico. La scheda calcola anche
  l'andamento (medie orarie, pendenza su 24-48 ore): «fermentazione attiva / rallenta / ferma»
  in dashboard, sull'OLED e in Home Assistant, con gli avvisi «fermentazione finita» e
  «fermentazione ferma?» (vedi docs/PROFILI.md).

## 6. ERRORI TIPICI E COME EVITARLI

- ERRORE 1 — Polinomio in Plato con AxHTherm in SG. Sintomo: HTTP 400 'valori fuori range plausibile' per tutta la fermentazione, poi — peggio — a fine fermentazione i valori 1.0–1.3 P entrano nel range SG e vengono accettati in silenzio come densita' valide. Rimedio: costruisci il polinomio in SG e verifica che a tilt=25 dia ~1.00.
- ERRORE 2 — Polinomio vuoto o con asterischi mancanti. Sintomo: gravity = 0 a ogni ciclo, nessun errore in rete, e uno step 'sotto soglia X' che scatta immediatamente. Rimedio: sul seriale cerca 'Parse error at N'; scrivi sempre 0.557*tilt, mai 0.557tilt.
- ERRORE 3 — 'http://' nel campo Server Address. Sintomo: nessuna POST arriva mai. Rimedio: solo 192.168.1.50. Il prefisso serve unicamente alla voce HTTPS Post.
- ERRORE 4 — Path senza slash iniziale. Scrivi /ispindel, non ispindel.
- ERRORE 5 — Aspettarsi la prima POST subito dopo il salvataggio. Non arriva: l'iSpindel va in deep sleep appena chiudi il portale e trasmette dopo un intero intervallo. Per collaudare, usa l'intervallo temporaneo a 60 s.
- ERRORE 6 — Termostato lento a rispondere. Oltre 5 secondi la POST fallisce, e il firmware NON ritenta, non bufferizza e non segnala nulla all'utente: il campione e' perso fino al ciclo successivo. Rispondi 200 con corpo cortissimo prima di qualsiasi scrittura pesante.
- ERRORE 7 — WiFi a 5 GHz o WPA3-only. L'ESP8266 non li vede. Usa 2.4 GHz WPA2.
- ERRORE 8 — Cercare l'IP statico nel portale. Non esiste: usa la prenotazione DHCP sul router.
- ERRORE 9 — iSpindel legato con un filo teso o appoggiato alla parete. Qualsiasi tensione sul filo altera l'inclinazione e quindi la densita' letta; se resta intrappolato nel krausen la lettura si congela su un valore falso. Lascialo libero di galleggiare, lontano da pareti e gorgogliatore; se usi un filo di recupero tienilo LENTO.
- ERRORE 10 — Aspettare 3.3 V per ricaricare. A quella soglia il firmware ha gia' azzerato la potenza TX e allungato lo sleep a >= 1 ora: smette di trasmettere prima ancora di scaricarsi. Ricarica a 3.5 V.
- ERRORE 11 — Sanificare con il tappo avvitato. La guarnizione del PETling e' interna, quindi lievito e batteri finiscono nella filettatura e li' non si lavano piu'. Lava e sanifica tappo e tubo SEPARATI: risciacquo subito dopo l'estrazione, asciugatura del tubo con carta, svitatura del tappo tenendolo con la carta per evitare gocce all'interno, ammollo 24 h, poi sanitizzante no-rinse. Prima di immergerlo, tappo avvitato a fondo e asciutto all'esterno: una goccia di mosto nel filetto resta li' tutta la fermentazione.
- ERRORE 12 — Ignorare la correzione di temperatura del densimetro durante la calibrazione. E' il singolo errore piu' comune e piu' grosso: una lettura a 30 gradi C sballa di 2.6 punti, piu' dell'intera tolleranza ammessa.
- SICUREZZA (non opzionale): usa un TP4056 a 6 pin con protezione DW01, rimuovi il diodo di ingresso del Wemos come da schema ufficiale, e tieni i reofori dei componenti CORTISSIMI o salda dal lato opposto — reofori lunghi possono perforare l'involucro della cella. E' documentato un iSpindel che ha preso fuoco in frigo perche' lasciato acceso a galleggiare dopo la fermentazione. A fine cotta: estrai, risciacqua, lava, ricarica e conserva la cella a 3.7–3.8 V con l'interruttore su OFF.

## Note tecniche verificate (estratto)

- Payload Generic HTTP JSON, campi nell'ordine esatto in cui il firmware li costruisce: name, ID (= ESP.getChipId(), intero decimale), token (SOLO se non vuoto), angle (gradi, float), temperature (gia' convertita nell'unita' scelta), temp_units ('C'/'F'/'K'), battery (Volt), gravity (risultato del polinomio), interval (secondi), RSSI (dBm, negativo). Fonte: pio/src/iSpindel.cpp:670-691.
- Il campo dell'angolo si chiama 'angle', non 'tilt' (in altri servizi come Ubidots/MQTT si chiama invece 'tilt'). Fonte: pio/src/iSpindel.cpp.
- Il campo ID (chip ID hardware) e' univoco e immutabile: e' la chiave piu' affidabile per identificare il dispositivo in AxHTherm, piu' del nome. Fonte: pio/src/iSpindel.cpp.
- Header HTTP inviati dalla POST: User-Agent: iSpindel, Connection: close, Content-Type: application/json. Il buffer JSON e' uno StaticJsonDocument<512>, quindi il payload sta sempre sotto i 512 byte. Fonti: pio/lib/Sender/Sender.cpp:433-436, pio/lib/Sender/Sender.h:55.
- NESSUNA RITENTA E NESSUN BUFFER: sendGenericPost() esegue una sola POST e ritorna 'return true;' incondizionatamente, fuori dai rami di controllo del codice HTTP. Il firmware non sa nemmeno di aver fallito, non ripete l'invio e non accumula il campione. Fonte: pio/lib/Sender/Sender.cpp:426-458.
- Timeout della POST = 5000 ms: il firmware non chiama mai http.setTimeout(), quindi vale HTTPCLIENT_DEFAULT_TCP_TIMEOUT del core ESP8266. Fonte: libraries/ESP8266HTTPClient/src/ESP8266HTTPClient.h:45 (github.com/esp8266/Arduino).
- Attesa massima per l'aggancio WiFi al risveglio: while (WiFi.status() == WL_DISCONNECTED) { delay(200); if (++wait > 50) break; } = 10 secondi; poi connectBackupCredentials() (altri 10 s max) e deep sleep SENZA inviare nulla in quel ciclo, anche se la connessione e' stata nel frattempo ripristinata. Fonte: pio/src/iSpindel.cpp:1347-1378, confermato nella issue #507.
- Server Address per la voce HTTP va inserito SENZA schema: sendGenericPost chiama http.begin(_client, server, port, uri) passando host, porta e path come parametri separati. La FAQ ufficiale conferma: 'Unlike HTTP, the HTTPS POST method requires using the full URL to the server for the Server Address setting'. Fonti: pio/lib/Sender/Sender.cpp, docs/FAQ-en.md.
- Voce esatta del menu servizio: 'HTTP', indice interno DTHTTP = 3, quarta voce della tendina. Ordine completo: [0] Ubidots, [1] empty, [2] CraftBeerPi, [3] HTTP, [4] TControl, [5] FHEM, [6] TCP, [7] iSpindel.de, [8] InfluxDB, [9] Prometheus, [10] MQTT, [11] ThingSpeak, [12] Blynk, [13] Brewblox, [14] AWSIOTMQTT, [15] HTTPS Post, [16] BierBot Bricks. Fonti: pio/lib/WiFiManagerKT/WiFiManagerKT.h (array lAPI), pio/lib/Globals/Globals.h.
- Per la voce HTTP l'array lAPI vale {token:1, server:1, uri:1, port:1} e tutti gli altri 0: il portale mostra esattamente Token, Server Address, Path/URI, Server Port e nasconde Username, Password, Channelnumber, job, instance, hassio, usehttps. E' il controllo visivo che conferma di aver selezionato la voce giusta. Fonte: pio/lib/WiFiManagerKT/WiFiManagerKT.h.
- Il token e' un campo del corpo JSON, non un header: 'if (myData.token[0] != 0) sender.add("token", myData.token);'. Se vuoto, la chiave 'token' e' proprio ASSENTE dal JSON (non stringa vuota): l'handler AxHTherm deve trattarla come opzionale. Fonte: pio/src/iSpindel.cpp.
- Etichette esatte dei campi del portale (dichiarazioni WiFiManagerParameter): 'iSpindel Name', 'Update Interval (s)', 'Token/ API key', 'Server Address', 'Server Port', 'Path / URI', 'Battery conversion factor', 'Polynominal' (con etichetta sopra 'Gravity conversion<br/>ex. "-0.00031*tilt^2+0.557*tilt-14.054"'), 'Connect to server via HTTPS'. Limiti: TKIDSIZE=40 (token fino a 80 caratteri), DNSSIZE=253 (Server Address e Path/URI). Fonti: pio/src/iSpindel.cpp:325-343 e 381-386, pio/lib/Globals/Globals.h.
- Ingresso in config mode. FAQ ufficiale: 'Switch it on, press the reset button 3-4 times which sets up an access point'. Il codice e' piu' permissivo: shouldStartConfig() ritorna true anche per REASON_DEFAULT_RST o REASON_EXT_SYS_RST, quindi anche un singolo reset o un power-cycle entra in configurazione. Fonti: docs/FAQ-en.md, pio/src/iSpindel.cpp:222-231.
- La finestra per il doppio reset e' di 1 secondo esatto: '#define DRD_TIMEOUT 1'. Per questo i reset vanno premuti rapidamente. Fonte: pio/lib/Globals/Globals.h:107.
- Access point: SSID 'iSpindel_%06X' (chip ID) o 'iSpindel_<nome>' se il nome e' impostato; APERTO (startConfigPortal(ssid) chiamato senza password); IP 192.168.4.1; timeout PORTALTIMEOUT = 300 secondi; LED lampeggiante a 1 s (flasher.attach(1, flash)) come segnalazione visiva del config mode. Fonti: pio/src/iSpindel.cpp:394-401, pio/lib/Globals/Globals.h:65, docs/README_en.md:135-138.
- Pagine del portale: '/' home con i pulsanti iSpindel Info, Configuration (/wifi), Maintenance (/mnt), Information (/i), Exit Portal (/close). La pagina Maintenance contiene in ordine: Offset Calibration, Firmware Update (POST multipart su /update, poi reboot), Factory Reset (/reset), Calibrate Battery Voltage. Fonti: pio/lib/WiFiManagerKT/WiFiManagerKT.h e .cpp:1015-1050.
- La tendina dei servizi e' popolata dinamicamente via JavaScript dall'array lAPI: se il JavaScript non viene eseguito la tendina appare VUOTA e il servizio non e' selezionabile. Fonte: pio/lib/WiFiManagerKT/WiFiManagerKT.h.
- Dopo il salvataggio l'iSpindel NON trasmette subito: wifiManager e' configurato con setBreakAfterConfig(true) e al termine di startConfiguration() il firmware chiama goodNight(myData.sleeptime), entrando immediatamente in deep sleep. Il primo POST arriva dopo un intero intervallo. Fonte: pio/src/iSpindel.cpp.
- In esercizio normale l'iSpindel NON e' raggiungibile in rete: 'Man kann die Spindel im Normalbetrieb nicht erreichen, sie sendet ausschliesslich'. Non esiste modo di forzare una lettura da AxHTherm: il ritmo lo detta solo l'Update Interval. Fonte: docs/FAQ.md.
- RISCHIO CRITICO — calculateGravity() inizializza 'float _gravity = 0;', compila il polinomio con te_compile(myData.polynominal, vars, 2, &err) e, se la compilazione fallisce, stampa solo 'Parse error at <n>' sulla seriale e RITORNA 0. Con polinomio vuoto o malformato la iSpindel invia gravity = 0 a ogni ciclo senza alcun segnale d'errore via rete. Errore classico documentato nella issue #468: asterischi mancanti (0.557tilt invece di 0.557*tilt). Fonti: pio/src/iSpindel.cpp:1182-1191, issue #468.
- Le uniche variabili ammesse nel polinomio sono 'tilt' e 'temp': te_variable vars[] = {{"tilt", &_tilt}, {"temp", &_temp}}. 'temp' e' SEMPRE in gradi Celsius, anche se si sceglie Fahrenheit o Kelvin come unita' di invio, perche' la conversione di scala e' applicata dopo, solo al campo 'temperature' del JSON. Fonte: pio/src/iSpindel.cpp:1173-1191, 680, 1035.
- Il motore di calcolo e' tinyexpr: '^' e' mappato su pow(), i numeri sono letti con strtod (notazione scientifica 1.0E-6 valida), funzioni disponibili abs, acos, asin, atan, atan2, ceil, cos, cosh, e, exp, fac, floor, ln, log, log10, ncr, npr, pi, pow, sin, sinh, sqrt, tan, tanh. Fonte: pio/lib/tinyexpr/tinyexpr.c:162-192, 251, 292.
- Il campo 'Polynominal' del form accetta al massimo 250 caratteri (lo storage interno e' char polynominal[1000]): se il tool restituisce la forma espansa 'c*tilt*tilt*tilt' va riscritta come 'c*tilt^3'. Brewfather conferma sul campo che formule troppo lunghe possono fallire. Fonti: pio/src/iSpindel.cpp:381-386, pio/lib/Globals/Globals.h:145, docs.brewfather.app/integrations/ispindel.
- L'UNITA' DELLA DENSITA' NON E' TRASMESSA: nel JSON esiste temp_units per la temperatura ma nessun campo equivalente per la densita'. Il polinomio di fabbrica '-0.00031*tilt^2+0.557*tilt-14.054' restituisce gradi PLATO (a tilt=60 da' 18.25; a tilt=25 da' -0.32), NON SG. E' il motivo per cui un iSpindel appena flashato manda numeri che AxHTherm rifiuta. Fonti: pio/lib/Globals/Globals.h:145, pio/src/iSpindel.cpp.
- Lato AxHTherm, handleIspindel() (src/svc/WebApi.cpp): nome della scheda nell'intestazione Host verificato (IP o nome, dalla 3.7.0); senza codice dei densimetri serve 'Content-Type: application/json' (HTTP 415 altrimenti, dalla 3.9.0); token verificato solo se cfg->ispindelToken non e' vuoto, con una pausa crescente dopo 5 errori dallo stesso indirizzo (HTTP 429); 'gravity' assente -> HTTP 400 'campo gravity mancante'; temperatura convertita in gradi C secondo 'temp_units' (C, F o K; assente = C, dalla 3.9.0); clamp di plausibilita' dipendente da cfg->gravityUnit — se 'plato' range -1..45, altrimenti (default 'sg') range 0.90..1.30 — piu' temperatura obbligatoriamente fra -10 e 50 gradi C; fuori range -> HTTP 400 'valori fuori range plausibile'. Risposta OK: {"result":"ok"}.

## 7. CODICE DEI DENSIMETRI E PONTE BLE (dalla 3.9.0)

- Il codice (Impostazioni → iSpindel → «Codice dei densimetri», pulsante «Genera codice») va
  copiato nel campo «Token/ API key» del portale dell'iSpindel. Con un profilo a densità è
  consigliato: senza, qualunque dispositivo della rete di casa può inviare letture, e la dashboard
  lo segnala («CODICE DEI DENSIMETRI vuoto»). Senza codice la scheda accetta solo letture inviate
  come JSON, il che blocca le pagine web malevole ma non un dispositivo della rete.
- Contratto per un ponte (ESP32 che legge un Tilt o un RAPT Pill via Bluetooth, o un'automazione
  di Home Assistant) che invia a `POST /ispindel`:
  - `Content-Type: application/json`;
  - `name`: unico per densimetro (è con questo che si sceglie quale pilota la cotta);
  - `ID`: numero stabile, per esempio gli ultimi 4 byte del MAC Bluetooth; se manca (0) il
    densimetro si riconosce solo dal nome;
  - `gravity`: nell'unità impostata sulla scheda (SG o °Plato);
  - `temperature` con `temp_units` ("C", "F" o "K"; senza, si intende °C);
  - `battery` in volt (fra 0 e 6; altri valori, per esempio una percentuale, vengono ignorati);
  - `angle` solo se il densimetro lo misura (la taratura guidata lo richiede);
  - `token` se sulla scheda è impostato il codice.