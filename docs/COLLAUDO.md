# COLLAUDO.md — Checklist di primo avvio e verifica al banco

Da eseguire in ordine. Fermati al primo passo che fallisce e riporta l'output.

## Fase A — Banco, relè SCOLLEGATI dai carichi

1. **Flash e monitor**: segui `docs/FLASH.md` (backup compreso), poi
   `pio device monitor -b 115200`. Atteso al boot: banner `AxHTherm 2.5 — firmware locale v2.6.0`,
   nessun reset-loop, `[sys] avvio completato`, heap > 20000 B.
2. **Boot pulito con GPIO15** (assunzione A6): il solo fatto che il WEMOS parta conferma
   che l'ULN tiene basso D8 al boot. Se non parte (log di boot ROM a 74880 baud), dimmelo.
3. **Scan I2C**: a monitor scrivi `scan i2c` + invio. Attesi: `0x68` (DS1307), `0x50`
   (EEPROM del modulo RTC), `0x3C`/`0x3D` (OLED, se collegato). **`0x20` (MCP23008) NON
   compare: sull'esemplare reale il chip non è montato** (verificato al banco 15/08/2026,
   docs/ANALISI.md:188) — è normale e atteso, non un guasto. I carichi stanno su REL1–3.
4. **Scan sonde**: `scan ow`. Attesi gli ID delle DS18B20 collegate (16 esadecimali ciascuno).
   Annotali: ti servono in Impostazioni → Sonde.
5. **RTC**: `stato` — guarda `trusted`. Se 0: imposta l'ora (`ora <epochUTC>` da seriale,
   o dalla UI "Sincronizza dall'orologio del browser") e verifica che dopo un reset resti `trusted=1`
   (batteria del modulo RTC carica).
6. **Test relè a vuoto** (si sente/vede il click, o si misura con il tester sul connettore RELAY):
   - `rele 1 1` → click; `rele 1 0` → rilascio. Ripeti per 2 e 3. **Sono queste le uscite
     reali**: al banco (15/08/2026) REL1 → cavo riscaldante, REL3 → frigo (REL2 non cablato).
   - `rele 4`–`8`: **non pilotano nulla** (MCP23008 non montato): il comando risponde
     "errore" — atteso, non un guasto.
   - `test off` per chiudere. Annota QUALE uscita fisica scatta per ciascun numero.
7. **WiFi**: al primo avvio parte il SoftAP `axhtherm-setup` (password `axhtherm25`).
   Collegati col telefono → http://192.168.4.1 → login `admin`/`axhtherm` →
   Impostazioni: SSID/password di casa, **cambia le tre password**, salva (riavvio).
   Poi raggiungilo su http://axhtherm.local dalla tua rete.
8. **Sonde in UI**: Impostazioni → Sonde: incolla l'ID della sonda di controllo (e ausiliarie),
   salva. In Dashboard le temperature devono comparire e aggiornarsi (~5 s).
9. **Failsafe sonda**: stacca la sonda di controllo → entro ~30 s allarme "sonda di controllo
   guasta" e uscite spente (erano comunque disarmate). Ricollega → l'allarme rientra.

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

17. Ricollega frigo e cavo riscaldante. Ripeti il punto 12 con i carichi veri e monitora
    il primo ciclo completo (il compressore parte e si ferma senza cicli ravvicinati).
18. **iSpindel**: configura sullo iSpindel l'endpoint `http://<IP>/ispindel` (HTTP, porta 80).
    Alla prima pubblicazione la densità compare in Dashboard con l'età del dato; dopo
    60 min senza dati deve comparire l'allarme "densità stantia".
19. (Se usati) MQTT/Home Assistant: abilita in Impostazioni e verifica la discovery.
20. Lascia girare 24 h con log attivo e scarica il CSV: temperatura entro tolleranza,
    nessun short-cycle nel grafico caldo/freddo.
