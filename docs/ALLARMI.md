# Allarmi: cosa significano e cosa fare

Riferito al firmware 3.15.0. Ogni allarme compare in dashboard (riquadro rosso o giallo in alto)
e, se c'è il display, nella riga in basso dell'OLED. In Home Assistant accende l'entità
«allarme», con due eccezioni: la batteria del densimetro ha un'entità sua, e durante i riavvii
a catena MQTT è spento.
Sull'OLED la riga in basso mostra l'allarme più importante, nell'ordine di questa tabella, con
quanti altri ce ne sono («+2»). Con due o più allarmi una pagina dell'OLED li elenca tutti
(dalla 3.15.0). Ogni allarme che resta acceso o spento per almeno 30 secondi finisce anche nel
registro eventi (Sistema → Registro eventi), con l'ora.

Legenda della colonna «Il firmware»:
- **solo allarme**: la regolazione continua come prima;
- **sospende**: toglie un verso (caldo o freddo) finché la condizione dura, poi riprende da solo;
- **blocca**: toglie il caldo fino allo sblocco manuale, anche dopo un riavvio;
- **ferma**: spegne entrambe le uscite.

## Guasti in corso

| OLED | Dashboard (inizio del testo) | Il firmware | Cosa fare |
|---|---|---|---|
| `! SONDA GUASTA` | sonda di controllo guasta o assente | **ferma** | Controlla la sonda del mosto: connettore, cavo, morsetti. Sistema → «Sonde: affidabilità e taratura» mostra il tipo di guasto (nessuna risposta, dati corrotti…). Il controllo riparte da solo quando la sonda torna a leggere |
| `! SONDA MOSTO SOSPETTA` | sonda di controllo SOSPETTA | solo allarme | La sonda del mosto legge diverso da tutte le altre: probabilmente è uscita dal pozzetto o ha perso il contatto termico. Rimettila a posto |
| `! USCITE (MCP) KO` | driver uscite (MCP23008) in errore | **ferma** | Il chip che pilota i relè non risponde: controlla l'alimentazione della scheda e il bus I2C (Sistema → Scan I2C) |
| `! CONFIG NON VALIDA` | configurazione non valida | **ferma** | La configurazione salvata non si legge: riapri Impostazioni, controlla e salva (o ripristina un backup da Sistema) |
| `! CALDO BLOCCATO` | CALDO BLOCCATO: … (quattro motivi, vedi sotto) | **blocca** il caldo | Leggi il motivo in dashboard, risolvi la causa, poi «Sblocca il caldo» |
| `! MOSTO TROPPO FREDDO` | MOSTO TROPPO FREDDO | solo allarme | Il mosto è sotto −2 °C, o 2 °C sotto il setpoint con l'antigelo acceso: ambiente gelato, riscaldatore debole, staccato o bloccato. Una birra al 5% gela intorno a −2 °C: intervieni |
| `! FRIGO NON SI FERMA` | FREDDO NON COMANDATO | solo allarme | L'aria resta fredda con il freddo spento: relè del frigo incollato? Il firmware non può aprirlo. Stacca il frigo dalla presa e controlla il relè. Se la sonda «camera» tocca l'evaporatore, spostala |
| `! LIMITE ASSOLUTO T` | MOSTO FUORI DAI LIMITI ASSOLUTI | **sospende** il verso che peggiora | Il mosto è oltre 42 °C o sotto −3 °C: qualcosa a monte ha sbagliato (sonda, cablaggio, relè). Controlla subito |
| `! RIAVVII A CATENA` | RIAVVII A CATENA | MQTT spento | Il firmware si è bloccato più volte poco dopo l'avvio. La regolazione continua. Sistema → «Ultimi riavvii» dice il motivo; MQTT torna da solo dopo 30 minuti stabili |
| `! SCOSTAMENTO T` | scostamento termico | solo allarme | Da 3 ore il mosto resta a più di 5 °C dal setpoint senza avvicinarsi: frigo o riscaldatore guasto, porta aperta, impianto sottodimensionato, oppure un limite sull'aria che interviene sempre |
| `! COLD CRASH LENTO` | COLD CRASH LENTO | solo allarme | In 6 ore il mosto è sceso meno della soglia impostata: frigo che non ce la fa, porta aperta, sonda fuori dal mosto. La discesa continua comunque |
| `! SCRITTURA FLASH KO` | scrittura su flash fallita | solo allarme | Stato e registro non vengono salvati: dopo un riavvio si riparte da dati vecchi. Riduci i giorni di registro in Impostazioni; se continua, la flash è guasta |
| `! FAILSAFE NON ATTIVO` | il timer del failsafe non sta girando | solo allarme | La protezione contro il blocco del programma è spenta: riavvia la scheda; se resta, riflasha |
| `! FAILSAFE: LOOP BLOCCATO` | FAILSAFE | il timer ha spento i relè, poi la regolazione è ripresa da sola | Il programma si è bloccato per qualche secondo. L'allarme resta finché non riarmi, per prendere in carico l'evento. Se succede spesso, guarda «Ultimi riavvii» in Sistema |
| `! SONDA CAMERA KO` | sonda della camera non valida | limiti sull'aria disattivati | Controlla la sonda «camera» (aria). La regolazione sul mosto continua |
| `! DENSITA' STANTIA` | densita' stantia | lo step a densità non avanza | Il densimetro non trasmette da troppo: batteria, WiFi, indirizzo del server nel densimetro |
| `! RAMPA FERMA` | rampa gradi/ora ferma | lo step non avanza | Il mosto non riesce a seguire la rampa: impianto troppo lento per quella velocità |
| `! PIU' DENSIMETRI` | piu' densimetri attivi senza selezione | nessun densimetro pilota la cotta | Scegli in Impostazioni → «Calibrazione densimetro» quale densimetro usa questa camera |
| `! OROLOGIO INAFFIDABILE` | orologio inaffidabile | i tempi degli step potrebbero sbagliare | Sistema → «Sincronizza dall'orologio del browser», oppure attiva l'NTP |
| `! BATT. iSPINDEL` | batteria iSpindel bassa | solo allarme | Ricarica il densimetro |
| `! FERMENTAZ. FERMA?` | FERMENTAZIONE FERMA? | solo avviso (non entra in «allarme» di HA) | Dalla 3.16.0. La densità è ferma da 48 ore ma l'attenuazione è sotto il 50%, o lo step a densità non ha ancora raggiunto la sua soglia. Controlla temperatura e lievito (una scossa al fermentatore, un grado in più), e che il densimetro non sia incrostato o appoggiato. Per le birre che finiscono alte di proposito si disattiva in Impostazioni → Densimetro |

### I quattro motivi del caldo bloccato

| Testo | Cosa è successo | Cosa controllare |
|---|---|---|
| …la sonda di controllo non sale | caldo acceso di continuo per il tempo impostato e il mosto non sale | sonda fuori dal mosto o staccata dal fermentatore; riscaldatore non collegato |
| …oltre il tempo massimo | caldo acceso di continuo oltre il massimo impostato | riscaldatore troppo debole o staccato, sonda |
| …in salita rapida a caldo SPENTO | il mosto sale di oltre 1 °C in 20 minuti con il caldo spento | SSR del caldo incollato: è il guasto tipico degli SSR. Togli la presa del riscaldatore e verifica |
| …freddo acceso da ore e temperatura che non scende | il frigo lavora da 2 ore e il mosto non scende | riscaldatore che lavora contro il frigo (SSR incollato), o frigo guasto |

Lo sblocco si fa dalla dashboard («Sblocca il caldo»), dopo aver capito la causa. Il freddo resta
sempre consentito. Il blocco toglie il comando al caldo, ma non può aprire un relè con i contatti
incollati: con «in salita rapida a caldo SPENTO» stacca il riscaldatore dalla presa.

## Configurazione e sicurezza

| Dashboard | Cosa fare |
|---|---|
| uscite DISARMATE | Normale dopo un riflash o un reset: controlla in Sistema → «Mapping uscite e armamento» quale relè fa il caldo e quale il freddo, poi arma |
| PASSWORD DI DEFAULT attiva | Scheda configurata prima della 3.4.0: cambia la password in Impostazioni (finché resta, l'aggiornamento via rete è bloccato) |
| NESSUNA PASSWORD impostata | Chiunque sulla rete può comandare la scheda: imposta una password |
| PASSWORD interfaccia troppo corta | Sceglila di almeno 8 caratteri |
| PASSWORD della rete di configurazione non valida | Sceglila di almeno 8 caratteri (Impostazioni → Rete WiFi) |
| CODICE DEI DENSIMETRI vuoto con un profilo a densità | Chiunque sulla rete può inviare letture finte che fanno avanzare gli step a densità: Impostazioni → iSpindel → «Genera codice», salva e copialo nel densimetro |
| STEP PARTITO SENZA IL TARGET | Uno step «parte all'arrivo in temperatura» ha aspettato 24 ore senza che il mosto ci arrivasse: il suo tempo conta comunque. Controlla frigo e riscaldatore; se va bene così, ignoralo: sparisce al cambio di step |
| Impostazioni salvate, attive al prossimo riavvio | Hai cambiato rete, display, MQTT o orologio: «Riavvia ora» quando la cotta lo permette (i relè restano spenti una decina di secondi) |

## Fasce in cima alla pagina

| Fascia | Significato | Cosa fare |
|---|---|---|
| rossa, CONNESSIONE PERSA | la pagina non riceve risposte dalla scheda; i valori in grigio sono vecchi | WiFi del telefono, alimentazione della scheda. La regolazione continua anche senza pagina |
| arancione, PAGINA IN ERRORE | la scheda risponde ma la pagina non riesce a mostrare i dati | ricarica la pagina; se resta, segnala il messaggio |
| pannello di accesso | sessione scaduta o password cambiata | rientra con la password |

## Messaggi dell'accesso e dell'aggiornamento

| Messaggio | Cosa fare |
|---|---|
| troppi tentativi sbagliati: riprova fra N s | Dopo 5 password sbagliate di fila da questo dispositivo ogni tentativo aspetta (fino a 60 s). Aspetta e riprova con calma. Se succede da solo, uno script o Home Assistant usa ancora la password vecchia |
| Nome non riconosciuto | Apri la scheda con il suo indirizzo IP o con `axhtherm-XXXX.local` |
| file incompleto / file danneggiato (CRC diverso) | Il file del firmware è rovinato o scaricato a metà: non è stato installato nulla. Riscaricalo o ricompilalo |
| non è un firmware.bin per questa scheda | Hai scelto un `.bin.gz` o un altro file: carica `firmware.bin` |
| aggiornamento via rete disattivato | Attivalo in Impostazioni → OTA |

## Tipi di guasto delle sonde

Sistema → «Sonde: affidabilità e taratura» dice, per ogni sonda, quante letture sono fallite al
primo tentativo, quante sono state recuperate rileggendo e di che tipo erano i guasti:

| Tipo | Cosa significa | Dove guardare |
|---|---|---|
| la sonda non trasmette | il bus funziona ma questa sonda non risponde (si leggono solo 1) | connettore, cavo o saldatura di QUESTA sonda; sonda staccata |
| nessuna sonda risponde | nessuna sonda risponde al reset del bus | linea dati in corto a massa, bus interrotto vicino alla scheda, resistenza di pull-up mancante: guarda il cablaggio comune, non la sonda |
| linea a massa durante la lettura | si leggono solo 0 | corto intermittente fra dati e massa |
| dati corrotti | la sonda risponde ma il controllo d'integrità fallisce | disturbi (cavo vicino a quello del compressore), cavo lungo, resistenza di pull-up |
| sonda riavviata | la sonda legge 85 °C, il valore che ha all'accensione | alimentazione instabile della sonda (contatto del +, cavo lungo con alimentazione parassita) |
| fuori scala | dati integri ma temperatura fuori da −20…60 °C | sonda guasta, o davvero fuori scala |

Qualche tentativo fallito al giorno, recuperato rileggendo, è normale. Una percentuale che sale o
un tipo che si ripete indica dove intervenire prima che la sonda si guasti del tutto.

## Righe dell'OLED

```
21.4°            > 20.0      temperatura di controllo · setpoint
PROFILO 2/5  [DISARMATO]     modalità: FERMO, TERMOSTATO, PROFILO passo/totale (PAUSA, FINE, ATTESA),
                             COLD CRASH (in discesa), COLD CRASH: OK (temperatura raggiunta)
RAFFREDDA                    uscite: RISCALDA, RAFFREDDA, riposo, attesa frigo N s, TEST RELE' ATTIVO
! SONDA GUASTA               allarme più importante, oppure l'indirizzo IP ((AP) = rete di configurazione)
```

Alla prima configurazione (nessuna rete di casa impostata) l'OLED mostra invece il nome della rete
di configurazione e la password per entrarci.

Per i guasti durante il montaggio e il collaudo vedi anche COLLAUDO.md, «Se qualcosa va storto».
