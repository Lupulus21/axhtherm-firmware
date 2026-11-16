# AxHTherm + Home Assistant — guida di configurazione

Home Assistant diventa l'hub: da lì hai monitoraggio, controllo, **accesso da remoto**,
**notifiche** e l'inoltro a **BrewFather** e **ThingSpeak**. Il termostato resta comunque
autonomo — se HA è spento, il controllo locale e la web UI continuano a funzionare.

Il termostato parla **MQTT in locale** con HA (nessun collegamento cloud dal dispositivo).

---

## 1. Prerequisiti in Home Assistant

1. **Broker MQTT.** Il più semplice: *Impostazioni → Componenti aggiuntivi → Mosquitto broker* → Installa e avvia.
2. **Integrazione MQTT.** *Impostazioni → Dispositivi e servizi → Aggiungi → MQTT* (punta al broker Mosquitto locale). Crea un utente/password per il broker (in *Impostazioni → Persone*, o gli utenti del componente Mosquitto).

Annota **indirizzo IP di HA**, **utente** e **password** del broker.

## 2. Configurare il termostato

Dalla web UI del termostato → **Impostazioni → MQTT / Home Assistant**:

| Campo | Valore |
|---|---|
| Abilitato | ✅ |
| Broker | l'IP di Home Assistant (es. `192.168.1.x`) |
| Porta | `1883` |
| Utente / Password | quelli del broker MQTT |
| Topic base | lascia il valore proposto: `axhtherm-XXXX` (chip ID) sulle schede configurate dalla 3.4.0, così due schede non si sovrappongono |
| Discovery Home Assistant | ✅ |

**Salva** → la pagina propone il riavvio, che serve per attivare MQTT: accetta. Entro un minuto, in HA, sotto *Impostazioni → Dispositivi
e servizi → MQTT* comparirà automaticamente il dispositivo, con il nome della scheda
(`axhtherm-XXXX`) e tutte le entità. Con due schede compaiono due dispositivi distinti.

## 3. Cosa appare in HA (auto-discovery)

- **Termostato (climate)**: temperatura mosto, setpoint (da −1 a 40 °C), stato
  riscalda/raffredda, off/auto. Dalla scheda climate imposti il setpoint e spegni; per i profili
  usa i pulsanti.
- **Sensori**: temperature mosto/camera/ambiente, setpoint, modalità, step, densità, OG,
  attenuazione, ABV, giorni di fermentazione, età densità, batteria iSpindel, memoria, segnale,
  ritardo massimo del ciclo di controllo dall'avvio, pendenza della temperatura (°C/h),
  **cold crash** (`-`, `in discesa`, `raggiunto`) e **cold crash: arrivo stimato** (minuti).
  Dalla 3.16.0 la densità ha l'unità (SG o °P), e ci sono anche:
  - **Fermentazione**: `attiva`, `rallenta`, `ferma` o `-` (dati insufficienti), calcolata
    sulle medie orarie del densimetro;
  - **Calo della densità**: SG o °P al giorno, sulle ultime 24 ore, negativo quando cala;
  - **Allarmi attivi**: i codici di `docs/ALLARMI.md` separati da virgola (per esempio
    `SONDA_CAMERA,OROLOGIO`), oppure `nessuno`.
- **Sensori binari**: riscaldamento, raffreddamento, uscite armate, profilo in marcia,
  **profilo completato**, allarme, densità stantia, batteria bassa, **caldo bloccato** (un
  controllo di sicurezza ha bloccato il riscaldatore: si sblocca solo dalla UI o da seriale;
  il motivo è nel campo `heat_lock_code` dello stato), **failsafe non attivo**, **cold crash
  raggiunto** (si accende quando il mosto arriva alla temperatura di cold crash), **cold crash
  lento** (in 6 ore è sceso meno della soglia), **antigelo attivo** e **mosto troppo freddo**. I sensori di problema accendono anche il sensore
  «allarme». Dalla 3.16.0 anche:
  - **Fermentazione finita**: densità ferma da 48 ore, con attenuazione almeno del 50%;
  - **Fermentazione ferma?**: densità ferma da 48 ore ma sopra l'atteso. È un avviso, non
    entra in «allarme», e si disattiva in Impostazioni;
  - **Step in attesa di conferma**: uno step con «aspetta la mia conferma» è finito;
  - **Ventola** (3.17.0): accesa o spenta, se ne hai assegnata una in Impostazioni.
- **Pulsanti**: Avvia / Ferma / Pausa / Riprendi / Salta step del profilo, e **Conferma step**
  (3.16.0).

> Esito dei comandi (3.16.0): ogni comando ricevuto su `<base>/cmd/…` ha una risposta su
> `<base>/cmd_result`, per esempio `{"cmd":"climate_temp","ok":false,"reason":1}`. Motivi:
> 1 setpoint ignorato (scheda ferma o profilo finito), 2 avvio rifiutato (profilo già in corso),
> 3 avvio fallito, 4 valore o comando non valido, 5 niente da confermare. I rifiuti finiscono
> anche nel registro eventi della scheda.

> L'entità climate «off/auto»: *off* ferma tutto; *auto* (da fermo) avvia la modalità
> termostato al setpoint impostato. I profili si avviano/gestiscono coi pulsanti.

## 4. Notifiche (esempi — `configuration.yaml` o editor automazioni)

Sostituisci `axhtherm` negli `entity_id` col nome reale che HA assegna (lo vedi cliccando l'entità).

```yaml
automation:
  - alias: "AxHTherm · fermentazione completata"
    trigger:
      - platform: state
        entity_id: binary_sensor.axhtherm_profilo_completato
        to: "on"
    action:
      - service: notify.notify        # notifica sull'app HA / dispositivi predefiniti
        data:
          title: "🍺 Fermentazione completata"
          message: "Il profilo è arrivato all'ultimo step."

  - alias: "AxHTherm · allarme"
    trigger:
      - platform: state
        entity_id: binary_sensor.axhtherm_allarme
        to: "on"
    action:
      - service: notify.notify
        data:
          title: "⚠️ AxHTherm in allarme"
          message: "Controlla il termostato (sonda, orologio o configurazione)."

  - alias: "AxHTherm · cold crash raggiunto"
    trigger:
      - platform: state
        entity_id: binary_sensor.axhtherm_cold_crash_raggiunto
        to: "on"
    action:
      - service: notify.notify
        data:
          title: "❄️ Cold crash raggiunto"
          message: "La birra è arrivata alla temperatura di cold crash: resta lì finché non fermi il profilo."

  - alias: "AxHTherm · fermentazione finita"
    trigger:
      - platform: state
        entity_id: binary_sensor.axhtherm_fermentazione_finita
        to: "on"
    action:
      - service: notify.notify
        data:
          title: "🍺 Fermentazione finita"
          message: "La densità è ferma da 48 ore: puoi pensare al cold crash o all'imbottigliamento."

  - alias: "AxHTherm · step da confermare"
    trigger:
      - platform: state
        entity_id: binary_sensor.axhtherm_step_in_attesa_di_conferma
        to: "on"
    action:
      - service: notify.notify
        data:
          title: "✋ Tocca a te"
          message: "Uno step è finito e aspetta la conferma (dry hop, campione, travaso)."

  - alias: "AxHTherm · batteria iSpindel bassa"
    trigger:
      - platform: state
        entity_id: binary_sensor.axhtherm_batteria_ispindel_bassa
        to: "on"
    action:
      - service: notify.notify
        data:
          message: "Batteria iSpindel bassa: ricaricala presto."
```

## 5. BrewFather (Custom Stream)

In BrewFather: *Settings → Power-ups → Custom Stream* → attiva e copia l'URL
(`https://log.brewfather.net/stream?id=XXXXXXXX`). Poi in HA:

```yaml
rest_command:
  brewfather_push:
    url: "https://log.brewfather.net/stream?id=XXXXXXXX"
    method: POST
    content_type: "application/json"
    payload: >
      {
        "name": "AxHTherm",
        "temp": {{ states('sensor.axhtherm_temperatura_mosto') | float(0) }},
        "temp_unit": "C",
        "gravity": {{ states('sensor.axhtherm_densita') | float(0) }},
        "gravity_unit": "G"
      }

automation:
  - alias: "AxHTherm · push BrewFather ogni 15 min"
    trigger:
      - platform: time_pattern
        minutes: "/15"          # BrewFather accetta al massimo 1 invio ogni 15 min
    condition:
      - condition: template
        value_template: "{{ states('sensor.axhtherm_densita') not in ['unknown','unavailable',''] }}"
    action:
      - service: rest_command.brewfather_push
```

## 6. ThingSpeak

Usa l'integrazione nativa: aggiungi al `configuration.yaml` il blocco `thingspeak:` con la tua
*Write API Key* e l'elenco delle entità da inviare (temperature, densità). Vedi la documentazione
HA dell'integrazione **ThingSpeak**. In alternativa, un `rest_command` verso
`https://api.thingspeak.com/update` sullo stesso schema del punto 5.

## 7. Tilt hydrometer (se vuoi)

Se la macchina che ospita HA ha il **Bluetooth** (Raspberry Pi, o una chiavetta USB BT da ~5 €):
attiva l'integrazione **Tilt Hydrometer** di HA — legge il Tilt direttamente. Se vuoi che anche il
termostato conosca la densità (per i profili «attesa densità»), aggiungi un'automazione che inoltra
la lettura al suo endpoint:

```yaml
rest_command:
  axhtherm_gravity:
    url: "http://192.168.1.50/ispindel"
    method: POST
    content_type: "application/json"
    payload: '{"name": "Tilt", "gravity": {{ states("sensor.tilt_gravity") | float(0) }}, "temperature": {{ states("sensor.tilt_temperature") | float(0) }}, "temp_units": "{{ "F" if state_attr("sensor.tilt_temperature", "unit_of_measurement") == "°F" else "C" }}"}'

automation:
  - alias: "AxHTherm · inoltra densità dal Tilt"
    trigger:
      - platform: state
        entity_id: sensor.tilt_gravity
    action:
      - service: rest_command.axhtherm_gravity
```

> **Il campo `name`.** Se in Impostazioni hai scelto un densimetro preciso, il termostato accetta
> solo le letture con quel nome: in quel caso scegli «Tilt» o togli il filtro. Una lettura
> **senza** `name` viene sempre accettata. Se hai impostato il codice dei densimetri, va aggiunto
> al payload come `"token": "..."`, altrimenti la richiesta prende 401. Dalla 3.9.0 la scheda
> legge `temp_units`, quindi un Tilt in °F arriva corretto.


## 8. Accesso da remoto

- **Nabu Casa** (abbonamento ufficiale HA): il modo più semplice e sicuro, un clic.
- **Self-host**: reverse proxy (es. Nginx Proxy Manager + Let's Encrypt) o una VPN verso casa.

In tutti i casi **è HA a essere esposto**, con il suo login e la sua crittografia; il termostato
resta sulla LAN e non tocca mai internet.

---

### Se HA è spento o irraggiungibile
Il termostato continua a controllare frigo e cavo, la web UI locale
(`http://<nome>.local` / IP) resta pienamente utilizzabile, iSpindel continua a pubblicare.
Il client MQTT ritenta la connessione in sottofondo, senza mai fermare il loop di controllo.
