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
  String uiPassword = kDefaultUiPassword;  // placeholder: sostituita al primo avvio (provisioning)
  // Vero finché le password sono quelle generate al primo avvio: l'OLED le mostra solo in
  // questa fase, con la rete di configurazione attiva. Diventa falso quando l'utente
  // sceglie la sua password dell'interfaccia.
  bool pwdAuto = false;
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

  // Sonde, nell'ordine controllo(mosto), camera, ambiente. Il ruolo di ciascuna vive in
  // ChamberCtx, non qui: questi sono solo gli slot.
  String probeId[ax::kMaxProbes];
  float probeOffset[ax::kMaxProbes] = {0.0f};
  String probeLabel[ax::kMaxProbes];

  // Nome della camera. Conservato nella sezione "chambers" di /config.json per
  // compatibilità con i file scritti dalla 3.0.0.
  String chamberLabel[AX_N_CHAMBERS];

  AppConfig() {
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) chamberLabel[i] = "Camera " + String(i + 1);
    for (uint8_t i = 0; i < ax::kMaxProbes; i++) probeLabel[i] = ax::defaultProbeRole(i);
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
  // Limiti sull'aria della camera (sonda "camera", ax::AirLimit), riferiti al setpoint.
  float airMinC = -5.0f;     // minimo assoluto dell'aria col freddo (-10 = nessun minimo, max 10)
  float airBelowSpC = 0.0f;  // aria al massimo cosi' sotto il setpoint (0 = nessun limite)
  float airAboveSpC = 10.0f; // aria al massimo cosi' sopra il setpoint (0 = nessun limite)

  // Sicurezza del riscaldatore (rilevatori di guasto, lib/axcore/src/ax/HeaterGuard.h).
  // Al rilevamento il caldo viene BLOCCATO (il freddo resta consentito) fino allo sblocco
  // manuale; il blocco sopravvive ai riavvii.
  uint16_t heatMaxOnH = 12;    // caldo acceso di continuo oltre N ore (2..48)
  uint16_t heatStuckMin = 60;  // sonda ferma a caldo acceso per N minuti (0 = spento, 15..240)

  // iSpindel / densità
  uint16_t gravityStaleMin = 60;  // oltre: dato stantio -> allarme, mai avanzare
  // Offset di taratura del densimetro assegnato: appartiene allo STRUMENTO. Applicato
  // dentro GravityTracker::update, cioè a monte di ogni allarme e confronto del motore.
  float gravityCorrection[AX_N_CHAMBERS] = {};
  float batteryAlertV = 3.3f;
  // Avviso «fermentazione ferma?» (3.16.0): densita' ferma da 48 ore ma sopra l'atteso.
  // Disattivabile per le birre che finiscono alte di proposito.
  bool fermStuckAlert = true;

  // Ventola della camera (3.17.0): rele' 0 = nessuna; modo 0 = segue caldo e freddo, 1 = sempre
  // accesa a regolazione attiva. Post-funzionamento in secondi; ricircolo «X min ogni Y min»
  // (0 = spento).
  uint8_t fanRel = 0;
  uint8_t fanMode = 0;
  uint16_t fanPostRunSec = 120;
  uint16_t fanCycleOnMin = 0;
  uint16_t fanCycleEveryMin = 0;
  String gravityUnit = "sg";  // D11: "sg" o "plato". NON solo etichetta: governa i
                              // confronti/formule degli step a densità, il clamp di
                              // /ispindel e la scala del log; immutabile a profilo in corso.
  String ispindelToken = "";  // se non vuoto: il POST /ispindel deve portare lo stesso token
  // Densimetro assegnato, per NOME (confronto senza distinzione di maiuscole). Vuoto =
  // accetta chiunque. Una lettura SENZA nome passa sempre: il Tilt inoltrato da Home
  // Assistant non manda "name" (HOME_ASSISTANT.md §7). Filtro applicato in onIspindel().
  String gravityDevice[AX_N_CHAMBERS];

  // Display OLED — controller UNVERIFIED: selezionabile
  bool oledEnabled = true;
  String oledDriver = "ssd1306";  // "ssd1306" | "sh1106"
  uint8_t oledAddress = 0x3C;
  bool oledFlip = false;
  // Luminosita' (3.15.0, PH-28/UX-16): 207 = quella della libreria, cioe' come prima.
  // Piu' bassa allunga la vita dei pixel di un display acceso giorno e notte.
  uint8_t oledBrightness = 207;

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
