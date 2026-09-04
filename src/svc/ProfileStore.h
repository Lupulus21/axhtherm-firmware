#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ax/Types.h"
#include "ax_dimensions.h"

// Profili di fermentazione su LittleFS.
// - PROFILO ATTIVO, UNO PER CAMERA: la copia in esecuzione, con il nome del profilo di
//   origine. È quella che sopravvive ai blackout; modificare la libreria non la tocca.
//   La camera 0 usa /profile.json — lo stesso percorso della v2.23.2, quindi il profilo
//   attivo di una cotta in corso NON va migrato: è già al posto giusto. Le camere
//   successive usano /profile<N>.json.
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
  // tornata troncata in campo. Con 12 step: 3.952 B, che stanno sotto il minimo osservato.
  // Il margine di 1 KB copre le chiavi (14 distinte, deduplicate) e il nome del profilo.
  static constexpr size_t kProfileDoc =
      JSON_OBJECT_SIZE(3) + JSON_ARRAY_SIZE(ax::kMaxSteps) +
      ax::kMaxSteps * JSON_OBJECT_SIZE(14) + 1024;

  // Profilo attivo, per camera.
  bool loadActive(uint8_t ch, String* name, ax::StepDef steps[ax::kMaxSteps], uint8_t* count);
  bool saveActive(uint8_t ch, const String& name, const ax::StepDef* steps, uint8_t count);
  // Percorso del profilo attivo di una camera: la 0 mantiene kPath (retrocompatibile).
  static String activePath(uint8_t ch) {
    return ch == 0 ? String(kPath) : String("/profile") + ch + ".json";
  }

  // Libreria.
  bool load(uint8_t id, String* name, ax::StepDef steps[ax::kMaxSteps], uint8_t* count);
  bool save(uint8_t id, const String& name, const ax::StepDef* steps, uint8_t count);
  bool remove(uint8_t id);
  bool exists(uint8_t id) const;  // il file c'è (per distinguere 404 da errore di lettura)
  // Riempie arr con {id, name, steps, durationSec, gravity} per ogni profilo esistente.
  void list(JsonArray& arr);

  static void stepsToJson(const ax::StepDef* steps, uint8_t count, JsonDocument& doc);
  static uint8_t stepsFromJson(JsonDocument& doc, ax::StepDef* steps);

 private:
  bool readFile(const String& path, String* name, ax::StepDef steps[], uint8_t* count);
  bool writeFile(const String& path, const String& tmpPath, const String& name,
                 const ax::StepDef* steps, uint8_t count);
};
