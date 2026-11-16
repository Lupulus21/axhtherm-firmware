# PROFILI.md — Libreria profili di fermentazione (v2.18)

**Aggiornamento v2.18.0 (23/08/2026).** I modelli sono passati da 13 a **49** e non
stanno piu' in una galleria separata: sono nello STESSO menu della libreria, dopo il
gruppo "I miei profili", divisi per famiglia (ale britanniche e americane, lager,
belgi/weizen/saison, kveik e lieviti moderni, acide e selvatiche, condizionamento).
Un modello si apre in **sola lettura**; da li' due strade:
 - **Usa modello** ne salva una copia in libreria com'e', pronta da avviare;
 - **Modifica...** sblocca l'editor e obbliga a un **nome nuovo**, cosi' l'originale
   resta sempre disponibile come riferimento.
I 46 modelli nuovi vengono da una ricerca con fonti verificate (schede tecniche
Fermentis/Lallemand/Wyeast/White Labs/Omega/Escarpment, BYO, Brulosophy, Janish).

Due regole imparate scrivendo questi modelli, che valgono per QUALSIASI profilo:
 1. uno step **ramp_rate non ha timeout**: avanza solo quando la temperatura misurata
    entra nella tolleranza. Per le discese verso il lagering si usa **ramp_time**
    (che finisce comunque), altrimenti un frigo che non arriva a 0 °C blocca il
    profilo per sempre;
 2. sotto i 4 °C il riscaldamento va **disabilitato** sullo step, o combatte col frigo.

Riferimento delle condizioni di avanzamento del motore. I numeri vengono da una ricerca sulle fonti primarie
(agosto 2026): Brulosophy (metodi + exBEERiment), BYO, Wyeast/White Labs,
White/Zainasheff *Yeast*, Palmer *How to Brew*, Fermentis/Lallemand/Omega,
documentazione BrewPiLess/Brewfather/RAPT. **Sono punti di partenza**: la galleria
carica il modello nell'editor e ogni numero va adattato a lievito, OG e gusto.

## Condizioni di avanzamento degli step (motore v2.12)

| Tipo | Avanza quando |
|---|---|
| Temperatura fissa | tempo trascorso ≥ durata |
| Rampa a tempo / °C/h | come in v2.6 (invariati) |
| Attesa densità — **soglia** | densità ≤ soglia (SG o °P) |
| Attesa densità — **% attenuazione** | attenuazione apparente ≥ target % (serve l'OG, catturata automaticamente o impostata a mano; senza OG lo step attende e vale solo il timeout) |
| Attesa densità — **stabile** | densità ferma entro ±banda per N ore consecutive (finestra che riparte quando la densità esce dalla banda; misurata dentro lo step, quindi fa anche da tempo minimo anti-lag; sopravvive ai riavvii). Dalla 3.16.0 la finestra riparte solo con **due letture di fila** fuori banda: un valore isolato, tipico del rumore di un iSpindel, non fa più ricominciare le 48 ore |

Ogni attesa a densità ha un **timeout di sicurezza** (giorni; 0 = nessuno): un
densimetro fermo, incrostato di krausen o scarico non blocca mai il profilo
(caso reale documentato: iSpindel a 1.000 con idrometro a 1.012). Consiglio:
timeout ≈ 1,5–2× la durata attesa dello step.

Ogni step ha la **politica di attuazione**: "senza caldo" (free-rise: la
temperatura sale da sola, il frigo fa solo da tetto) e/o "senza freddo" (mai
raffreddare, es. salita saison dove una discesa rischia lo stallo).

**Freeze-guard**: setpoint minimo -1 °C (una birra al 5% congela a ~-3 °C;
sotto -1 l'overshoot del frigo diventa un rischio).

**Default consolidati**: stabilità 1 punto (0.001 SG) @ 48 h (kveik: 2@24;
saison: 1@72; cap motore 168 h); trigger del rialzo/diacetyl rest ≈ 50 % di
attenuazione apparente (convergenza di White 2–5 punti dal FG, Wyeast 40–50 %,
Brulosophy 50 %); crash standard -0,3 °C/h (Brulosophy: crash immediato e rampa
sono sensorialmente indistinguibili, p=0.89 — la rampa serve contro il suck-back
nei fermentatori non pressurizzati); lager tradizionale -0,1 °C/h (entro il
limite Noonan di 3 °C/giorno).

## I 13 modelli

1. **Ale pulita (US-05)** — 18° fino a 50 % att. (timeout 5 g) → rampa 0,08 °C/h
   a 21° → 21° fino a stabile 1@48 (timeout 7 g) → crash -0,3 °C/h a 2° → 2°×2 g.
2. **NEIPA dry-hop** — 19°×36 h (a fine step: dry hop #1, biotrasformazione) →
   19° fino a 66 % att. → rampa a 21° → stabile 1@48 (dry hop #2) → *secondo*
   stabile 1@72, timeout 9 g (hop creep: il dry hop riattiva la fermentazione) →
   crash → 2°×2 g. Le notifiche ai cambi step si fanno in Home Assistant (sensore "step").
3. **Lager classica + diacetyl rest** — 10° fino a 50 % att. (timeout 14 g) →
   rampa 0,25 °C/h a 17° (rest) → stabile 1@48 → discesa lenta 0,1 °C/h a 1° →
   lagering 1°×21 g (regola Noonan: 7–12 g ogni 2 °P di OG).
4. **Lager rapida (Brulosophy)** — 11° fino a 50 % att. → +0,23 °C/h a 19° →
   stabile 1@48 → -0,3 °C/h a 0° → 0°×4 g. Grain-to-glass 3–4 settimane.
5. **Lager Narziss a gradini** — 10°×3 g **senza caldo** (pitch freddo a 7–9°,
   salita libera) → 13°×3 g → 16° fino a stabile → -0,042 °C/h (1 °C/g) a 2° →
   1°×21 g. Senza rest esplicito, alla tedesca.
6. **W-34/70 calda (ibrida)** — 15° fino a 75 % att. → 18° → stabile → crash →
   2°×6 g. Difendibile fino a ~21° (exBEERiment p=0.083), non oltre.
7. **Weizen garofano** — 17° fino a 70 % att. → 19° → stabile → crash breve
   2°×36 h. Variante banana: +3–4 °C (con WB-06 tutto +3 °C).
8. **Saison free-rise** — 20°×2 g → rampa 1 °C/g a 27° **senza freddo** →
   27° **senza freddo** fino a stabile 1@72 (timeout 14 g; mai oltre 29°) →
   crash → 2°×2 g. Una volta iniziata la salita non deve mai scendere.
9. **Kveik Voss express** — 38°×24 h (anti-lag: la stabilità non deve scattare
   prima che la fermentazione parta) → 38° fino a stabile 2@24 (timeout 4 g) →
   -0,5 °C/h a 2° → 2°×2 g. Range LalBrew Voss 25–40°, ottimale 35–40.
10. **Pseudo-lager Lutra (32°)** — 32°×24 h (anti-lag) → 32° fino a stabile 1@24 →
    crash a 0° → 0°×3 g.
11. **Belga forte / Tripel** — 20°×36 h → rampa a 26° **senza freddo** → stabile
    (timeout 10 g) → discesa a 20° → conditioning caldo 20°×4 g → crash → 2°×3 g.
12. **Kettle sour (Lacto 35°)** — 35°×24 h → 35°×24 h (assaggio/pH 3,2–3,5 tra i
    due step; poi si ferma il profilo e si bolle). Il Lacto non muove la SG:
    niente trigger a densità.
13. **Rifermentazione in bottiglia** — 21°×14 g, solo tempo.

## Novità della 3.16.0

- **Aspetta la mia conferma** (casella «fine step» su ogni step): quando lo step finisce il
  profilo non avanza da solo, tiene il setpoint di fine step e aspetta. In dashboard compare
  «Conferma e prosegui»; si conferma anche dal pulsante «Conferma step» di Home Assistant o con
  il comando seriale `conferma`. «Salta step» vale come conferma. L'attesa sopravvive ai
  riavvii. Serve per i passaggi a mano: dry hop, campione, travaso, aggiunta di frutta.
  Esempio: «19° fino a 66 % att. ✋conferma» → aggiungi il luppolo, poi conferma e parte il
  passo successivo.
- **Fermentazione finita / ferma?**: la scheda calcola l'andamento della densità dalle medie
  orarie del densimetro. Con la densità ferma da 48 ore (meno di 0,001 SG, circa 0,25 °P) e
  attenuazione almeno del 50% segnala «fermentazione finita»; se invece è ferma sopra l'atteso
  (attenuazione sotto il 50%, o uno step a densità con la soglia non ancora raggiunta)
  segnala «fermentazione ferma?». Sono avvisi: non cambiano la regolazione né fanno avanzare
  il profilo.

## Novità della 3.12.0

- **Attendi il target** (casella «parte all'arrivo in temperatura» sugli step a temperatura
  fissa): il tempo dello step parte quando il mosto entra nella tolleranza del target, non
  quando lo step comincia. «18 °C × 3 giorni» partendo da un mosto a 22 °C vuol dire 3 giorni
  a 18, non 3 giorni meno il tempo della discesa. Se in 24 ore il mosto non ci arriva, il tempo
  parte comunque e la dashboard lo segnala («STEP PARTITO SENZA IL TARGET»): un frigo che non ce
  la fa non deve bloccare il profilo per sempre. Con un setpoint manuale attivo l'arrivo si misura
  su quello; un blackout lungo durante l'attesa la fa ripartire da capo al ritorno. Per gli step a rampa resta la rampa °C/h, che
  già aspetta il target.
- **Ripresa dopo un blackout lungo**: se uno step è scaduto mentre la scheda era spenta (da più
  di 10 minuti), al ritorno si avanza di **uno** step solo, e quello nuovo parte da quel momento.
  Prima si avanzavano tutti gli step scaduti, ciascuno con inizio nel passato: una sosta mai
  avvenuta, con il controllo spento, risultava fatta. Le interruzioni brevi (riavvio,
  aggiornamento) non spostano gli step.
- **Export con l'unità di densità**: il file di un profilo porta `unit` (`sg` o `plato`);
  all'import su una scheda con l'altra unità le soglie «densità sotto» e le bande di stabilità
  si convertono. Un file senza unità (versioni precedenti) si importa com'è, con un avviso.
- **Backup della libreria** (scheda Profili): «Scarica tutta la libreria» mette tutti i profili
  in un file; «Ripristina libreria…» li ricarica, saltando quelli con un nome già presente e
  convertendo l'unità se serve.

## Cold crash dalla dashboard (dalla 3.11.0)

Dashboard → Marcia → «cold crash fino a» e «Esegui cold crash», con il termostato o un profilo
in corso. Il piano è uno solo, scelto il 28/10/2026:

- **discesa di 3 °C ogni 6 ore** (0,5 °C/h) oppure, dalla 3.13.0, **1,5 °C ogni 6 ore**
  (0,25 °C/h, più dolce), dalla temperatura attuale al target (−1…12 °C), con il riscaldamento
  disattivato;
- quando il mosto **arriva** alla temperatura (entro la tolleranza) lo segnala in dashboard,
  sull'OLED («COLD CRASH: OK») e in Home Assistant («Cold crash raggiunto»), e da lì **la mantiene
  finché non fermi il profilo**: non c'è una durata;
- se il frigo è più lento del programma va bene, ma se in 6 ore il mosto scende meno della
  **soglia** impostata scatta l'allarme «COLD CRASH LENTO». Proposta: metà di quanto il
  programma scende in 6 ore (1,5 °C a 0,5 °C/h, 0,75 °C a 0,25 °C/h); massimo due terzi (2 °C e
  1 °C), perché vicino al programma l'allarme si accenderebbe anche con il frigo in regola;
- la dashboard mostra la **stima d'arrivo**: dalla discesa reale delle ultime ore (dopo la
  prima ora), prima sulla velocità programmata;
- **antigelo**: con il caldo disattivato e un setpoint fino a 4 °C, se il mosto scende di oltre
  1 °C sotto il setpoint (garage sotto zero) il caldo si riaccende finché non risale a mezzo
  grado. Vale per ogni step senza caldo con setpoint freddo, non solo per il cold crash. Se non
  basta (mosto sotto −2 °C, o 2 °C sotto il setpoint) scatta «MOSTO TROPPO FREDDO».

Non si avvia con il mosto già sotto il target: in quel caso si usa il termostato.

**Rampe lente e risoluzione delle sonde.** La rampa non la «legge» la sonda: la fa il setpoint,
che scende in modo continuo calcolato sul tempo, e il controllo tiene il mosto vicino a quel
setpoint. A 0,25 °C/h il setpoint scende di 0,0625 °C, cioè un passo della DS18B20 a 12 bit, ogni
15 minuti. La risoluzione limita quanto bene si sa dov'è il mosto in un istante, non la velocità
media: in 4 ore il setpoint scende di 1 °C, 16 passi. La precisione assoluta (±0,5 °C) non conta
per una pendenza, perché un errore costante sposta tutte le letture allo stesso modo. Il limite
vero è il frigo, acceso o spento: il mosto oscilla dentro la tolleranza e la sua massa media i
cicli, così la discesa reale è una scala a gradini piccoli che sulle ore segue la rampa. Il
modello «lager tradizionale» usa già 0,1 °C/h con lo stesso meccanismo.

## Note operative

- Le **notifiche** (dry hop, assaggio kettle sour) non sono nel firmware: si
  creano in HA con un'automazione sul cambio del sensore step (vedi
  docs/HOME_ASSISTANT.md).
- Il modello parte **disabilitato finché non premi Salva**: la galleria compila
  solo l'editor.
- La condizione "punti dal FG atteso" delle fonti (rest a FG+4 punti) si esprime
  qui come **% attenuazione** (≈ 50 %) o direttamente come soglia SG assoluta.
- Con densità in **°Plato** la banda di stabilità è in °P (usa ~0,2–0,5 °P dove i
  modelli dicono 0.001–0.002 SG).
