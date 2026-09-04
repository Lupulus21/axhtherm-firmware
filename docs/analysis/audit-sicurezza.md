# Audit di sicurezza — AxHTherm 2.5

Campagna del 2026-09-01, 4 lenti (una persa per sospensione del computer: **autenticazione e
sessione non è stata completata**). Ogni rilievo qui sotto è stato **riverificato a mano**, anche
nel sorgente del framework Arduino ESP8266.

> **Tutti i CRITICO sono PREESISTENTI nella baseline v2.23.2**, verificato con `git show ad1b50d`.
> Nessuno è stato introdotto dal refactor a 2 camere.

## Il punto centrale: qui la disponibilità *è* sicurezza

Il web server gira **dentro** l'unico loop cooperativo (`web.loop()` → `server_.handleClient()`,
`main.cpp`). Mentre è bloccato non girano `controlTick`, `probes.loop`, `outputs.loop`,
`mqtt.loop` né `persistNow` — e **i relè restano latchati nell'ultimo stato**, perché
`OutputBus::writeRel` è un `digitalWrite`/OLAT senza failsafe hardware. Lo dichiara il progetto
stesso nel commento all'OTA: *«il loop è congelato e i relè restano latchati»*.

Aggravante decisiva: le attese usano `yield()`, che **nutre sia il soft-WDT sia il watchdog
hardware**. Nessun riavvio salvifico: il dispositivo resta vivo e fermo.

Questo contraddice il principio non negoziabile n.5 dichiarato in `ARCHITETTURA.md`:
*«Il loop di controllo non si blocca mai»*.

## CRITICO

| ID | Titolo | Verifica |
|---|---|---|
| S1 | **Connessione TCP "a goccia" congela il controllo, senza autenticazione.** Gli header si leggono con `client.readStringUntil('\r')` (`Parsing-impl.h:47,125`); `Stream::timedRead` **riazzera `_startMillis` a ogni byte** con `_timeout` = 5000 ms (`WiFiClient.cpp:80`) e chiama `yield()`. Un client che invia un byte ogni ~4 s dentro una riga di header non fa mai scadere il timeout e non chiude mai la riga | Sorgente del framework letto: `Stream.cpp:30-42` conferma il timeout **per byte**, non per richiesta |
| S2 | **Upload multipart verso `/api/update` congela il controllo, senza autenticazione.** Il framework consuma il corpo in `_parseForm` **prima** che l'handler applicativo giri, e `_uploadReadByte` attende con `while(!client.available() && client.connected()) yield();` — **nessun timeout** (`Parsing-impl.h:339`). Il controllo `otaAuthorized_` a valle fa solo `return`: il parser continua a leggere | Confermato in `Parsing-impl.h:339`. **Il percorso non avversariale è il più probabile**: una OTA legittima dal telefono che esce dal WiFi senza chiudere il TCP lascia il controllore congelato, potenzialmente col compressore eccitato |
| S3 | **`gravityCorrection` senza limiti → corruzione dello stack.** `ConfigStore::fromJson:220` la legge senza clamp, mentre `probeOffset` a **riga 162** è clampato a ±5 con test di appartenenza NaN-safe. Il valore finisce in `String(float, 4)`, che usa `char buf[33]` + `dtostrf` (`WString.cpp:108-109`); `dtostrf` gestisce NaN/inf ma **non ha alcun controllo di lunghezza** per un finito grande: ~3,4e38 scrive ~45 byte in 33 | Asimmetria verificata nel codice. Richiede un `PUT /api/config` **autenticato**. Persistente: il valore resta in `/config.json`, e via cattura automatica dell'OG può finire in `/state.json` e riprodursi a ogni `/api/status` |

## Cosa regge davvero

Non è un colabrodo: la sicurezza **funzionale** è solida, ed è quella che protegge la birra.
Uscite disarmate alla nascita; qualunque condizione dubbia → tutto spento; clamp del setpoint a
`[-1,+40]` in un unico choke point NaN-safe; anti-corto-ciclo persistito attraverso i riavvii;
interlock strutturale; scritture atomiche verificate; OTA sbarrata finché la password è quella di
default o vuota (`authValid()`, non `auth()`); segreti mascherati nelle risposte; locking
ottimistico sulla configurazione.

## Cosa NON c'è, per scelta dichiarata

`ARCHITETTURA.md` fissa il modello di minaccia: *«LAN domestica, HTTP in chiaro — modello
accettato»*. Coerentemente: **nessun TLS** (né HTTP né MQTT); token di sessione
`MD5(uiPassword + chipId + ":axsession")` **statico**, cookie da un anno, nessuna rotazione —
chi lo intercetta una volta ha accesso finché non cambi la password; rate limiting sul login =
`delay(300)`, nessun lockout; una sola credenziale, nessun ruolo, nessun audit trail;
`/ispindel` accetta POST non autenticati (con token opzionale).

Non sono sviste: sono conseguenze del modello. Ma vanno chiamate col loro nome — **questo non è
un profilo di sicurezza "enterprise"**, e non è progettato per esserlo.

## Limite di questa campagna

La lente su **autenticazione e sessione** non è stata completata (il computer è andato in
sospensione a metà). Quanto scritto sopra su cookie e login viene da verifiche diverse, mie e
dirette, non da quella lente: va considerato **non esaustivo**.
