#pragma once
#include <memory>
#include <new>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ax/Types.h"

// Profili di fermentazione su LittleFS.
// - PROFILO ATTIVO (/profile.json): la copia in esecuzione, con il nome del profilo
//   di origine. È quella che sopravvive ai blackout; modificare la libreria non la tocca.
// - LIBRERIA (/profiles/p<id>.json, id 1..20): le ricette con nome, richiamabili
//   dalla dashboard all'avvio di una fermentazione.
class ProfileStore {
 public:
  static constexpr const char* kPath = "/profile.json";
  static constexpr uint8_t kMaxProfiles = 20;

  // Capacita' di un documento "profilo completo", DERIVATA dalla struttura invece che
  // arrotondata a 6144. Il numero tondo era il DOPPIO del fabbisogno reale, e su questo
  // dispositivo il piu' grande blocco CONTIGUO misurato scende fino a 5.848 B: un blocco
  // da 6.152 non e' garantito, ed e' esattamente cosi' che una lettura di profilo e'
  // tornata troncata in campo. Con 12 step: 4.144 B (con il campo "attendi il target"), sotto il minimo osservato.
  // Il margine di 1 KB copre le chiavi (14 distinte, deduplicate) e il nome del profilo.
  static constexpr size_t kProfileDoc =
      JSON_OBJECT_SIZE(3) + JSON_ARRAY_SIZE(ax::kMaxSteps) +
      ax::kMaxSteps * JSON_OBJECT_SIZE(15) + 1024;

  // Profilo attivo.
  bool loadActive(String* name, ax::StepDef steps[ax::kMaxSteps], uint8_t* count);
  bool saveActive(const String& name, const ax::StepDef* steps, uint8_t count);

  // Libreria.
  bool load(uint8_t id, String* name, ax::StepDef steps[ax::kMaxSteps], uint8_t* count);
  bool save(uint8_t id, const String& name, const ax::StepDef* steps, uint8_t count);
  bool remove(uint8_t id);
  bool exists(uint8_t id) const;  // il file c'è (per distinguere 404 da errore di lettura)
  // Riempie arr con {id, name, steps, durationSec, gravity} per ogni profilo esistente.
  // False se manca la memoria per leggere i profili: chi chiama NON deve mandare una lista
  // vuota (il client crederebbe liberi gli id e li sovrascriverebbe).
  bool list(JsonArray& arr);
  // Step sull'heap per la durata di una richiesta (3.17.3): 12 step sono ~530 B, che sullo
  // stack dei handler web si sommavano a LittleFS e all'invio TCP.
  struct StepBuf {
    std::unique_ptr<ax::StepDef[]> p{new (std::nothrow) ax::StepDef[ax::kMaxSteps]};
    explicit operator bool() const { return (bool)p; }
    ax::StepDef* get() { return p.get(); }
  };

  static void stepsToJson(const ax::StepDef* steps, uint8_t count, JsonDocument& doc);
  static uint8_t stepsFromJson(JsonDocument& doc, ax::StepDef* steps);

 private:
  bool readFile(const String& path, String* name, ax::StepDef steps[], uint8_t* count);
  bool writeFile(const String& path, const String& tmpPath, const String& name,
                 const ax::StepDef* steps, uint8_t count);
};
