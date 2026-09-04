# Revisione forense AxHTherm 2.5 — Piano e tetti dichiarati

Data di avvio: 2026-08-23. Mandato: `docs/REVISIONE_FORENSE.md`.

## Tetti dichiarati prima di iniziare (§7, criterio 3; §10)

- **Giri di ricerca: massimo 5**, con strategia diversa a ogni giro, nell'ordine del §7 fase 5:
  1. per asse (§6, 8 agenti)
  2. per file (ledger §5, agenti per gruppo di file, letti per intero)
  3. per archetipo (§4, agenti che cercano ciascuna forma in tutta la codebase)
  4. per percorso d'uso (end-to-end)
  5. per assenza (cosa dovrebbe esserci e non c'è)
- **Agenti totali: massimo 140** (ricognizione + ricerca + confutazione + critici + sintesi).
- **Tetto per agente di ricerca: 8 rilievi**, i più gravi.
- **Criteri di arresto** (il primo che si verifica): convergenza come definita al §7;
  rendimento decrescente (<1 rilievo confermato ogni 2 agenti → il giro successivo è
  l'ultimo); esaurimento dei tetti sopra.

## Regole operative

- Nessuna modifica al codice durante la revisione.
- Nessun contatto con il dispositivo fisico: gli esperimenti "da verificare sul campo"
  vengono **descritti**, non eseguiti.
- Deduplicazione contro **tutti** i rilievi già visti, compresi i respinti.
- L'esito di ogni fase viene salvato in questa cartella prima di iniziare la successiva.

## File di esito previsti

- `00-mappa.md` — Fase 1, ricognizione
- `giroN-rilievi.json` — rilievi grezzi del giro N
- `giroN-confutazioni.json` — verdetti della fase avversariale del giro N
- `giroN-critico.md` — esito del critico di completezza dopo il giro N
- `REGISTRO-RILIEVI.md` — registro cumulativo (confermati, respinti, discordi)
- `REFERTO_FINALE.md` — Fase 6
