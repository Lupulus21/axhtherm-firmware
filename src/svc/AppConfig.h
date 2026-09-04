#pragma once
#include <Arduino.h>
#include "ax_dimensions.h"

// Configurazione completa dell'applicazione (persistita in /config.json su LittleFS).
// Ogni campo ha un default sicuro: config assente o corrotta => default + valid=false
// (e con valid=false le uscite restano spente: vedi SafetyMonitor).
struct AppConfig {
  // Versione dello schema di /config.json. ASSENTE nei file scritti fino alla v2.23.2:
  // la sua assenza È il marcatore della v1. La migrazione dalla v1 è gratuita per
  // costruzione — fromJson usa ovunque l'idioma `x | default`, quindi le chiavi nuove
  // prendono semplicemente il default — ma il campo serve alle migrazioni FUTURE, che
  // senza un numero di versione non potrebbero distinguere "assente" da "vecchio".
  static constexpr uint8_t kSchemaVersion = 2;

  // Sistema
  static constexpr const char* kDefaultUiPassword = "axhtherm";
  String hostname = "axhtherm";
  String uiPassword = kDefaultUiPassword;  // placeholder (D5): da cambiare al primo avvio
  bool usingDefaultUiPassword() const { return uiPassword == kDefaultUiPassword; }

  // Rete
  String ssid = "";
  String wifiPassword = "";
  bool dhcp = true;
  String ip = "", mask = "", gw = "", dns = "";
  String softApPassword = "axhtherm25";  // placeholder (D5)
  uint8_t softApAfterMin = 5;

  // Orologio
  bool ntpEnabled = true;                          // D12
  String ntpServer = "pool.ntp.org";               // unico host esterno, disattivabile
  String tz = "CET-1CEST,M3.5.0,M10.5.0/3";        // D6: Europe/Rome con ora legale

  // Sonde: kProbesPerChamber per camera, nell'ordine controllo(mosto), camera, ambiente.
  // L'assegnazione sonda->camera vive in ChamberCtx, non qui: questi sono solo gli slot.
  String probeId[ax::kMaxProbes];
  float probeOffset[ax::kMaxProbes] = {0.0f};
  String probeLabel[ax::kMaxProbes];

  // Nome della camera, mostrato in UI/OLED. NB: i parametri di REGOLAZIONE (PID,
  // tolleranza, tempi) restano CONDIVISI fra le camere. È una scelta deliberata: sono
  // 13 campi e duplicarli gonfierebbe /config.json, che è servito e salvato con
  // documenti JSON da 6 KB su uno heap di ~17 KB — la causa accertata dei salvataggi
  // falliti in campo. L'inerzia specifica di ciascun frigo è comunque già per camera:
  // la imparano gli stimatori predittivi (coolEstCph/heatEstCph in /state.json).
  // Una taratura PID per camera resta un'estensione naturale, da fare misurando.
  String chamberLabel[AX_N_CHAMBERS];

  AppConfig() {
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) chamberLabel[i] = "Camera " + String(i + 1);
    // Etichette di default. A camera singola sono esattamente "mosto"/"camera"/
    // "ambiente" come la v2.23.2; dalla seconda camera in poi prendono il suffisso.
    for (uint8_t i = 0; i < ax::kMaxProbes; i++) {
      probeLabel[i] = ax::defaultProbeRole(i);
      const uint8_t camera = i / ax::kProbesPerChamber;
      if (camera > 0) probeLabel[i] += " " + String(camera + 1);
    }
  }

  // Regolazione
  bool usePid = true;  // D9: PID sul caldo; false = isteresi anche sul caldo
  bool predictive = true;  // spegnimento predittivo (stile BrewPi): impara l'inerzia
  float toleranceC = 0.5f;
  float pidKp = 2.0f, pidKi = 1.0f, pidKd = 5.0f;  // default dal manuale originale
  uint32_t pidWindowMs = 30000;
  float pidFullPowerBelow = 5.0f;
  float pidCutAbove = 0.5f;
  uint16_t compressorRestMin = 5;
  uint16_t compressorMinOnSec = 60;
  uint16_t minStateSec = 60;
  uint16_t minSwitchSec = 120;

  // iSpindel / densità
  uint16_t gravityStaleMin = 60;  // oltre: dato stantio -> allarme, mai avanzare
  // Offset di taratura, PER CAMERA perché appartiene allo STRUMENTO, non al sistema:
  // due iSpindel hanno tarature diverse, e applicare quella dell'uno alle letture
  // dell'altro falsifica la densità dentro GravityTracker::update, cioè a monte di
  // ogni allarme e di ogni confronto del motore. Costa 4 B per camera.
  float gravityCorrection[AX_N_CHAMBERS] = {};
  float batteryAlertV = 3.3f;
  String gravityUnit = "sg";  // D11: "sg" o "plato". NON solo etichetta: governa i
                              // confronti/formule degli step a densità, il clamp di
                              // /ispindel e la scala del log; immutabile a profilo in corso.
  String ispindelToken = "";  // se non vuoto: il POST /ispindel deve portare lo stesso token
  // Densimetro assegnato a ciascuna camera, per NOME (confronto senza distinzione di
  // maiuscole). Regola di instradamento, valida per costruzione su AL PIÙ UNA camera —
  // vedi onIspindel() in main.cpp, che è l'unico punto che la applica:
  //   1. lettura SENZA nome        -> camera 0, sempre (storico: il Tilt inoltrato da
  //                                   Home Assistant non manda "name" — HOME_ASSISTANT.md §7);
  //   2. nome rivendicato da una camera -> quella camera, e nessun'altra;
  //   3. nome non rivendicato      -> camera 0 solo se il suo campo è VUOTO
  //                                   ("accetta chiunque", storico), altrimenti scartata.
  // Vuoto sulla camera 0 = accetta chiunque. Vuoto su una camera con indice > 0 =
  // NON ASSEGNATA: non riceve letture, e i suoi step a densità non avanzano (né per
  // condizione né per timeout: vedi TickInput::gravitySourceMissing).
  // A AX_N_CHAMBERS=1 le tre regole si riducono esattamente al filtro della v2.23.2.
  String gravityDevice[AX_N_CHAMBERS];

  // Display OLED — controller UNVERIFIED: selezionabile
  bool oledEnabled = true;
  String oledDriver = "ssd1306";  // "ssd1306" | "sh1106"
  uint8_t oledAddress = 0x3C;
  bool oledFlip = false;

  // Log storico
  uint8_t logSampleMin = 5;      // D8
  uint16_t logRetentionDays = 14;

  // MQTT (D2: incluso, spento finché non configurato)
  bool mqttEnabled = false;
  String mqttHost = "";
  uint16_t mqttPort = 1883;
  String mqttUser = "", mqttPass = "";
  String mqttBaseTopic = "axhtherm";
  bool haDiscovery = true;

  // OTA dalla web UI (D2)
  bool otaEnabled = true;

  uint32_t configRev = 0;  // revisione per il locking ottimistico: incrementata a ogni
                           // salvataggio (PUT config, taratura). Un PUT che porta una
                           // revisione più vecchia è rifiutato (un altro client ha scritto).
  bool valid = false;  // true solo se caricata e parsata correttamente
};
