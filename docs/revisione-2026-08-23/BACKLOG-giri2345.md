# Backlog — rilievi dei giri 2-5 non ancora corretti (2026-08-25)

I giri 2-5 (per archetipo/assenza/percorso) + critico di completezza hanno prodotto 32 rilievi
nuovi (5 critici, 13 gravi, 14 minori; 4 regressioni). Corretti subito i 9 critici/gravi più
puliti (vedi CORREZIONI.md, sezione giri 2-5). Restano da affrontare:

## Critici / gravi
- **[critico] Step a densità appeso per sempre con dato FRESCO ma condizione mai soddisfatta**
  (Below con target irraggiungibile, o Stable resettato dal rumore), maxDurationSec=0: nessun
  allarme (la rampa °C/h ora ce l'ha, C14). Direzione: escalation a `alarmGravity` dopo N giorni
  (es. 21) con orologio attendibile, e/o rendere `profileNoLimit` visibile su OLED/MQTT.
- **[grave] Nessun allarme di scostamento/fuga termica**: temperatura di controllo lontana dal
  setpoint per ore senza segnalazione (guasto attuatore, sonda giusta ma impianto ko). Direzione:
  allarme se |T-setpoint| oltre una banda per un tempo, su tutti i canali.
- **[grave] Metriche di fermentazione (attenuazione/ABV/attività/«densità stabile ✓») calcolate da
  una lettura del densimetro CONGELATA e mostrate come attuali** senza il marchio STANTIA. Direzione:
  marcare/attenuare le metriche derivate quando `gravityStale`.
- **[grave] /api/config concorrente sovrascrive la taratura appena fatta** (last-write-wins
  sull'array probes). Direzione: locking ottimistico (ETag/versione sul config) — cambio di design.
- **[grave] Nome iSpindel troncato a 16 byte in 3 punti**: due densimetri che collidono nei primi
  16 byte pilotano entrambi la cotta senza far scattare l'allarme «più densimetri». Direzione:
  ampliare il campo nome o confrontare l'ID oltre al nome.

## Minori
- **[minore, regr C14] Il timeout di sicurezza della rampa °C/h non è impostabile dall'editor**
  (campo maxDurationSec nascosto per gli step ramp_rate): il fix C14 è configurabile solo via API.
- **[minore] GET /api/profiles/{id}** risponde 404 per un profilo esistente ma illeggibile
  (OOM/corrotto): la distinzione del fix C30 è solo nella lista, non nel GET singolo. Direzione:
  metodo `ProfileStore::exists(id)` → 500 se esiste ma non si legge.
- **[minore] Conferma factory reset** enumera «config, stato, profilo» ma cancella anche libreria
  (20 ricette), punti calibrazione e registro densimetri. Direzione: correggere il testo in UI.
- **[minore] Nome cotta troncato a 40 byte** (la pagina limita a 40 CARATTERI): con accenti il
  nome salvato differisce e il taglio può spezzare UTF-8. Direzione: trim su confine UTF-8 o rifiuto.
- **[minore] Registro/OG non portano l'unità di densità**: cambio SG↔Plato (permesso in Termostato/
  Off) reinterpreta la cronologia sulla scala sbagliata. Direzione: marcare il log con l'unità.
- **[minore] JSON discovery MQTT climate senza `overflowed()`**: base topic lungo → config troncata
  verso Home Assistant. Direzione: controllo overflow + buffer al caso peggiore.
- **[minore] dtSec del controllo cablato a 1.0** mentre il tick reale è variabile: PID e stimatore
  predittivo su tempo falso quando il loop rallenta. Direzione: passare il dt reale (clampato).
- **[minore] Priorità fissa degli allarmi OLED**: `OROLOGIO INAFFIDABILE` (persistente) nasconde
  storage/rampa/densità. Direzione: riordinare o far ruotare la riga fra gli allarmi attivi.
- **[minore] RUNPROF non invalidato in modo robusto** al riordino delle risposte (archetipo 21):
  il piano in esecuzione può restare vecchio dopo un cambio cotta da un altro client.

## Note
Tutti verificati per lettura del codice ATTUALE (con i fix applicati). I 4 marcati "regressione"
nascono dai fix precedenti; quello del timbro boot (C31) e il completamento del bit allarme sono
già corretti. Materiali: docs/revisione-2026-08-23/giri2345-completo.json (referti per esteso,
incluso il critico di completezza).
