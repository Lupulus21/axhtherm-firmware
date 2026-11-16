#pragma once
#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
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
#include "svc/EventLog.h"
#include "hal/ProbeBus.h"
#include "hal/OutputBus.h"
#include "svc/TimeService.h"
#include "svc/HeapMon.h"
#include "svc/LoopStats.h"
#include "svc/RebootInfo.h"
#include "ax/ProfileEngine.h"
#include "ax/RetryPenalty.h"
#include "ax/FwImageCheck.h"
#include "ax/ControlPipeline.h"
#include "ax/GravityTracker.h"

// REST JSON + UI statica gzippata (PROGMEM) + webhook iSpindel + OTA upload.
// Autenticazione HTTP Basic (utente "admin") su tutto tranne /ispindel.
class WebApi {
 public:
  // Riferimenti che appartengono alla camera, separati da Refs (servizi di DISPOSITIVO:
  // configurazione, store, bus sonde, uscite, orologio, registro).
  struct ChamberRefs {
    uint8_t chamber = 0;
    ax::ProfileEngine* engine = nullptr;
    ax::ControlPipeline* pipeline = nullptr;
    ax::GravityTracker* gravity = nullptr;
    String* activeProfileName = nullptr;
    // Indici delle sonde di controllo e ausiliarie (ChamberCtx): servono per etichettare
    // le temperature con la sonda giusta.
    const uint8_t* controlProbe = nullptr;
    const uint8_t* auxProbe = nullptr;
    uint8_t auxCount = 0;
    std::function<AppStatus()> status;
    std::function<bool()> isArmed;                  // stato LIVE (status è in cache 1 s)
    // (profileId 0=step attivi, batch, delayedStartAt) -> 0 ok / 404 / 400
    std::function<int(int, const String&, uint32_t)> startProfile;
    std::function<bool(uint8_t, uint8_t)> arm;      // (coolRel, heatRel)
    std::function<void()> disarm;
    std::function<void(float)> setOg;
    // Cold crash appena avviato: (target °C, soglia di discesa minima in 6 ore, °C/h).
    std::function<void(float, float, float)> beginCrash;
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
    OutputBus* outputs = nullptr;
    TimeService* time = nullptr;
    DataLog* log = nullptr;
    EventLog* events = nullptr;  // registro eventi nella EEPROM (3.15.0); spento se assente
    // Scrive un evento con l'ora corrente (main): configurazione salvata, OTA accettata.
    std::function<void(uint8_t code, uint8_t a, uint16_t b, int16_t v)> logEvent;
    // Vero se la camera è armata: il test manuale dei relè è vietato.
    std::function<bool()> anyArmed;
    // Disarmo completo: la camera + tutti gli 8 canali (anche quelli di un test manuale).
    std::function<void()> disarmAll;
    // Vero se la sonda `idx` è la sonda di CONTROLLO della camera armata o con un
    // profilo in marcia: la taratura va rifiutata.
    std::function<bool(uint8_t)> probeInUse;
    // Rinvia il failsafe hardware da un percorso lungo ma legittimo CHE STA AVANZANDO.
    // Puntatore a funzione nudo, non std::function: viene chiamato una volta per RIGA
    // dell'export, fino a 50.000 volte.
    void (*progress)() = nullptr;
    const HeapMon* heap = nullptr;  // minimi di memoria dall'avvio
    const LoopStats* loop = nullptr;  // salute del loop di controllo
    const RebootInfo* reboot = nullptr;  // motivo dell'ultimo riavvio, riavvii a catena
    std::function<bool()> persistNow;               // transizione -> salva stato (esito)
    std::function<bool()> clearHeatLock;            // sblocco manuale del caldo (esito del salvataggio)
    std::function<void()> applyConfigLive;          // riapplica config non di rete
    std::function<void(float, float, float, float, int, uint32_t, const String&)> ispindelData;
    // gravity, temp, batt, angle, rssi, deviceId, deviceName
    std::function<void()> factoryReset;
    std::function<void(uint32_t, uint8_t)> scheduleReboot;   // ms di ritardo, RebootIntent
    ChamberRefs ch[AX_N_CHAMBERS];
  };

  void begin(const Refs& refs);

  void loop() {
    server_.handleClient();
    if (csv_.active) csvPump();
  }

 private:
  // Segmento di URI subito dopo `prefix` (fino al '/' successivo o alla fine); vuoto se
  // l'URI non inizia con prefix. Si legge da server_.uri() invece che da pathArg():
  // su una richiesta al percorso LETTERALE con le graffe (es. "/api/profiles/{}")
  // UriBraces combacia senza popolare pathArgs e pathArg(0) va in assert, cioè un
  // panic raggiungibile senza autenticazione.
  String segAfter(const char* prefix) const;

  bool auth();       // authValid() + risposta 401 JSON (senza popup del browser)
  bool authValid();  // cookie di sessione valido OPPURE Basic auth (fallback script)
  void computeToken();
  // Nome con cui la scheda e' stata chiamata (intestazione Host): un IP, il suo nome, il
  // suo nome .local o con uno dei domini dei router di casa. Altri nomi = DNS rebinding: una
  // pagina web che si fa passare per la scheda. Se falso e reply, ha gia' risposto 403.
  bool hostOk(bool reply = true);
  // Penalita' sui tentativi sbagliati, per indirizzo IP di chi prova. pwLocked: se in
  // pausa ha gia' risposto 429.
  uint32_t ipKey() { return (uint32_t)server_.client().remoteIP(); }
  bool pwLocked();
  void pwFail() { pwPen_.fail(ipKey(), millis()); }
  void sendWait(uint32_t sec);
  // Basic auth sbagliata (una richiesta senza credenziali non e' un tentativo).
  void countBasicFail() {
    if (server_.hasHeader(F("Authorization")) && !pwPen_.locked(ipKey(), millis())) pwFail();
  }
  // Impronta delle impostazioni attive solo al riavvio (rete, display, MQTT, ora).
  static uint32_t netSig(const AppConfig& c);
  void handleLogin();
  void sendJson(int code, const String& body) {
    server_.send(code, "application/json", body);
  }
  void sendOk() { sendJson(200, "{\"ok\":true}"); }
  // Esito di un comando che cambia lo stato: "ok" solo se e' anche salvato, altrimenti al
  // prossimo riavvio tornerebbe indietro (un disarmo che si riarma, un setpoint di prima).
  void sendSaved(bool saved) {
    if (saved) sendOk();
    else sendErr(500, F("comando eseguito ma NON salvato su flash: al riavvio tornerebbe com'era"));
  }
  // I messaggi fissi passano con F("..."): restano in flash invece di occupare DRAM
  // (su ESP8266 i letterali normali stanno in .rodata, cioè in RAM).
  void sendErr(int code, const char* msg) { sendErrT(code, msg); }
  void sendErr(int code, const __FlashStringHelper* msg) { sendErrT(code, msg); }
  template <typename M>
  void sendErrT(int code, M msg) {
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
  void handleEvents();
  // Parti di /api/status (3.17.2): funzioni separate, cosi' i loro temporanei non si sommano.
  void statusSistema(JsonDocument& doc, const AppStatus& st);
  void statusCamera(JsonDocument& doc, const AppStatus& st);
  void statusAllarmi(JsonDocument& doc, const AppStatus& st);
  void statusRete(JsonDocument& doc, const AppStatus& st);
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
  const ChamberRefs& c_() const { return r_.ch[0]; }
  bool otaAuthorized_ = false;
  String corretti_;  // campi riportati nei limiti dall'ultimo PUT /api/config (JSON)
  String offsetMossi_;  // posizioni (da 1) la cui taratura e' stata spostata o azzerata
  bool profiloDensita() const;
  // Motivo dell'ultimo rifiuto OTA: 1 disattivato, 2 password di fabbrica, 3 password
  // errata, 5 troppi tentativi (otaWait_ secondi), 6 nome della scheda non riconosciuto.
  uint8_t otaDeny_ = 0;
  uint32_t otaWait_ = 0;
  ax::FwImageCheck fwCheck_;  // il file ricevuto e' un firmware intero e integro?
  ax::FwImageCheck::Result otaImg_ = ax::FwImageCheck::Result::Ok;
  // Impostazioni salvate che diventano attive solo al riavvio (rete, display, MQTT, ora):
  // il riavvio si propone, non si impone. La UI chiede, e lo ricorda finche' non avviene.
  bool rebootPending_ = false;
  uint32_t bootNetSig_ = 0;  // netSig all'avvio: in sospeso solo se le impostazioni ne differiscono
  String bootHost_;  // nome in uso dall'avvio (mDNS): resta valido finche' non si riavvia
  ax::RetryPenaltyByKey<4> pwPen_;   // password: login, Basic, password attuale, OTA
  ax::RetryPenaltyByKey<4> tokPen_;  // codice dei densimetri: separato, un densimetro
                                     // configurato male non deve chiudere fuori l'utente
  String sessionToken_;  // MD5(password+chipId): sopravvive ai riavvii, muore col cambio password
  String tokenPass_;
  // Diagnostica OneWire: scan()/tempById() resettano il bus, quindi si ri-scansiona al più ogni
  // 2 s (un polling serrato affamerebbe le letture periodiche e congelerebbe la sonda di controllo).
  uint32_t owScanMs_ = 0;
  String owProbesCache_ = "[]";

  // Export CSV a pezzi (3.14.0). Prima l'intero file (fino a 50.000 righe) partiva in una sola
  // chiamata e la regolazione restava ferma per tutto il download, col failsafe rinviato a
  // mano. Ora l'handler manda solo le intestazioni e loop() scrive un pezzo per giro, e solo
  // quando il buffer TCP ha posto: un client lento non blocca nulla.
  struct CsvExport {
    bool active = false;
    WiFiClient client;
    DataLog::Cursor cur;
    uint32_t rows = 0;
    uint32_t lastMs = 0;  // ultimo pezzo scritto: oltre 30 s fermo, si chiude
    bool plato = false;
  } csv_;
  void csvPump();
  void csvEnd(const __FlashStringHelper* trailer);
};
