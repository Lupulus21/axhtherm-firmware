# AxHTherm 3.0 — controllore di fermentazione a due camere

Firmware per **WEMOS D1 mini (ESP8266)** su scheda **AxHTherm 2.5**, che governa fino a
**2 camere di fermentazione indipendenti**: ciascuna con setpoint, profilo, sonde, uscite
caldo/freddo e densimetro propri.

Pilota **attuatori di potenza** — compressore frigorifero e cavo/tappetino riscaldante — su
cotte che durano settimane o mesi.

---

## ⚠️ Leggi questo prima di flashare

**Il pinout è compilato dentro il binario.** Questo firmware presuppone la scheda AxHTherm 2.5
con un WEMOS D1 mini:

| Funzione | Pin |
|---|---|
| REL1 / REL2 / REL3 | GPIO12 / GPIO13 / GPIO15 (diretti, bufferati da ULN2803) |
| REL4–REL8 | via MCP23008 su I²C, indirizzo `0x20` |
| Bus sonde OneWire | GPIO0 (pull-up 4,7 kΩ) |
| I²C (SCL / SDA) | GPIO5 / GPIO4 |

Se il tuo cablaggio è diverso, **i relè si accenderanno su pin sbagliati**. Su un dispositivo
che comanda un compressore non è un fastidio: è un guasto. Verifica `include/hardware_map.h`
prima di compilare, e collauda **a carichi scollegati**.

**I contatti dei relè devono essere NO (normalmente aperti).** Tutta la catena di stato sicuro
— uscite spente come prima istruzione di `setup()`, ULN2803 non invertente, reset dell'MCP,
failsafe hardware — assume che *bobina diseccitata = carico spento*. Con contatti NC ogni
protezione si rovescia e il failsafe accenderebbe gli attuatori invece di spegnerli.

**Prova gli attuatori prima di ogni cotta.** Un attuatore che non attua (carico scollegato,
relè guasto) è invisibile al controllo: il firmware rileva lo scostamento termico e alza un
allarme dopo 3 ore, ma non può correggerlo. Procedura in `docs/COLLAUDO_2CAMERE.md`, sezione C.

Il software è fornito così com'è, senza garanzia. Chi lo installa si assume la responsabilità
del proprio impianto elettrico e delle proprie fermentazioni.

---

## Cosa fa

- **Regolazione** a isteresi a doppia soglia, con **PID opzionale sul caldo** (finestra a duty)
  e **spegnimento predittivo** che impara l'inerzia termica di ciascun frigo (°C/h).
- **Profili di fermentazione** fino a 12 step, 4 tipi: temperatura fissa, rampa a tempo, rampa
  a velocità (°C/h), e **attesa a densità** (soglia, stabilità, o % di attenuazione).
- **Densimetri iSpindel**, uno per camera, con cattura automatica dell'OG e taratura per
  strumento.
- **Interfaccia web** completa servita dal dispositivo (compilata nel firmware, non serve rete
  esterna), **display OLED**, **MQTT** con discovery Home Assistant, **registro** su LittleFS
  con esportazione CSV, **OTA**.
- Funziona **senza WiFi**: la rete è un servizio accessorio, non una dipendenza del controllo.

## Le protezioni, che sono il motivo per cui esiste

| | |
|---|---|
| Uscite spente all'avvio | Prima istruzione di `setup()`, prima di Serial, I²C e filesystem |
| Uscite DISARMATE alla nascita | L'automazione non pilota nulla finché non confermi il mapping relè→carico |
| Qualunque dubbio → tutto spento | Sonda guasta, setpoint assente, config corrotta, driver KO, non armato |
| Mai caldo e freddo insieme | Interlock strutturale + priorità al freddo, con tempo morto sull'inversione |
| Anti-corto-ciclo compressore | Riposo minimo fra avvii e marcia minima, su epoch **persistiti**: vale attraverso i riavvii |
| Clamp del setpoint | `[-1, +40] °C` all'attuazione, NaN-safe: un unico punto che copre ogni sorgente |
| Salti d'orologio | Gli epoch àncora vengono traslati: niente fast-forward del profilo né azzeramento del riposo |
| Failsafe hardware | Timer1 in ISR: se il loop si blocca oltre 60 s, i relè su GPIO diretti vengono spenti |
| Sfasamento fra camere | Compressori sfasati di 30 s, finestre PID sfasate: niente spunti di corrente simultanei |

## Scaricare il firmware

Due strade, a seconda di cosa ti serve. **In questa pagina non troverai un file `.bin`**: qui
c'è il codice sorgente, i binari stanno nelle Release.

### A — voglio solo installarlo

Vai in **[Releases](https://github.com/Lupulus21/axhtherm-firmware/releases)** e scarica il file
che ti serve. Non serve git, non serve compilare, non serve scaricare le cartelle.

| File | Per |
|---|---|
| `axhtherm-3.0.0-1camera.bin` | Una camera — **prendi questo se non sei sicuro** |
| `axhtherm-3.0.0-2camere.bin` | Due camere: richiede MCP23008, una DS18B20 in più, breakout per P1 |

Controlla che il file sia arrivato integro:

```bash
shasum -a 256 -c SHA256SUMS      # macOS/Linux
certutil -hashfile axhtherm-3.0.0-1camera.bin SHA256   # Windows
```

Poi si installa da USB con [esptool](https://github.com/espressif/esptool):

```bash
esptool.py --port /dev/cu.usbserial-XXXX write_flash 0x0 axhtherm-3.0.0-1camera.bin
```

Se hai già una versione precedente sul dispositivo, è più comodo l'**OTA** dall'interfaccia web
(Sistema → Aggiornamento firmware): niente cavo, e la configurazione resta.

### B — voglio leggerlo, modificarlo o compilarlo da me

Serve [PlatformIO](https://platformio.org/) (si installa anche come estensione di VS Code).

```bash
git clone https://github.com/Lupulus21/axhtherm-firmware.git
cd axhtherm-firmware

pio run -e d1_mini        # una camera
pio run -e d1_mini_2ch    # due camere
pio test -e native        # 85 test sul motore di controllo, eseguiti sul PC
```

PlatformIO scarica da sé toolchain e librerie: la prima compilazione richiede qualche minuto,
le successive una ventina di secondi. **Il binario esce in
`.pio/build/<environment>/firmware.bin`** — quella cartella non è nel repository perché è
generata, e cambia a ogni compilazione.

> **La compilazione è riproducibile.** Il `.bin` che ottieni clonando e compilando è
> **byte-identico** a quello allegato alla release (verificato: stesso SHA256). Non devi
> fidarti del binario che ti passano: puoi rigenerarlo dal sorgente e confrontarlo.

Il numero di camere è un **flag di compilazione** (`AX_N_CHAMBERS`): dimensiona array statici,
e renderlo dinamico significherebbe allocare su uno heap già sottile (~14 KB liberi).

**Per due camere serve hardware in più**: l'MCP23008 nel footprint DIP-18 (per la 4ª uscita),
una sonda DS18B20 e un breakout per il connettore P1. Dettagli in `docs/COLLAUDO_2CAMERE.md`.
Senza, la seconda camera degrada in sicurezza: non attua e segnala l'allarme, **senza toccare
la prima**.

## Primo avvio

Il dispositivo parte in SoftAP. Password di default `axhtherm` (utente `admin`) — **l'OTA resta
disabilitato finché non la cambi**, di proposito.

Il modello di sicurezza dichiarato è **LAN domestica, HTTP in chiaro**: niente TLS, cookie di
sessione statico, nessun blocco dopo tentativi falliti. Non è un profilo adatto a esporre il
dispositivo su Internet, e non è pensato per esserlo.

## Documentazione

| | |
|---|---|
| `docs/ARCHITETTURA.md` | Struttura, principi non negoziabili, loop di controllo, persistenza |
| `docs/COLLAUDO_2CAMERE.md` | Cablaggio e collaudo passo per passo — **leggilo prima di cablare** |
| `docs/PROFILI.md` | Come si scrivono i profili di fermentazione |
| `docs/ISPINDEL.md` | Collegare i densimetri |
| `docs/HOME_ASSISTANT.md` | MQTT e discovery |
| `docs/analysis/` | Analisi forense, studio di fattibilità a 2 camere, audit di sicurezza |
| `CHANGELOG.md` | Cronaca delle versioni, difetti trovati in campo e loro cura |

Il codice e i commenti sono in italiano.

## Licenza

[MIT](LICENSE). Usalo, modificalo, ridistribuiscilo. Nessuna garanzia: vedi l'avvertenza in
cima a questo file — governa attuatori di potenza, e la responsabilità dell'impianto resta di
chi lo installa.
