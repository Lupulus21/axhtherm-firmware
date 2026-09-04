#pragma once
#include <Arduino.h>
#include "ax_dimensions.h"
#include "svc/AppStatus.h"
#include "ax/ProfileEngine.h"
#include "ax/ControlPipeline.h"
#include "ax/SafetyMonitor.h"

// Tutto lo stato che appartiene a UNA camera di fermentazione.
//
// Il motore di controllo (lib/axcore) era già fatto di oggetti senza stato globale e
// quindi già istanziabile N volte senza modifiche: qui si limita a raccoglierne le
// istanze insieme allo stato di contorno che in v2.23.2 viveva in variabili globali
// di main.cpp o — più insidioso — in `static` locali di funzione.
//
// Le `static` locali sono il motivo principale per cui questa struct esiste: NON danno
// errore di compilazione quando il codice viene chiamato per due camere, diventano
// semplicemente condivise in silenzio. Erano quattro nel rilevatore di scostamento
// termico e una nel cross-check della sonda di controllo.
struct ChamberCtx {
  uint8_t idx = 0;  // 0..AX_N_CHAMBERS-1: identità della camera

  // --- mappa RUOLO -> indice sonda ---
  // Il ruolo di una sonda (controllo/ausiliaria) appartiene alla CAMERA, non al bus:
  // in v2.23.2 era invece cablato nell'indice (ProbeBus::control() ritornava st_[0]).
  // Con due camere l'indice 0 non è più "la" sonda di controllo, ed è essenziale che
  // il cross-check di plausibilità confronti il mosto SOLO con le ausiliarie della
  // PROPRIA camera: confrontarlo con quelle dell'altra non significherebbe nulla.
  uint8_t probeControl = 0;
  uint8_t probeAux[ax::kAuxPerChamber] = {0};
  uint8_t auxCount = 0;

  // Assegnazione contigua di default: la camera i prende le sonde
  // [i*3, i*3+1, i*3+2]. Per la camera 0 dà controllo=0, ausiliarie={1,2},
  // cioè esattamente la mappatura della v2.23.2.
  void assignDefaultProbes() {
    const uint8_t base = (uint8_t)(idx * ax::kProbesPerChamber);
    probeControl = base;
    auxCount = ax::kAuxPerChamber;
    for (uint8_t i = 0; i < auxCount; i++) probeAux[i] = (uint8_t)(base + 1 + i);
  }

  // Consenso all'automazione per QUESTA camera. Il mapping relè->carico va confermato
  // dall'utente camera per camera: armare la 1 non deve armare la 2.
  bool armed = false;

  // --- catena di controllo (già per-istanza in lib/axcore) ---
  ax::ProfileEngine engine;
  ax::ControlPipeline pipeline;
  ax::SafetyMonitor safety;

  // --- esito dell'ultimo tick ---
  ax::ProfileEngine::TickOutput lastTick;
  ax::ControlPipeline::Status lastPipe;
  bool prevCool = false;

  // Ripresa con orologio inaffidabile: ri-àncora la rampa in corso alla temperatura
  // REALE al primo tick con sonda valida. È stato PER CAMERA: in v2.23.2 era una
  // `static` di FILE, e come tale è sopravvissuta al primo passaggio di ChamberCtx
  // (che aveva raccolto le `static` LOCALI di funzione). Lasciata globale, sarebbe
  // stata armata dal profilo della camera 0 e consumata dalla PRIMA camera che nel
  // giro di controllo avesse avuto la sonda valida: la rampa di una camera ri-ancorata
  // alla temperatura dell'altra.
  bool rebaseRampPending = false;

  // --- identificazione della cotta in corso ---
  String activeProfileName;

  // --- fotografia per UI/OLED/MQTT, ricostruita una volta al secondo ---
  AppStatus curStatus;

  // --- allarme di scostamento termico (erano `static` locali in controlTick) ---
  // Soglia: scarto > 5 °C senza progresso >= 0,5 °C dal picco per 3 h.
  float devSp = -999.0f;      // setpoint su cui si sta misurando lo scostamento
  float devBestGap = 1e9f;    // minimo scarto raggiunto da quando la finestra si è aperta
  uint32_t devStallMs = 0;    // durata ACCUMULATA di stallo (non "now - inizio")
  bool devStalling = false;   // finestra di stallo aperta

  // --- cross-check di plausibilità della sonda di controllo (era `static` locale) ---
  // Soglia: divergenza > 15 °C da TUTTE le ausiliarie per 30 min accumulati.
  uint32_t probeSuspectMs = 0;
};
