#pragma once
#include <Arduino.h>
#include "ax_dimensions.h"
#include "svc/AppStatus.h"
#include "ax/ProfileEngine.h"
#include "ax/ControlPipeline.h"
#include "ax/SafetyMonitor.h"
#include "ax/HeaterGuard.h"
#include "ax/SlopeEstimator.h"
#include "ax/AirLimit.h"
#include "ax/CoolWatch.h"
#include "ax/CrashMonitor.h"

// Tutto lo stato della camera di fermentazione: le istanze del motore di controllo
// (lib/axcore) e lo stato di contorno che in v2.23.2 viveva in variabili globali di
// main.cpp o in `static` locali di funzione (rilevatore di scostamento termico,
// cross-check della sonda di controllo). Raccolto qui dalla 3.0.0, che gestiva più
// camere; dalla 3.1.0 l'istanza è una sola (AX_N_CHAMBERS = 1).
struct ChamberCtx {
  uint8_t idx = 0;  // identità della camera (sempre 0)

  // --- mappa RUOLO -> indice sonda ---
  // controllo = sonda 0 (mosto), ausiliarie = sonde 1 e 2 (camera, ambiente).
  uint8_t probeControl = 0;
  uint8_t probeAux[ax::kAuxPerChamber] = {0};
  uint8_t auxCount = 0;

  void assignDefaultProbes() {
    const uint8_t base = (uint8_t)(idx * ax::kProbesPerChamber);
    probeControl = base;
    auxCount = ax::kAuxPerChamber;
    for (uint8_t i = 0; i < auxCount; i++) probeAux[i] = (uint8_t)(base + 1 + i);
  }

  // Consenso all'automazione: il mapping relè->carico va confermato dall'utente.
  bool armed = false;

  // --- catena di controllo (già per-istanza in lib/axcore) ---
  ax::ProfileEngine engine;
  ax::ControlPipeline pipeline;
  ax::SafetyMonitor safety;
  ax::HeaterGuard heaterGuard;
  ax::SlopeEstimator slope;  // pendenza del mosto, per il tempo d'arrivo
  ax::AirLimit airLimit;     // limiti sull'aria della camera
  ax::CoolWatch coolWatch;   // relè del frigo incollato (solo allarme)
  bool coolUncommanded = false;
  ax::CrashMonitor crash;    // discesa reale del cold crash (stima d'arrivo, discesa lenta)
  bool crashActive = false;
  // Antigelo: caldo consentito anche su uno step che lo vieta, con setpoint freddo e
  // mosto oltre 1 °C sotto il setpoint (garage sotto zero durante un cold crash).
  bool antigelo = false;
  bool tooCold = false;  // vedi AppStatus::alarmTooCold
  // Limiti ASSOLUTI sul mosto, fissi nel codice (con isteresi): caldo tolto oltre 42 °C,
  // freddo tolto sotto -3 °C. I setpoint stanno in -1..40: scattano solo se qualcosa a
  // monte ha sbagliato. Ultima rete, indipendente dal setpoint.
  bool absHeatCut = false, absCoolCut = false;

  // Blocco del caldo (ax::HeatLock, 0 = nessuno): toglie il consenso al SOLO caldo, il
  // freddo resta consentito. Si toglie solo con un gesto esplicito dell'utente ed e'
  // salvato su flash (/state.json): un riavvio non lo cancella.
  uint8_t heatLock = 0;
  uint32_t heatLockEpoch = 0;

  // --- esito dell'ultimo tick ---
  ax::ProfileEngine::TickOutput lastTick;
  ax::ControlPipeline::Status lastPipe;
  bool prevCool = false;

  // Ripresa con orologio inaffidabile: ri-àncora la rampa in corso alla temperatura
  // REALE al primo tick con sonda valida.
  bool rebaseRampPending = false;

  // --- identificazione della cotta in corso ---
  String activeProfileName;

  // --- fotografia per UI/OLED/MQTT, ricostruita una volta al secondo ---
  AppStatus curStatus;

  // --- allarme di scostamento termico ---
  // Soglia: scarto > 5 °C senza progresso >= 0,5 °C dal picco per 3 h.
  float devSp = -999.0f;      // setpoint su cui si sta misurando lo scostamento
  float devBestGap = 1e9f;    // minimo scarto raggiunto da quando la finestra si è aperta
  uint32_t devStallMs = 0;    // durata ACCUMULATA di stallo (non "now - inizio")
  bool devStalling = false;   // finestra di stallo aperta

  // --- cross-check di plausibilità della sonda di controllo ---
  // Soglia: divergenza > 15 °C da TUTTE le ausiliarie per 30 min accumulati.
  uint32_t probeSuspectMs = 0;
};
