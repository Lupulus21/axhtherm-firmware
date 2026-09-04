# FLASH.md — Installazione del firmware su macOS

Procedura completa: backup OBBLIGATORIO della flash originale, cancellazione, scrittura,
ripristino in caso di ripensamento. Tutto da terminale macOS.

## 0. Prerequisiti

- WEMOS D1 mini collegato via USB. Il convertitore è un CH340: macOS recenti hanno il
  driver di serie; la porta appare come `/dev/cu.usbserial-XXXX` o `/dev/cu.wchusbserialXXXX`.
- **ATTENZIONE**: durante flash e primi collaudi i relè devono essere **scollegati dai carichi**
  (frigo e cavo riscaldante staccati dalle uscite di potenza).

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

```sh
cd ~/dev/axhtherm25
mkdir -p backups
$ESPTOOL --port $PORT --baud 460800 read_flash 0x0 0x400000 backups/axhtherm_originale_4MB.bin
shasum -a 256 backups/axhtherm_originale_4MB.bin | tee backups/axhtherm_originale_4MB.sha256
```

Se il baud 460800 dà errori, riprova con `--baud 115200` (più lento ma affidabile).
Il file deve risultare esattamente da **4 194 304 byte**: verifica con `ls -l backups/`.

### Ripristino del firmware originale (in qualunque momento futuro)

```sh
$ESPTOOL --port $PORT --baud 460800 erase_flash
$ESPTOOL --port $PORT --baud 460800 write_flash 0x0 backups/axhtherm_originale_4MB.bin
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

Checklist passo-passo in `docs/COLLAUDO.md`. In sintesi: prima tutto **senza carichi**
(scan bus, sonde, test relè a vuoto sentendo il click, mapping, ARMA), poi i carichi reali.

## 6. Aggiornamenti successivi

Dalla web UI: **Sistema → Aggiornamento firmware (OTA)** caricando
`.pio/build/d1_mini/firmware.bin` (protetto dalla password UI). Configurazione, profilo,
stato e log su LittleFS NON vengono toccati dagli aggiornamenti.
