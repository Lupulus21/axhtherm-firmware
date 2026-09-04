# Due camere — configurazione, cablaggio e collaudo

Procedura per passare da una a due camere di fermentazione. Leggila **tutta prima di
flashare**: due punti non sono reversibili.

---

## ⚠️ Due avvertimenti non reversibili

1. **Il formato di `/state.json` cambia.** Il firmware nuovo lo riscrive in schema v2 al primo
   salvataggio. **Tornare al firmware precedente fa perdere lo stato**: il vecchio codice non
   riconosce lo schema v2 e ripartirebbe Off/disarmato. Se vuoi poter tornare indietro, salva
   prima una copia della LittleFS (procedura in `docs/FLASH.md`).
2. **Prima di qualunque flash, controlla che non ci sia una cotta in corso**:
   `GET /api/status` → `mode` (2 = profilo) e `profile.running`. Un flash riavvia il
   controllore e fa cadere i relè.

---

## 1. Che cosa serve (hardware)

Il fabbisogno è **4 uscite relè**; oggi ne esistono fisicamente **3**.

| # | Componente | Perché |
|---|---|---|
| 1 | **MCP23008-E/P (DIP-18)** | **Indispensabile.** Sull'esemplare non è montato (verificato al banco 15/08/2026). Senza, REL4-REL8 non esistono: gli ingressi `I4-I8` dell'ULN2803 sono cablati ai suoi `GP0-GP4` e non sono raggiungibili da nessun altro header. Consigliato uno zoccolo, per un montaggio non distruttivo |
| 2 | **Breakout per P1** (femmina 2,54 mm → morsettiera) | REL4-REL8 escono **solo su un pettine a 2,54 mm**, non su morsettiera: non è un punto di attestazione per cavi di potenza |
| 3 | 1 canale relè esterno | I relè sono tutti esterni alla scheda |
| 4 | 1÷3 **DS18B20** | Sonda di controllo della camera 2 (+ ausiliarie) |
| 5 | Elettrolitico **470-1000 µF / 10 V** | Sulla 5 V non c'è alcun serbatoio: `C1` è **47 pF**, l'unico condensatore della scheda |
| 6 | ~~Alimentatore 5 V ≥ 2 A~~ | ✅ **Già a posto**: l'alimentatore in uso dichiara ≥ 2 A (verificato 2026-09-02). Nessun acquisto |
| 7 | Moduli relè **opto-isolati con `JD-VCC` separato** | **Sceglierli così.** Toglie 280-360 mA di bobine dal rail dell'MCU e con essi i transitori, e **elimina il rischio R3**: oggi il comune dei diodi `CD` è una rotaia unica condivisa dalle bobine di *entrambe* le camere |

### Margine dell'alimentatore

ESP8266 in TX ~350 mA + OLED ~20 mA + I²C ~5 mA + **4 bobine relè 280-360 mA** ≈ **700-740 mA**
nel caso peggiore. **✅ Risolto**: l'alimentatore in uso dichiara **≥ 2 A** (verificato
2026-09-02) — margine oltre il doppio, qualunque tipo di moduli si scelga.

Resta valida la raccomandazione sui moduli **opto-isolati con `JD-VCC` separato**: non serve
più per il consumo medio, ma toglie i transitori di bobina dal rail dell'MCU (unico
condensatore a bordo: `C1 = 47 pF`) e separa elettricamente le due camere.

> Lo sfasamento delle finestre PID riduce la sovrapposizione dei riscaldanti ma **non la
> elimina**: a duty > ~96,7 % la fase è ignorata per costruzione. Dimensiona per i carichi
> contemporanei.

---

## 2. Cablaggio

**Allocazione raccomandata** — tiene *entrambi i compressori* su GPIO diretti:

| | Freddo | Caldo |
|---|---|---|
| **Camera 1** | REL3 (GPIO15) — *già cablato al frigo* | REL1 (GPIO12) — *già cablato al riscaldante* |
| **Camera 2** | **REL2** (GPIO13, oggi libero) | **REL4** (via MCP23008, su P1) |

Il razionale è di sicurezza, e vale la pena capirlo perché **non è simmetrico**: se l'I²C cade,
la camera 2 perde **il caldo, non il freddo**. Una birra non riscaldata rallenta; una non
raffreddata si rovina.

Due meccanismi coprono **solo i GPIO diretti** e non l'espansore:
- `safeInitEarly()`, che spegne le uscite come primissima istruzione all'avvio;
- il **failsafe su timer hardware**, che interviene se il loop di controllo si blocca — l'I²C
  non è comandabile da un'ISR.

Concretamente: se il loop si congela, il riscaldante della camera 2 su REL4 **resta acceso**
fino a quando il loop riparte (poi il recupero lo spegne). È il limite noto di questa
allocazione, ed è la ragione per cui **entrambi i compressori** stanno su GPIO diretti: sono
loro il carico che, restando acceso, fa il danno peggiore.

> 🔴 **La numerazione dei pin di P1 in `docs/ANALISI.md` è SBAGLIATA** rispetto al layout
> pubblicato. **Fidati solo dei nomi serigrafati**, mai dei numeri.
>
> 🔴 `PDF rev 1.0` e layout pubblicato **divergono** su `POWER` (2 o 3 poli) e sul jumper `UPWR`.
> Guarda la tua scheda prima di cablare: da lì dipende dove si preleva il comune bobine `CD`.

Le sonde della camera 2 vanno **in parallelo** sullo stesso morsetto PROBE (bus OneWire unico).
Con due frigo distanti, il cablaggio a stella su lunghe distanze è la causa classica dei -127.

---

## 3. Compilazione e flash

```bash
~/.venvs/platformio/bin/pio run  -e d1_mini_2ch                      # due camere
~/.venvs/platformio/bin/pio test -e native                           # atteso 81/81
~/.venvs/platformio/bin/pio run  -e d1_mini_2ch -t upload --upload-port /dev/cu.usbserial-11340
```

`AX_N_CHAMBERS` è un **flag di compilazione**, non un'impostazione runtime: dimensiona array
statici, e renderlo dinamico significherebbe allocare su uno heap di ~17 KB — peggiorando
proprio il vincolo che il progetto sorveglia. `-e d1_mini` resta la build a una camera.

---

## 4. Collaudo, in quest'ordine

**A — prima del cablaggio di potenza, a carichi staccati**

1. `scan i2c` da seriale → deve comparire **`0x20`** (il MCP montato). Se non c'è, fermati.
2. `scan ow` → devono comparire gli ID di **tutte** le sonde, incluse le nuove.
3. Impostazioni → Sonde: assegna gli ID. Le sonde 1-3 sono della camera 1, le 4-6 della
   camera 2 (il pannello lo indica). **La stessa sonda non può stare in due slot**: il
   firmware rifiuta il salvataggio.
4. `rele 1 1` … `rele 4 1` → verifica il click di ciascun canale, **incluso REL4** (che passa
   dal MCP). Poi `test off`.

**B — armamento, una camera per volta**

5. `arma 1 3 1` (camera 1: freddo REL3, caldo REL1) — la forma storica `arma 3 1` vale ancora.
6. `arma 2 2 4` (camera 2: freddo REL2, caldo REL4).
   Il firmware **rifiuta** se un relè è già assegnato all'altra camera.
7. Nell'interfaccia deve comparire il **selettore di camera** in alto. Cambiando camera,
   dashboard, andamento e profilo devono seguire; la **libreria profili resta condivisa**.

**C — verifiche di sicurezza (le più importanti)**

8. **Gli attuatori scaldano e raffreddano davvero.** A carichi collegati e camera **disarmata**,
   per ciascuna camera: `rele <caldo> 1`, attendi 15 minuti, verifica che la temperatura **salga**
   di almeno 0,5 °C; poi `rele <freddo> 1`, attendi 20 minuti, verifica che **scenda**. `test off`
   fra una prova e l'altra.

   > **Perché questa prova esiste.** Il 25-26/08/2026 il registro mostra 14 ore di comando caldo
   > continuo con setpoint 35 °C e la temperatura **in discesa** da 29,2 a 24,8 °C: il cavo
   > riscaldante non era collegato (prova a banco). Il firmware ha fatto tutto il possibile —
   > allarme di scostamento termico dopo tre ore — ma **un attuatore che non attua è invisibile
   > al controllo finché non è troppo tardi**. Con una birra dentro, quattordici ore a 25 °C
   > invece di 35 sono una fermentazione Kveik andata storta. Questa prova costa 35 minuti e va
   > fatta **prima di ogni cotta**, non solo al primo collaudo.

9. **Guasto sonda isolato**: scollega la sonda di controllo della camera 2 → dopo ~15 s la sua
   uscita cade e compare l'allarme. **La camera 1 deve continuare a lavorare.**
10. **Guasto I²C isolato**: sfila il MCP23008 (a dispositivo spento) e riaccendi → la camera 2
   alza `uscite (MCP) KO` e non attua; **la camera 1 continua**.
11. **Disarmo**: `disarma 2` ferma solo la camera 2. `disarma` (senza argomenti) e il pulsante
    dell'interfaccia fermano **tutto**, compresi i canali non mappati.
12. **Riavvio a caldo**: con entrambe le camere in marcia, `riavvia`. Al ritorno entrambe
    devono riprendere profilo, step e **riposo compressore**; i due compressori non devono
    ripartire nello stesso istante (sfasamento di 30 s per camera).

**D — numero da annotare**

13. `heap` da seriale, o `freeHeap` in `/api/status`. Atteso **≥ 10 KB**. Sotto quella soglia,
    il piano prevede di ripiegare su due controllori separati invece di forzare.

---

## 5. Se qualcosa va storto

| Sintomo | Causa probabile |
|---|---|
| `rele 4` risponde errore | MCP23008 non montato o non a 0x20 (`scan i2c`) |
| Camera 2 sempre in stato sicuro | Sonda di controllo non assegnata, o non armata, o `uscite (MCP) KO` |
| Allarme `scostamento T` dopo 3 h, uscita comandata ma T ferma o in deriva | **Attuatore che non attua**: carico scollegato, modulo relè o canale ULN guasto. Il firmware può solo segnalarlo — esegui la prova 8 |
| `-127` intermittenti | Bus OneWire troppo lungo o a stella; pull-up unica da 4,7 kΩ |
| Salvataggio config rifiutato | Stessa sonda in due slot, o etichetta troppo lunga (max 24) |
| Allarme `SCRITTURA FLASH KO` al boot | `/state.json` illeggibile, oppure descriveva più camere di quante ne gestisce il firmware (stato perso: è segnalato apposta) |
