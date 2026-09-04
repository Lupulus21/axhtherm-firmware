#pragma once
#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <functional>
#include "svc/AppConfig.h"
#include "ax_dimensions.h"
#include "svc/AppStatus.h"
#include "svc/ConfigStore.h"
#include "svc/ProfileStore.h"
#include "svc/CalibStore.h"
#include "svc/DeviceRegistry.h"
#include "svc/BootLog.h"
#include "svc/DataLog.h"
#include "hal/ProbeBus.h"
#include "hal/OutputBus.h"
#include "svc/TimeService.h"
#include "ax/ProfileEngine.h"
#include "ax/ControlPipeline.h"
#include "ax/GravityTracker.h"

// REST JSON + UI statica gzippata (PROGMEM) + webhook iSpindel + OTA upload.
// Autenticazione HTTP Basic (utente "admin") su tutto tranne /ispindel.
class WebApi {
 public:
  // Riferimenti che appartengono a UNA camera. Separati da Refs (che raccoglie i servizi
  // di DISPOSITIVO: configurazione, store, bus sonde, uscite, orologio, registro) perché
  // gli endpoint chamber-scoped devono poter essere serviti per la camera indicata
  // nell'URL senza duplicare gli handler.
  struct ChamberRefs {
    uint8_t chamber = 0;
    ax::ProfileEngine* engine = nullptr;
    ax::ControlPipeline* pipeline = nullptr;
    ax::GravityTracker* gravity = nullptr;
    String* activeProfileName = nullptr;
    // Indice della sonda di CONTROLLO di questa camera: il ruolo non è più implicito
    // nell'indice 0, appartiene alla camera (ChamberCtx::probeControl).
    const uint8_t* controlProbe = nullptr;
    // Ausiliarie della camera: puntatore all'array in ChamberCtx (nessuna copia). Servono
    // per etichettare le temperature con la sonda GIUSTA: i valori arrivano dalla camera
    // servita, quindi anche label e "configured" devono venire dai suoi indici.
    const uint8_t* auxProbe = nullptr;
    uint8_t auxCount = 0;
    std::function<AppStatus()> status;
    std::function<bool()> isArmed;                  // stato LIVE (status è in cache 1 s)
    // (profileId 0=step attivi, batch, delayedStartAt) -> 0 ok / 404 / 400
    std::function<int(int, const String&, uint32_t)> startProfile;
    std::function<bool(uint8_t, uint8_t)> arm;      // (coolRel, heatRel)
    std::function<void()> disarm;   // disarma SOLO questa camera
    std::function<void(float)> setOg;
  };

  struct Refs {
    AppConfig* cfg = nullptr;
    ConfigStore* cfgStore = nullptr;
    ProfileStore* profStore = nullptr;
    ax::GravityTracker* gravityAny = nullptr;  // ultima lettura di qualsiasi densimetro
    CalibStore* calib = nullptr;
    DeviceRegistry* devices = nullptr;
    BootLog* boots = nullptr;       // storico dei riavvii
    String* calibDeviceName = nullptr;  // nome dell'ultimo densimetro che ha scritto
    ProbeBus* probes = nullptr;
    // Indice della sonda di CONTROLLO della camera servita da questa API. Il ruolo non
    // è più implicito nell'indice 0: appartiene alla camera (ChamberCtx). Finché l'API
    // non è per-camera (passo `api-v2`) punta alla camera 0.

    OutputBus* outputs = nullptr;
    TimeService* time = nullptr;
    DataLog* log = nullptr;
    // Vero se una camera QUALSIASI è armata. Semantica diversa da isArmed: i relè sono
    // una risorsa di dispositivo, quindi il test manuale va vietato se sta fermentando
    // una camera qualunque, non solo quella servita da questo endpoint.
    std::function<bool()> anyArmed;
    // Disarmo di DISPOSITIVO: ogni camera + tutti gli 8 canali. Distinto dal disarmo per
    // camera (ChamberRefs::disarm) perché l'endpoint storico /api/outputs/disarm non
    // porta un indice: per un'azione di sicurezza fare di più è la scelta giusta.
    std::function<void()> disarmAll;
    // Vero se la sonda `idx` è la sonda di CONTROLLO di una camera armata o con un
    // profilo in marcia. La taratura è un endpoint di DISPOSITIVO (una sonda sola, un
    // bagno di riferimento), quindi la guardia deve interrogare TUTTE le camere: con
    // c_() avrebbe controllato solo quella servita, cioè sempre la prima.
    std::function<bool(uint8_t)> probeInUse;
    // Rinvia il failsafe hardware da un percorso lungo ma legittimo CHE STA AVANZANDO.
    // Puntatore a funzione nudo, non std::function: viene chiamato una volta per RIGA
    // dell'export, fino a 50.000 volte.
    void (*progress)() = nullptr;
    std::function<void()> persistNow;               // transizione -> salva stato
    std::function<void()> applyConfigLive;          // riapplica config non di rete
    std::function<void(float, float, float, float, int, uint32_t, const String&)> ispindelData;
    // gravity, temp, batt, angle, rssi, deviceId, deviceName
    std::function<void()> factoryReset;
    std::function<void(uint32_t)> scheduleReboot;   // ms di ritardo
    // Una vista per camera. L\'endpoint scelto dall\'URL seleziona quale usare.
    ChamberRefs ch[AX_N_CHAMBERS];
  };

  void begin(const Refs& refs);

 private:
  // Registra un endpoint in DUE forme: quella storica (/api/<x>, camera 1) e quella per
  // camera (/api/ch/<n>/<x>). Gli handler non cambiano: leggono la camera da c_().
  void onChamber(const char* apiPath, HTTPMethod method, std::function<void()> h);
  // Dispatch delle rotte /api/ch/<n>/... senza registrarle una per una.
  //
  // Registrarle costava ~2,9 KB di heap PERMANENTE (15 handler, ciascuno con un
  // FunctionRequestHandler, un UriBraces clonato con la sua String, e una lambda che
  // cattura una std::function). Era la voce dinamica piu' grossa introdotta dalla seconda
  // camera — piu' del costo in RAM statica della camera stessa.
  // Qui si sfrutta il fatto che l'indice camera si ricava GIA' dall'URI (segAfter), quindi
  // UriBraces non serve: una sola tabella e un dispatch dentro onNotFound bastano, e gli
  // URL esposti restano identici.
  struct RottaCamera {
    const char* path;          // percorso BASE, es. "/api/status"
    HTTPMethod method;
    void (WebApi::*fn)();      // puntatore a membro: nessuna allocazione
  };
  static const RottaCamera kRotteCamera[];
  static const uint8_t kNRotteCamera;
  bool dispatchCamera();       // true se la richiesta e' stata servita
  // Involucri per i comandi di profilo, che il dispatch invoca senza argomenti.
  void cmdStart() { handleProfileCmd("start"); }
  void cmdStop() { handleProfileCmd("stop"); }
  void cmdPause() { handleProfileCmd("pause"); }
  void cmdResume() { handleProfileCmd("resume"); }
  void cmdSkip() { handleProfileCmd("skip"); }
  // Disarmo della SOLA camera indicata nell'URL, distinto da /api/outputs/disarm che e'
  // di dispositivo e le disarma tutte.
  void cmdDisarmCamera() {
    c_().disarm();
    r_.persistNow();
    sendOk();
  }
  bool selectChamber(const String& n);  // false = risposta 404 gia' inviata
  // Segmento di URI subito dopo `prefix` (fino al '/' successivo o alla fine); vuoto se
  // l'URI non inizia con prefix. Si legge da server_.uri() invece che da pathArg():
  // vedi il commento in selectChamberFromUri.
  String segAfter(const char* prefix) const;
  bool selectChamberFromUri();
 public:
  // sel_ azzerata PRIMA di servire: ogni richiesta parte dalla camera 1, e solo una
  // rotta /api/ch/<n>/... la sposta. Senza questo, la camera scelta da una richiesta
  // precedente resterebbe selezionata per la successiva — un endpoint di dispositivo
  // servirebbe silenziosamente la camera sbagliata.
  void loop() { sel_ = 0; server_.handleClient(); }

 private:
  bool auth();       // authValid() + risposta 401 JSON (senza popup del browser)
  bool authValid();  // cookie di sessione valido OPPURE Basic auth (fallback script)
  void computeToken();
  void handleLogin();
  void sendJson(int code, const String& body) {
    server_.send(code, "application/json", body);
  }
  void sendOk() { sendJson(200, "{\"ok\":true}"); }
  void sendErr(int code, const char* msg) {
    // Serializzato, non concatenato: da quando i messaggi includono le etichette
    // delle sonde (scritte dall'utente) un apice doppio romperebbe la risposta.
    StaticJsonDocument<256> d;
    d["ok"] = false;
    d["error"] = msg;
    String b;
    serializeJson(d, b);
    sendJson(code, b);
  }

  void handleRoot();
  void handleStatus();
  void handleGetConfig();
  void handlePutConfig();
  void handleGetProfile();
  void handlePutProfile();
  void handleProfilesList();
  void handleProfileById(int method);  // GET/PUT/DELETE su /api/profiles/{id}
  void handleProfileCmd(const String& cmd);
  void handleColdCrash();
  void handleSetpoint();
  void handleThermostat();
  void handleOg();
  void handleArm();
  void handleIspindel();
  void handleCalibGet();
  void handleCalibAdd();
  void handleCalibDelete();
  void handleLogCsv();
  void handleLogJson();
  void handleWifiScan();
  void handleDiagI2c();
  void handleDiagOneWire();
  void handleProbeDiag();
  void handleProbeCalib();
  void handleDiagRelay();
  void handleTime();
  void handleUpdateFinal();
  void handleUpdateUpload();

  ESP8266WebServer server_{80};
  Refs r_;
  // Camera servita dalla richiesta in corso. Gli endpoint "storici" (/api/status, ...)
  // la lasciano a 0, quindi con una sola camera il comportamento è identico a prima.
  uint8_t sel_ = 0;
  const ChamberRefs& c_() const { return r_.ch[sel_ < AX_N_CHAMBERS ? sel_ : 0]; }
  bool otaAuthorized_ = false;
  String sessionToken_;  // MD5(password+chipId): sopravvive ai riavvii, muore col cambio password
  String tokenPass_;
  // Diagnostica OneWire: scan()/tempById() resettano il bus, quindi si ri-scansiona al più ogni
  // 2 s (un polling serrato affamerebbe le letture periodiche e congelerebbe la sonda di controllo).
  uint32_t owScanMs_ = 0;
  String owProbesCache_ = "[]";
};
