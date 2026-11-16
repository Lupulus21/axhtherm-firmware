# COLLAUDO.md — Checklist di primo avvio e verifica al banco

Da eseguire in ordine. Fermati al primo passo che fallisce e riporta l'output.

## ⚠️ Sicurezza, prima di collegare il riscaldatore

Il firmware comanda il riscaldatore tramite un relè o un SSR. Se questo si guasta in chiusura
(contatti incollati; per un SSR il corto è il guasto tipico) **nessun firmware può spegnerlo**:
i controlli del riscaldatore tolgono il comando e danno l'allarme, ma il riscaldatore resta
acceso finché qualcosa non apre il circuito o interviene una persona. Fuori dalla scheda servono:

- **Differenziale da 30 mA** e magnetotermico sulla linea che alimenta i carichi.
- Riscaldatore appoggiato su una **superficie non combustibile**, lontano da plastica,
  polistirolo, legno e tessuti.

Un termostato bimetallico N/C a riarmo manuale in serie al riscaldatore aprirebbe il circuito
anche con il relè incollato. Scelta del 28/10/2026: **non viene montato**, e dalla 3.10.0 il
firmware non chiede più di confermarlo. Il caso del relè incollato resta coperto solo
dall'allarme: la fermentazione dura giorni senza sorveglianza, quindi conviene che gli allarmi
arrivino al telefono (Home Assistant).

**Prima di iniziare.** Sulla linea 5 V della scheda non c'è alcun serbatoio: `C1` è da 47 pF,
l'unico condensatore a bordo. Un elettrolitico da **470-1000 µF / 10 V** fra VCC e GND del
morsetto POWER assorbe i transitori delle bobine dei relè. Se usi moduli relè esterni, meglio
quelli opto-isolati con `JD-VCC` separato.

## Fase A — Banco, relè SCOLLEGATI dai carichi

1. **Flash e monitor**: segui `docs/FLASH.md` (backup compreso), poi
   `pio device monitor -b 115200`. Atteso al boot: banner `AxHTherm 2.5 — firmware locale v<versione>`,
   nessun reset-loop, `[sys] avvio completato`. Annota l'heap libero (`heap` da seriale, o
   `freeHeap` in `/api/status`): sotto 12000 B fermati e segnalalo.
2. **Boot pulito con GPIO15** (assunzione A6): il solo fatto che il WEMOS parta conferma
   che l'ULN tiene basso D8 al boot. Se non parte (log di boot ROM a 74880 baud), dimmelo.
3. **Scan I2C**: a monitor scrivi `scan i2c` + invio. Attesi: `0x68` (DS1307), `0x50`
   (EEPROM del modulo RTC), `0x3C`/`0x3D` (OLED, se collegato). `0x20` (MCP23008) compare
   solo se il chip è montato: c'è sulle schede PCBWay v3, manca sull'esemplare originale del
   2018 (docs/ANALISI.md:188). In entrambi i casi è normale: i carichi stanno su REL1–3.
4. **Scan sonde**: `scan ow`. Attesi gli ID delle DS18B20 collegate (16 esadecimali ciascuno).
   Annotali: ti servono in Impostazioni → Sonde.
5. **RTC**: `stato` — guarda `trusted`. Se 0: imposta l'ora (`ora <epochUTC>` da seriale,
   o dalla UI "Sincronizza dall'orologio del browser") e verifica che dopo un reset resti `trusted=1`
   (batteria del modulo RTC carica).
6. **Test relè a vuoto** (si sente/vede il click, o si misura con il tester sul connettore RELAY):
   - `rele 1 1` → click; `rele 1 0` → rilascio. Ripeti per 2 e 3. **Sono queste le uscite
     reali**: al banco (15/08/2026) REL1 → cavo riscaldante, REL3 → frigo (REL2 non cablato).
   - `rele 4`–`8`: escono solo sul pettine P1 e passano dal MCP23008. Senza il chip il
     comando risponde "errore": atteso, non un guasto.
   - `test off` per chiudere. Annota QUALE uscita fisica scatta per ciascun numero.
7. **WiFi**: al primo avvio la scheda genera le sue password e le scrive **una volta** sul
   monitor seriale, nel riquadro «CREDENZIALI DI QUESTA SCHEDA» (se c'è l'OLED, compaiono anche
   lì finché la rete di configurazione è attiva). Annotale; se te le perdi, il comando seriale
   `password` le mostra di nuovo. Collegati col telefono alla rete `axhtherm-xxxx-setup` (xxxx =
   ultime 4 cifre esadecimali del chip ID, minuscole) → http://192.168.4.1 → login `admin` con la password annotata → Impostazioni:
   SSID/password di casa, salva (riavvio). Poi raggiungila su http://axhtherm-xxxx.local dalla
   tua rete. Se vuoi una password tua, cambiala in Impostazioni: serve quella attuale.
8. **Sonde in UI**: Impostazioni → Sonde: incolla l'ID della sonda di controllo (e ausiliarie),
   salva. In Dashboard le temperature devono comparire e aggiornarsi (~5 s).
9. **Failsafe sonda**: stacca la sonda di controllo → entro ~30 s allarme "sonda di controllo
   guasta" e uscite spente (erano comunque disarmate). Ricollega → l'allarme rientra. Con le
   sonde avvitate ai morsetti della scheda (scheda di banco) la prova si salta: la logica è
   quella provata nelle versioni precedenti.

## Fase B — Identificazione dei carichi (assunzione A1, la sola vera incognita)

10. Con i carichi ANCORA scollegati: dalla pagina Sistema usa il test relè e con un tester in
    continuità sul connettore RELAY identifica quale REL chiude il circuito del **frigo** e
    quale quello del **cavo riscaldante** (dipende da come è cablato il tuo quadro).
11. In Sistema → Mapping: seleziona Relè FREDDO e Relè CALDO, **ARMA**.

## Fase C — Prova funzionale senza internet (router spento o ESP in una rete isolata)

12. Modalità **termostato** a setpoint vicino alla temperatura ambiente: verifica dai LED/click
    che le uscite commutino nel verso giusto (temperatura sopra il set → scatta il relè FREDDO;
    sotto → CALDO a finestre PID). I LED della scheda riflettono i relè: LED2=REL1, LED1=REL2, LED3=REL3.
13. **Anti short-cycle**: alza e abbassa rapidamente il setpoint per far spegnere il frigo e
    richiederlo subito: in Dashboard deve comparire "protezione compressore: N s" e il relè
    NON deve riscattare prima del riposo (default 5 min).
14. **Riavvio sotto protezione**: col frigo appena spento togli alimentazione all'ESP e
    ridagliela: la protezione deve essere ancora attiva (persistita).
15. **Profilo**: crea un profilo di prova (step 1 fissa 30 min, step 2 rampa a tempo 1 h,
    step 3–5 a piacere), avvialo, verifica avanzamento, pausa/riprendi, salta step,
    setpoint manuale. Spegni/riaccendi a metà rampa: deve riprendere dalla posizione oraria giusta.
16. **UI dal telefono**: dashboard, grafico, editor profilo, export CSV.

## Fase D — Carichi reali

17. Verifica i punti di sicurezza in testa a questo documento (differenziale, superficie non
    combustibile), poi ricollega frigo e cavo riscaldante. Ripeti il punto 12 con i carichi veri e monitora
    il primo ciclo completo (il compressore parte e si ferma senza cicli ravvicinati).
18. **Gli attuatori scaldano e raffreddano davvero.** A carichi collegati e camera
    **disarmata**: `rele <caldo> 1`, attendi 15 minuti, verifica che la temperatura **salga**
    di almeno 0,5 °C; poi `rele <freddo> 1`, attendi 20 minuti, verifica che **scenda**.
    `test off` fra una prova e l'altra. **Da ripetere prima di ogni cotta**, non solo al
    primo collaudo.

    > **Perché questa prova esiste.** Il 25-26/08/2026 il registro mostra 14 ore di comando caldo
    > continuo con setpoint 35 °C e la temperatura **in discesa** da 29,2 a 24,8 °C: il cavo
    > riscaldante non era collegato (prova a banco). Il firmware ha dato l'allarme di
    > scostamento termico dopo tre ore, ma **un attuatore che non attua è invisibile al
    > controllo finché non è troppo tardi**. La prova costa 35 minuti.

19. **iSpindel**: configura sullo iSpindel l'endpoint `http://<IP>/ispindel` (HTTP, porta 80).
    Alla prima pubblicazione la densità compare in Dashboard con l'età del dato; dopo
    60 min senza dati deve comparire l'allarme "densità stantia".
20. (Se usati) MQTT/Home Assistant: abilita in Impostazioni e verifica la discovery.
21. Lascia girare 24 h con log attivo e scarica il CSV: temperatura entro tolleranza,
    nessun short-cycle nel grafico caldo/freddo.

## Fase E — Prova dei controlli sul riscaldatore (a carichi SCOLLEGATI)

Ogni prova si fa con il riscaldatore e il frigo **staccati** dalle uscite: si simula il guasto
con la sonda in un bicchiere d'acqua. Alla fine di ognuna verifica che compaiano l'avviso
«CALDO BLOCCATO» in dashboard, «! CALDO BLOCCATO» sull'OLED e (se usato) l'entità «Caldo
bloccato» in Home Assistant; poi riavvia la scheda (il blocco deve restare) e sblocca dalla
dashboard o con `sblocca` da seriale.

Usa un bicchiere d'acqua **a temperatura ambiente**, lasciato riposare: acqua fredda che si
scalda o acqua calda che si raffredda falserebbero le prove 22 e 24.

22. **Sonda ferma.** In Impostazioni metti «Sonda ferma a caldo acceso» a 15 min. Termostato a
    5 °C sopra la temperatura dell'acqua: il caldo resta acceso di continuo e, col carico
    staccato, la temperatura non sale. Entro 15 minuti: blocco. Rimetti poi il valore di prima.
23. **Salita a caldo spento.** Caldo spento da almeno 10 minuti (termostato basso, o caldo
    sospeso). Metti la sonda del mosto in un bicchiere d'**acqua fredda di frigo** (5–10 °C) e
    lasciala lì: l'acqua si scalda da sola verso la temperatura della stanza, una salita
    graduale come quella di un SSR incollato. Entro 10–20 minuti: blocco «temperatura in salita
    rapida a caldo SPENTO». Non spostare la sonda in un'acqua più calda: i salti oltre 0,5 °C
    fra due letture sono scartati apposta (sonda spostata, offset cambiato) e la prova non
    scatterebbe. Nel frattempo può comparire anche «sonda di controllo SOSPETTA» (diverge dalle
    altre): è giusto.
24. **Freddo inefficace.** Termostato 3 °C sotto la temperatura dell'acqua, frigo staccato: il
    freddo resta comandato e la temperatura non scende. Dopo 2 ore: blocco.
25. **Tempo massimo** (facoltativa: dura almeno 2 ore). Come la prova 22 con «Sonda ferma» a 0
    e «Caldo acceso al massimo» a 2 ore.
26. **Freddo non comandato** (allarme, non blocco; carichi sempre staccati: il guasto si simula
    con la sonda). Sonda del mosto in acqua a temperatura ambiente, sonda «camera» in acqua e
    ghiaccio. Termostato 3 °C sotto la temperatura dell'acqua: il freddo viene comandato.
    Aspetta che il relè del freddo scatti **davvero** (clic o LED: dopo un riarmo il riposo
    del compressore può ritardarlo fino a 5 minuti), lascialo acceso un paio di minuti, poi
    alza il setpoint sopra la temperatura dell'acqua: il relè si apre (dopo la marcia minima).
    **Lascia la sonda «camera» nel ghiaccio**: è l'aria che non risale, come con un frigo che
    non si ferma. Dopo 10 minuti compare «FREDDO NON COMANDATO». Sparisce togliendo la sonda
    dal ghiaccio (l'aria risale) o quando il relè del freddo si richiude.
27. **Limite sull'aria.** Termostato a −1 °C con la sonda del mosto in acqua a temperatura
    ambiente: il freddo è comandato. Metti la sonda «camera» in acqua, ghiaccio e sale (sotto
    −5 °C): la dashboard scrive «freddo sospeso: aria della camera al limite» e il relè del
    freddo si apre (dopo la marcia minima del compressore). Tolta dal ghiaccio, sopra −2 °C il
    freddo riparte, ma solo dopo il riposo del compressore (5 minuti). Toglila **entro 10
    minuti**: se resta più a lungo compare anche «FREDDO NON COMANDATO», ed è giusto (l'aria non
    risale dopo lo stop, proprio come con un relè incollato).

## Fase F — Accesso e aggiornamento (dalla 3.7.0)

28. **Riavvio proposto.** Cambia la rotazione del display e salva: la pagina chiede se riavviare.
    Rispondi no: in dashboard compare «Impostazioni salvate, attive al prossimo riavvio» con il
    pulsante «Riavvia ora». Con le uscite armate e un termostato attivo, la domanda avvisa che i
    relè resteranno spenti per una decina di secondi.
29. **Tentativi sbagliati** (dal computer, per non aspettare a mano): cinque password sbagliate di
    fila con `for i in 1 2 3 4 5 6; do curl -s -u admin:sbagliata http://<IP>/api/status; echo; done`.
    Le prime cinque rispondono «login richiesto»; la sesta, subito dopo, «troppi tentativi
    sbagliati: riprova fra 2 s». Intanto dal telefono (un altro indirizzo) si entra normalmente.
    Dopo un minuto, con la password giusta, si entra anche dal computer.
30. **Nome della scheda.** `curl -i -H "Host: esempio.com" http://<IP>/` risponde 403, e così
    `-H "Host: axhtherm-XXXX.esempio.com"`; con `http://<IP>/` e `http://axhtherm-XXXX.local/` la
    pagina si apre.
31. **Aggiornamento verificato.** Tronca una copia del firmware
    (`head -c 300000 firmware.bin > rotto.bin`) e caricala dalla pagina: risponde subito «file
    incompleto» senza inviarlo. Poi caricala da riga di comando (esempio in FLASH.md, con
    `rotto.bin`): la scheda risponde «file incompleto o con dati in più… niente installato», e
    dopo un riavvio la versione è ancora quella di prima. Infine carica il file intero:
    avanzamento in percentuale, riavvio e pagina ricaricata da sola.
32. **Backup.** Sistema → Scarica backup; cambia un valore (per esempio la tolleranza) e salva;
    Ripristina dal file: il valore torna quello del backup e le password restano valide.

33. **Tipo di guasto delle sonde** (dalla 3.8.0, uscite disarmate; con le sonde avvitate ai
    morsetti si salta, e si guarda la tabella delle sonde durante le 48 ore). Stacca per una trentina di
    secondi il connettore della sonda «camera» e riattaccalo: in Sistema → «Sonde: affidabilità e
    taratura» la riga della camera mostra letture perse con il tipo «la sonda non trasmette». Al
    riattacco possono comparire anche «sonda riavviata» o «dati corrotti», e qualche tentativo
    fallito sulle altre sonde: inserire una sonda a bus acceso disturba la linea comune.

Le prove 34 e 35 si fanno **a profilo fermo** e con il nome del tuo densimetro al posto di
`NOME`: una lettura finta durante un profilo a densità in corsa verrebbe usata davvero, e un
nome inventato resterebbe nel registro dei densimetri.

34. **Temperatura in °F** (dalla 3.9.0, codice dei densimetri ancora vuoto):
    `curl -i -X POST http://<IP>/ispindel -H 'Content-Type: application/json' -d '{"name":"NOME","gravity":1.050,"temperature":68,"temp_units":"F"}'`
    risponde 200 e la lettura arriva con 20,0 °C.
35. **Densimetri senza codice.** La stessa richiesta con `-H 'Content-Type: text/plain'` risponde
    415. Con un profilo a densità caricato la dashboard mostra «CODICE DEI DENSIMETRI vuoto».
    «Genera codice», copialo, salva: ora la richiesta di prima risponde 401, e 200 aggiungendo
    `"token":"<codice>"` al JSON. Ricordati di metterlo anche nel densimetro.
36. **Taratura legata alla sonda.** In Impostazioni → Sonde scambia le sonde di due posizioni
    (cambia entrambe le tendine, poi salva una volta sola): la pagina dice «Sonda cambiata… la taratura segue la sonda» e ogni correzione si sposta
    con la sua sonda. Scegli una sonda mai usata in una posizione: la sua correzione riparte da 0.
37. **Export CSV durante la regolazione** (dalla 3.14.0). Con almeno qualche giorno di registro,
    scarica il CSV dall'Andamento a 14 giorni mentre la dashboard resta aperta in un'altra scheda
    del browser. La dashboard continua ad aggiornarsi, il file si apre completo, e in Sistema
    «cicli in ritardo» non aumenta.
38. **Stack** (dalla 3.14.0). Con MQTT attivo spegni il broker per qualche minuto e riaccendilo,
    tre volte. Poi, dal monitor seriale, `heap`: lo stack libero minimo deve restare sopra
    1.500 B (era 880 B nella 3.13.0).
39. **Registro eventi** (dalla 3.15.0). Sistema → Registro eventi: c'è l'avvio. Arma e disarma
    (a carichi scollegati), avvia e ferma un profilo, stacca la sonda della camera per un minuto:
    compaiono gli eventi con l'ora, e l'allarme solo dopo 30 secondi. Togli la corrente, riaccendi:
    gli eventi ci sono ancora, più il nuovo avvio. Al monitor seriale, `eventi` li elenca.
40. **OLED a pagine** (dalla 3.15.0). Con un profilo in corso il display alterna ogni 5 secondi lo
    stato e la pagina della fermentazione (densità, step, tempo che manca). Con due allarmi
    (per esempio sonda della camera staccata e orologio non impostato) compare la pagina con
    entrambi. In Impostazioni → Display, «Luminosità» cambia lo schermo subito, senza riavvio.
41. **Step «aspetta la mia conferma»** (dalla 3.16.0, uscite disarmate). Profilo di prova con
    due step fissi da 6 minuti, il primo con «aspetta la mia conferma». Dopo 6 minuti: in
    dashboard «Conferma e prosegui», sull'OLED «CONFERMA», in HA «Step in attesa di conferma»
    acceso. Riavvia la scheda: l'attesa c'è ancora. Premi «Conferma e prosegui»: parte il
    secondo step, e nel registro eventi ci sono attesa e conferma.
42. **Comandi MQTT rifiutati** (dalla 3.16.0). A scheda ferma, imposta un setpoint dalla scheda
    climate di HA: su `<base>/cmd_result` arriva `"ok":false,"reason":1` e nel registro eventi
    «comando rifiutato (MQTT)».
43. **Ventola** (dalla 3.17.0, anche senza ventola montata: basta il LED del relè). In
    Impostazioni → Ventola scegli REL2, «segue caldo e freddo», 2 minuti. Arma e avvia il
    termostato con un setpoint che fa partire il freddo: si accendono il LED del freddo e
    quello di REL2. Quando il freddo si ferma, il LED di REL2 resta acceso 2 minuti e poi si
    spegne. Disarma: si spegne tutto. Scegli come ventola lo stesso relè del freddo: la
    dashboard dice «VENTOLA IGNORATA».

## Se qualcosa va storto

Il significato di ogni allarme, con cosa fare, è in ALLARMI.md.


| Sintomo | Causa probabile |
|---|---|
| Allarme `scostamento T` dopo 3 h, uscita comandata ma temperatura ferma o in deriva | **Attuatore che non attua**: carico scollegato, modulo relè o canale ULN guasto. Il firmware può solo segnalarlo: esegui la prova 18 |
| `-127` intermittenti | Bus OneWire troppo lungo o a stella; pull-up unica da 4,7 kΩ |
| `rele 4` risponde errore | MCP23008 non montato o non a 0x20 (`scan i2c`) |
| Salvataggio config rifiutato | Stessa sonda in due slot, o etichetta troppo lunga (max 24) |
| Allarme `SCRITTURA FLASH KO` al boot | `/state.json` illeggibile: lo stato è perso e viene segnalato apposta |
| `CALDO BLOCCATO` | Un controllo di sicurezza ha visto un possibile guasto del riscaldatore o della sonda: il testo in dashboard dice quale. Risolvi la causa, poi sblocca |
| «Salita a caldo spento» quando si apre il frigo | La sonda di controllo è in aria invece che nel pozzetto del fermentatore: l'aria calda la fa salire di colpo. Mettila nel pozzetto |
| «Sonda ferma» con un riscaldatore debole su tanto mosto | Il mosto sale meno di 0,2 °C in un'ora anche a caldo pieno: alza i minuti del controllo o mettilo a 0 in Impostazioni |
| «Salvato, ma riportati nei limiti: …» | Uno o più parametri erano fuori dall'intervallo plausibile: il firmware ha messo il valore limite, e il modulo ora mostra quello in uso |
| Il frigo si ferma prima del target nel cold crash | Limite sull'aria della camera: l'aria è scesa sotto l'aria minima (default −5 °C). È voluto (evita il gelo vicino alle pareti); per toglierlo metti l'aria minima a −10 in Impostazioni |
| `FREDDO NON COMANDATO` | Il frigo sembra non fermarsi quando dovrebbe: relè del frigo incollato? Il firmware non può aprirlo: controlla il relè. Se la sonda «camera» è attaccata all'evaporatore, spostala |
| `RIAVVII A CATENA` | Il firmware si è bloccato più volte appena acceso: gira senza MQTT finché non regge 30 minuti, poi MQTT si riaccende da solo. In Diagnostica, registro dei riavvii: causa ed eccezione. Mandameli |
