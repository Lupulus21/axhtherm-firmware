# AxHTherm 3.17 — controllore di fermentazione

Firmware per **WEMOS D1 mini (ESP8266)** su scheda **AxHTherm 2.5**: governa **una camera di
fermentazione** (frigo + riscaldatore) con setpoint, profili, sonde e densimetro.

Pilota **attuatori di potenza** — compressore frigorifero e cavo/tappetino riscaldante — su
cotte che durano settimane o mesi.

> La versione a **due camere** (3.0.0) resta disponibile al tag
> [`v3.0.0`](https://github.com/Lupulus21/axhtherm-firmware/tree/v3.0.0). Dalla 3.1.0 il firmware
> gestisce una camera per scheda: per due frighi si usano due schede.

### ⬇️ Scarica il firmware già compilato

I file `.bin` **non sono fra le cartelle qui sopra**: qui c'è il codice sorgente. I binari
pronti stanno nelle **[Releases](https://github.com/Lupulus21/axhtherm-firmware/releases)**.

- **[axhtherm-3.17.3.bin](https://github.com/Lupulus21/axhtherm-firmware/releases/download/v3.17.3/axhtherm-3.17.3.bin)**
- [SHA256SUMS](https://github.com/Lupulus21/axhtherm-firmware/releases/download/v3.17.3/SHA256SUMS) — per verificare il download

**Prima di flasharlo leggi l'avvertenza qui sotto**: il pinout è compilato nel binario.

---

## ⚠️ Leggi questo prima di flashare

**Il pinout è compilato dentro il binario.** Questo firmware presuppone la scheda AxHTherm 2.5
con un WEMOS D1 mini:

| Funzione | Pin |
|---|---|
| REL1 / REL2 / REL3 | GPIO12 / GPIO13 / GPIO15 (diretti, bufferati da ULN2803) |
| REL4–REL8 | via MCP23008 su I²C, indirizzo `0x20` |
| Bus sonde OneWire | GPIO0 (pull-up 4,7 kΩ) |
| I²C (SCL / SDA) | GPIO5 / GPIO4 — OLED `0x3C`, RTC DS1307 `0x68`, EEPROM del Tiny RTC `0x50` |

Se il tuo cablaggio è diverso, **i relè si accenderanno su pin sbagliati**. Su un dispositivo
che comanda un compressore non è un fastidio: è un guasto. Verifica `include/hardware_map.h`
prima di compilare, e **identifica ogni relè col test manuale a carichi scollegati**
(`docs/COLLAUDO.md`, fase A): l'ordine dei morsetti sul connettore RELAY può non essere quello
dello schema.

**I contatti dei relè devono essere NO (normalmente aperti).** Tutta la catena di stato sicuro
— uscite spente come prima istruzione di `setup()`, ULN2803 non invertente, reset dell'MCP,
failsafe hardware — assume che *bobina diseccitata = carico spento*. Con contatti NC ogni
protezione si rovescia e il failsafe accenderebbe gli attuatori invece di spegnerli.

**Prova gli attuatori prima di ogni cotta.** Il firmware rileva molti guasti (vedi sotto), ma
non può aprire un relè con i contatti incollati. Procedura completa in `docs/COLLAUDO.md`.

Il software è fornito così com'è, senza garanzia. Chi lo installa si assume la responsabilità
del proprio impianto elettrico e delle proprie fermentazioni.

---

## Cosa fa

- **Regolazione** a isteresi a doppia soglia, con **PID opzionale sul caldo** (finestra a duty)
  e **spegnimento predittivo** che impara l'inerzia termica del frigo (°C/h).
- **Profili di fermentazione** fino a 12 step, 4 tipi: temperatura fissa, rampa a tempo, rampa a
  velocità (°C/h), **attesa a densità** (soglia, stabilità, % di attenuazione). Per ogni step:
  «parte all'arrivo in temperatura», «senza caldo / senza freddo», **«aspetta la mia
  conferma»** (dry hop, campione, travaso). Libreria di 20 profili, esporta e importa.
- **Cold crash** dalla dashboard, a 3 o a 1,5 °C ogni 6 ore, con stima d'arrivo e allarme se il
  frigo non tiene il passo.
- **Densimetri** (iSpindel, o Tilt/RAPT tramite un ponte) con cattura automatica dell'OG,
  taratura per strumento e **andamento della densità**: attiva, rallenta, ferma. Avvisa a
  **fermentazione finita** e se la fermentazione sembra **ferma sopra l'atteso**.
- **Interfaccia web** completa servita dal dispositivo (compilata nel firmware, non serve rete
  esterna): dashboard, grafici con minimi e massimi, export CSV, editor dei profili, diagnostica
  delle sonde, backup della configurazione.
- **Registro eventi** nella EEPROM del modulo orologio: gli ultimi 256 eventi (avvii,
  armamenti, step, allarmi) restano anche senza corrente.
- **Display OLED** a pagine: stato, fermentazione, elenco degli allarmi.
- **MQTT** con discovery **Home Assistant**: termostato, sensori, allarmi per nome, pulsanti dei
  profili, esito dei comandi.
- **Ventola** della camera su un relè libero: segue caldo e freddo, ricircolo programmabile.
- **OTA** con verifica di lunghezza e CRC del file, e la password attuale obbligatoria.
- Funziona **senza WiFi**: la rete è un servizio accessorio, non una dipendenza del controllo.

## Le protezioni, che sono il motivo per cui esiste

| | |
|---|---|
| Uscite spente all'avvio | Prima istruzione di `setup()`, prima di Serial, I²C e filesystem |
| Uscite DISARMATE alla nascita | L'automazione non pilota nulla finché non confermi il mapping relè→carico |
| Qualunque dubbio → tutto spento | Sonda guasta, setpoint assente, config corrotta, driver KO, non armato |
| Mai caldo e freddo insieme | Interlock strutturale + priorità al freddo, con tempo morto sull'inversione |
| Anti-corto-ciclo compressore | Riposo minimo fra avvii e marcia minima, su epoch **persistiti**: vale attraverso i riavvii |
| Blocco del riscaldatore | Sonda che non sale, caldo acceso troppo a lungo, salita a caldo spento (SSR incollato), freddo inefficace: **si spegne solo il caldo**, con riarmo manuale che sopravvive ai riavvii |
| Limiti di temperatura | Assoluti sul mosto (oltre 42 °C niente caldo, sotto −3 °C niente freddo) e sull'aria della camera |
| Frigo che non si ferma | Allarme se l'aria resta fredda col freddo spento (relè incollato) |
| Clamp del setpoint | `[-1, +40] °C` all'attuazione, NaN-safe: un unico punto che copre ogni sorgente |
| Salti d'orologio | Gli epoch àncora vengono traslati: niente fast-forward del profilo né azzeramento del riposo |
| Failsafe hardware | Timer1 in ISR: se il loop si blocca oltre 60 s, i relè su GPIO diretti vengono spenti |
| Riavvii a catena | Tre blocchi in 10 minuti: MQTT si spegne finché il firmware non regge 30 minuti |

## Scaricare il firmware

Due strade, a seconda di cosa ti serve. **In questa pagina non troverai un file `.bin`**: qui
c'è il codice sorgente, i binari stanno nelle Release.

### A — voglio solo installarlo

Vai in **[Releases](https://github.com/Lupulus21/axhtherm-firmware/releases)** e scarica
`axhtherm-3.17.3.bin` e `SHA256SUMS`. Non serve git, non serve compilare.

Controlla che il file sia arrivato integro:

```bash
shasum -a 256 -c SHA256SUMS                      # macOS/Linux
certutil -hashfile axhtherm-3.17.3.bin SHA256    # Windows
```

Poi si installa da USB con [esptool](https://github.com/espressif/esptool):

```bash
esptool.py --port /dev/cu.usbserial-XXXX write_flash 0x0 axhtherm-3.17.3.bin
```

Senza `erase_flash` la configurazione, lo stato e il registro restano. Se hai già una versione
precedente sul dispositivo, è più comodo l'**OTA** dall'interfaccia web (Sistema →
Aggiornamento firmware). Da una 3.0.0 a due camere la configurazione resta e la seconda camera
viene scartata. Procedura completa, backup compreso, in `docs/FLASH.md`.

### B — voglio leggerlo, modificarlo o compilarlo da me

Serve [PlatformIO](https://platformio.org/) (si installa anche come estensione di VS Code).

```bash
git clone https://github.com/Lupulus21/axhtherm-firmware.git
cd axhtherm-firmware

pio run -e d1_mini        # il firmware
pio test -e native        # 126 test sul motore di controllo, eseguiti sul PC
```

PlatformIO scarica da sé toolchain e librerie: la prima compilazione richiede qualche minuto,
le successive una ventina di secondi. **Il binario esce in `.pio/build/d1_mini/firmware.bin`**
— quella cartella non è nel repository perché è generata.

> **La compilazione è riproducibile.** Compilando il tag `v3.17.3` si ottiene un `.bin`
> **byte-identico** a quello allegato alla release (stesso SHA256): non devi fidarti del binario
> che ti passano, puoi rigenerarlo dal sorgente e confrontarlo.

## Primo avvio

Su una scheda nuova (o dopo un reset di fabbrica) il firmware genera **password casuali**, una
per l'interfaccia e una per la rete di configurazione, e le stampa sul **monitor seriale**
(115200 baud). Si rivedono in ogni momento col comando seriale `password`: il cavo USB vale
come chiave. Con il display montato compaiono anche lì, finché non imposti la rete di casa.
L'aggiornamento via rete resta bloccato finché la password dell'interfaccia è quella di
fabbrica delle versioni vecchie.

Il modello di sicurezza dichiarato è **LAN domestica, HTTP in chiaro**: niente TLS, sessione con
cookie, attese crescenti dopo i tentativi di login sbagliati. Non è un profilo adatto a esporre
il dispositivo su Internet, e non è pensato per esserlo.

## Documentazione

| | |
|---|---|
| `docs/FLASH.md` | Backup, installazione via USB e aggiornamenti |
| `docs/COLLAUDO.md` | Cablaggio e collaudo passo per passo — **leggilo prima di cablare** |
| `docs/ALLARMI.md` | Cosa significa ogni allarme e cosa fare |
| `docs/PROFILI.md` | Come si scrivono i profili di fermentazione |
| `docs/ISPINDEL.md` | Collegare e tarare i densimetri |
| `docs/HOME_ASSISTANT.md` | MQTT, entità e notifiche |
| `docs/ARCHITETTURA.md` | Struttura, principi non negoziabili, loop di controllo, persistenza, API |
| `docs/ANALISI.md` | Analisi iniziale: schema della scheda, manuale e firmware originale |
| `CHANGELOG.md` | Cronaca delle versioni, difetti trovati in campo e loro cura |

Il codice e i commenti sono in italiano.

## Licenza

[MIT](LICENSE). Usalo, modificalo, ridistribuiscilo. Nessuna garanzia: vedi l'avvertenza in
cima a questo file — governa attuatori di potenza, e la responsabilità dell'impianto resta di
chi lo installa.
