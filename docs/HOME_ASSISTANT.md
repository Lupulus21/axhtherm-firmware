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
| Topic base | `axhtherm` (lascia così) |
| Discovery Home Assistant | ✅ |

**Salva** → il dispositivo si riavvia. Entro un minuto, in HA, sotto *Impostazioni → Dispositivi
e servizi → MQTT* comparirà automaticamente il dispositivo **AxHTherm** con tutte le entità.

## 3. Cosa appare in HA (auto-discovery)

- **Termostato (climate)**: temperatura mosto, setpoint, stato riscalda/raffredda, off/auto.
  Dalla scheda climate imposti il setpoint e spegni; per i profili usa i pulsanti.
- **Sensori**: temperature mosto/camera/ambiente, setpoint, modalità, step, densità, OG,
  attenuazione, ABV, giorni di fermentazione, età densità, batteria iSpindel, memoria, segnale.
- **Sensori binari**: riscaldamento, raffreddamento, uscite armate, profilo in marcia,
  **profilo completato**, allarme, densità stantia, batteria bassa.
- **Pulsanti**: Avvia / Ferma / Pausa / Riprendi / Salta step del profilo.

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
    payload: '{"name": "Tilt", "gravity": {{ states("sensor.tilt_gravity") | float(0) }}, "temperature": {{ states("sensor.tilt_temperature") | float(0) }}}'

> **Il campo `name` conta, con due camere.** Ogni camera segue solo il densimetro che le è
> assegnato in Impostazioni, e l'abbinamento è **per nome**. Dando un nome al Tilt lo si può
> assegnare a una camera precisa; una lettura **senza** `name` viene sempre attribuita alla
> **camera 1**, che è il comportamento storico ed è il motivo per cui questa ricetta continua
> a funzionare anche senza il campo. Se hai impostato un token iSpindel, va aggiunto al
> payload, altrimenti la richiesta prende 401.

automation:
  - alias: "AxHTherm · inoltra densità dal Tilt"
    trigger:
      - platform: state
        entity_id: sensor.tilt_gravity
    action:
      - service: rest_command.axhtherm_gravity
```

## 8. Accesso da remoto

- **Nabu Casa** (abbonamento ufficiale HA): il modo più semplice e sicuro, un clic.
- **Self-host**: reverse proxy (es. Nginx Proxy Manager + Let's Encrypt) o una VPN verso casa.

In tutti i casi **è HA a essere esposto**, con il suo login e la sua crittografia; il termostato
resta sulla LAN e non tocca mai internet.

---

### Se HA è spento o irraggiungibile
Il termostato continua a controllare frigo e cavo, la web UI locale
(`http://axhtherm.local` / IP) resta pienamente utilizzabile, iSpindel continua a pubblicare.
Il client MQTT ritenta la connessione in sottofondo, senza mai fermare il loop di controllo.
