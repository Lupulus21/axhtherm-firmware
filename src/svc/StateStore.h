#pragma once
#include <Arduino.h>
#include "ax_dimensions.h"
#include "ax/Types.h"
#include "ax/CompressorGuard.h"

// Stato runtime di UNA camera: ciò che serve per riprendere la sua cotta dopo un
// reset o un blackout.
struct ChamberState {
  ax::EngineSnapshot engine;
  ax::CompressorGuard::Persist guard;
  bool coolWasOn = false;
  bool armed = false;
  uint8_t coolRel = 0;
  uint8_t heatRel = 0;
  String batchName = "";        // nome della cotta in corso (facoltativo)
  float ogGravity = 0;          // densità iniziale della cotta (0 = non ancora catturata)
  float coolEstCph = 0;         // coefficiente appreso dello spegnimento predittivo
  float heatEstCph = 0;         // (°C per ora di marcia); 0 = mai appreso
};

// Stato runtime persistito su /state.json. Scritto SOLO sulle transizioni + checkpoint
// ogni 5 minuti.
struct PersistState {
  // Versione dello schema del file. ASSENTE nei file scritti fino alla v2.23.2: la sua
  // assenza È il marcatore della v1 (formato mono-camera, chiavi piatte in radice).
  static constexpr uint8_t kSchemaVersion = 2;

  // Globali di dispositivo (non appartengono a nessuna camera).
  uint32_t lastSeenEpoch = 0;   // per la verifica di monotonia dell'RTC al boot
  bool epochsTrusted = false;   // gli epoch salvati sono sulla timeline reale?
  // Il failsafe hardware e' scattato e l'evento non e' ancora stato preso in carico.
  // Persistito perche' altrimenti si perderebbe proprio nel caso per cui il failsafe
  // esiste: un blocco durante un'OTA, che finisce con un riavvio.
  bool failsafePending = false;

  ChamberState ch[AX_N_CHAMBERS];
};

class StateStore {
 public:
  static constexpr const char* kPath = "/state.json";
  // Legge sia lo schema v2 (per camera) sia il v1 mono-camera della v2.23.2, che viene
  // migrato nella camera 0. Ritorna false solo su file assente o JSON illeggibile.
  bool load(PersistState* st);
  bool save(const PersistState& st);  // atomica

  // Vero se l'ultimo load() ha migrato un file di schema v1 (mono-camera): il chiamante
  // può volerlo registrare, perché il prossimo save() riscriverà il file in v2 e la
  // conversione non è reversibile.
  bool migratedFromV1() const { return migratedV1_; }

  // Quante camere il file letto descriveva davvero. Serve a distinguere "camera che il
  // file non conosceva" (va irrobustita: riposo pieno) da "primissimo avvio, nessun
  // file" (comportamento storico: nessun vincolo). Valida solo dopo un load() riuscito.
  uint8_t chambersRead() const { return chambersRead_; }

  // Quante camere del file sono state SCARTATE perché il firmware ne gestisce meno
  // (accade riportando indietro il firmware). Il loro stato è perso: va detto, non
  // lasciato scoprire dopo.
  uint8_t excessChambers() const { return excessChambers_; }

 private:
  bool migratedV1_ = false;
  uint8_t chambersRead_ = 0;
  uint8_t excessChambers_ = 0;
};
