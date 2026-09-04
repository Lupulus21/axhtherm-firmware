# PCB AxHTherm 2V5 — file di fabbricazione e fonti

Raccolti il 18/08/2026 con una ricerca approfondita. La PCB **non è in vendita da nessuna parte**
(non lo è mai stata: veniva distribuita in group-buy nel gruppo Telegram; ioBrewing è defunto),
ma il progetto è **open source (GPL 3.0)** pubblicato dagli autori stessi, quindi è legale e
semplice **farsela produrre**.

## File in questa cartella

| File | Cos'è |
|---|---|
| `Gerber_AxHTherm_2V5.zip` | **Set Gerber completo e pronto da ordinare** (2 layer, 93,98×58,42 mm, export EasyEDA 13/06/2021). Scaricato da un Google Drive pubblico, archivio verificato: 10 file (rame top/bottom, silk, mask, outline, fori PTH/NPTH). |
| `EasyEDA_schematic_AxHTherm2V5.json` | Sorgente EasyEDA dello schema (backup, scaricato via API) |
| `EasyEDA_pcb_AxHTherm2V5.json` | Sorgente EasyEDA del layout PCB (backup; doc aggiornato al 2024-02-12) |
| `BOM_AxHTherm_2V5.csv` | **BOM ufficiale degli autori** (17 righe, 22 componenti), estratta dal progetto EasyEDA; include i codici LCSC dove pubblicati (ULN2803APG=C5252, LED 5mm=C52721, header 4p=C124413) |
| `CPL_AxHTherm_2V5.csv` | **Pick & place** (22 footprint): designatore, package, coordinate in mm dall'angolo in basso a sinistra, lato, rotazione. Generato dal layout EasyEDA; sanità verificata (outline ricavato = 93,98×58,42 mm esatti). Nota: LED1-3, OLED e STD_I2C risultano sul lato BOTTOM nel progetto (montaggio dal retro); confronta con la tua scheda fisica. Rotazioni in convenzione EasyEDA: per assemblaggio JLCPCB potrebbero servire aggiustamenti — per una scheda tutta THT il file è comunque solo informativo. |
| `serigrafia_render.png` | Render della serigrafia del Gerber (verifica visiva: "AxHTherm v3 – PCB by rekhyt © – license GPL v3") |
| `pcb_anteprima_oshwlab.png` | Anteprima del PCB sbrocciato dal progetto OSHWLab |

## Come ordinare la PCB

1. Vai su **jlcpcb.com** (o pcbway.com, o qualunque produttore) → "Order now / Quote".
2. Carica `Gerber_AxHTherm_2V5.zip` così com'è.
3. Parametri: 2 layer, FR-4, dimensioni rilevate automaticamente (94×58,4 mm), tutto default.
4. Costo tipico: **~2–5 $ per 5 pezzi** + spedizione. Componenti (BOM) a parte: ~12–20 €
   (WEMOS D1 mini, ULN2803APG DIP-18, MCP23008-E/P DIP-18, modulo Tiny RTC, HLK-PM01,
   connettori H3-5.0, 3 LED 5 mm, R 4,7k/1k — tutto through-hole, facile da saldare).

## Fonti ufficiali (verificate vive al 18/08/2026)

- Progetto OSHWLab dell'autore: https://oshwlab.com/rekhyt/axhtherm2-5v (schema + PCB, GPL 3.0)
- Apertura diretta nell'editor EasyEDA: https://easyeda.com/editor#id=09f49661168f41d29f0c7bae9fb9db89|51c55702fd3e4bc7949905ef7e7a5198
- Gerber su Google Drive (fonte dello zip qui salvato): https://drive.google.com/file/d/1Fp5DLNvQyg8P5gniin-QDVvHDetCxSCF/view
- Versione precedente (PCB v4, 2018): https://oshwlab.com/rekhyt/BSTherm-663e612209294a9bad68ba3063270f36
- Pagina Hackaday: https://hackaday.io/project/161813-axhtherm
- Community Telegram (unico canale per chiedere eventuali scorte residue): https://t.me/axhtherm (242 iscritti) e @axhitaly2016

Autori: Carmine Moleti, Michele "Rekhyt" Antonecchia, Stefano "essetielle" Longo (team AxH / ioBrewing).

## Avvertenza sulla revisione

La scheda fisica in possesso è la **rev 1.0 del 2018-10-12**; il Gerber e il layout pubblicati
hanno serigrafia "**AxHTherm v3**" (revisione successiva, stessa BOM e stesso schema di connettori).
Prima di un eventuale ordine conviene un confronto visivo col PDF `Schematics_AxHTherm2-5V.pdf`
nella cartella madre — a vista i connettori (RELAY REL1-3, PROBE, POWER, P1 a 20 pin, I2C/OLED,
SERIAL/BT) coincidono.
