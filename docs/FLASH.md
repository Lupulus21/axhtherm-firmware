# FLASH.md — Installazione del firmware su macOS

Procedura completa: backup OBBLIGATORIO della flash originale, cancellazione, scrittura,
ripristino in caso di ripensamento. Tutto da terminale macOS.

## 0. Prerequisiti

- WEMOS D1 mini collegato via USB. Il convertitore è un CH340: macOS recenti hanno il
  driver di serie; la porta appare come `/dev/cu.usbserial-XXXX` o `/dev/cu.wchusbserialXXXX`.
- **ATTENZIONE**: durante flash e primi collaudi i relè devono essere **scollegati dai carichi**
  (frigo e cavo riscaldante staccati dalle uscite di potenza).
- **Prima di collegare il riscaldatore** leggi la sezione di sicurezza in testa a
  `docs/COLLAUDO.md`.

```sh
# 1) esptool in un venv dedicato (una tantum)
python3 -m venv ~/.venvs/esptool
~/.venvs/esptool/bin/pip install -q esptool
ESPTOOL=~/.venvs/esptool/bin/esptool.py

# 2) trova la porta (scollega/ricollega l'USB e guarda cosa appare)
ls /dev/cu.*
PORT=/dev/cu.usbserial-XXXX   # <-- adatta
```

## 1. Verifica della flash reale (assunzione A5)

```sh
$ESPTOOL --port $PORT flash_id
```

Atteso: `Detected flash size: 4MB`. Se diverso da 4 MB **fermati e dimmelo**: il layout
`4M2M` del progetto non andrebbe bene.

## 2. Backup INTEGRALE della flash originale — OBBLIGATORIO prima di tutto

Salva firmware originale + configurazione SPIFFS in un unico file. Tienilo per sempre.

I backup **non** vanno nel repository (`backups/` è in `.gitignore`): contengono la password
del WiFi. Tienili in una cartella tua, fuori dal repository.

```sh
BK="$HOME/axhtherm-backups"; mkdir -p "$BK"
cd "$BK"
$ESPTOOL --port $PORT --baud 460800 read_flash 0x0 0x400000 axhtherm_originale_4MB.bin
shasum -a 256 axhtherm_originale_4MB.bin | tee axhtherm_originale_4MB.sha256
```

Per una scheda nuova (WEMOS mai usato) dai al file un nome che la identifichi, per esempio
`wemos_<chipId>_4MB.bin`: il backup serve a tornare allo stato di fabbrica del modulo.

Se il baud 460800 dà errori, riprova con `--baud 115200` (più lento ma affidabile).
Il file deve risultare esattamente da **4 194 304 byte**: verifica con `ls -l "$BK"`.

Se anche a 115200 compare `Corrupt data, expected 0x1000 bytes but received 0xff…` il
collegamento USB perde byte (cavo, hub, porta): prova un altro cavo o una porta diretta del Mac.
In alternativa leggi a pezzi e unisci; ogni pezzo è verificato da esptool, e si ritentano solo
quelli falliti (così è stato fatto il backup della scheda di banco il 28/10/2026):

```sh
for i in $(seq 0 63); do f=$(printf "p%02d.bin" $i)
  until [ -s $f ]; do $ESPTOOL --port $PORT --baud 115200 read_flash $((i*0x10000)) 0x10000 $f \
    >/dev/null 2>&1 || rm -f $f; done; done
cat p*.bin > backup_4MB.bin && rm p*.bin && ls -l backup_4MB.bin   # 4194304 byte
```

Per **aggiornare** una scheda già configurata non serve cancellare la flash: si scrive solo il
firmware (passo 4) e configurazione, stato e registro restano.

### Ripristino del firmware originale (in qualunque momento futuro)

```sh
$ESPTOOL --port $PORT --baud 460800 erase_flash
$ESPTOOL --port $PORT --baud 460800 write_flash 0x0 "$BK/axhtherm_originale_4MB.bin"
```

Riporta il dispositivo esattamente com'era (incluse le configurazioni salvate su SPIFFS).

## 3. Cancellazione totale della flash

```sh
$ESPTOOL --port $PORT erase_flash
```

## 4. Scrittura del nuovo firmware

Via PlatformIO (consigliato: compila, comprime la UI e flasha in un colpo):

```sh
~/.venvs/platformio/bin/pio run -d ~/dev/axhtherm25 -t upload --upload-port $PORT
```

Oppure con esptool sul binario già compilato:

```sh
$ESPTOOL --port $PORT --baud 460800 write_flash 0x0 ~/dev/axhtherm25/.pio/build/d1_mini/firmware.bin
```

Non serve nessuna immagine filesystem: la web UI è dentro il firmware (gzip in PROGMEM)
e LittleFS si formatta da solo al primo avvio (area dati 2 MB, vuota).

## 5. Primo avvio e collaudo

```sh
~/.venvs/platformio/bin/pio device monitor -p $PORT -b 115200
```

Su una scheda nuova (o dopo un reset di fabbrica) il primo avvio scrive qui le **credenziali
della scheda**: nome e password della rete di configurazione, password dell'interfaccia. Sono
casuali e diverse per ogni scheda: annotale. Una scheda già configurata, aggiornata, tiene le sue.

> Dopo `pio run -t upload` la scheda si avvia **prima** che il monitor si apra: il riquadro
> delle credenziali di solito non si vede. Nessun problema: **scrivi `password` e invio**, e
> le credenziali ricompaiono. `password nuova` ne genera altre; `reset fabbrica` (con conferma)
> riporta la scheda come nuova, ultima risorsa se non si raggiunge più dalla rete.

Due casi in cui la scheda torna a valori "di fabbrica" senza che tu lo chieda:
- **configurazione illeggibile al boot**: per sicurezza si usano i default pubblici (`axhtherm`,
  rete `axhtherm-setup` con password `axhtherm25`) con le uscite disabilitate, finché non la
  correggi; `password` mostra sempre quelli in uso;
- **filesystem riformattato** (LittleFS non montabile all'avvio): la scheda si comporta da nuova,
  con nome e password nuovi da leggere qui.

Checklist passo-passo in `docs/COLLAUDO.md`. In sintesi: prima tutto **senza carichi**
(scan bus, sonde, test relè a vuoto sentendo il click, mapping, ARMA), poi i carichi reali.

## 6. Aggiornamenti successivi

Dalla web UI: **Sistema → Aggiornamento firmware (OTA)** caricando
`.pio/build/d1_mini/firmware.bin`: serve la password attuale dell'interfaccia, nel campo accanto
al pulsante (ed è disattivato finché resta la password di fabbrica `axhtherm` delle schede
configurate prima della 3.4.0). Configurazione, profilo,
stato e log su LittleFS NON vengono toccati dagli aggiornamenti.

Dalla 3.7.0 la scheda verifica la lunghezza e il CRC che il firmware porta al suo interno e
conferma l'aggiornamento solo se il file è intero e integro: un file danneggiato o scaricato a
metà viene rifiutato e resta il firmware attuale. Si carica `firmware.bin`, non `.bin.gz`. La
pagina mostra l'avanzamento dell'invio e si ricarica da sola quando la scheda torna. Da riga di
comando:

```sh
curl -u admin:PASSWORD -H "X-Ax-Password: PASSWORD" \
     -F "firmware=@.pio/build/d1_mini/firmware.bin" http://axhtherm-XXXX.local/api/update
```
