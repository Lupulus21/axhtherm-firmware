// AxHTherm 2.5 — firmware riscritto da zero. Nessun cloud: controllo, configurazione
// e monitoraggio locali. Vedi docs/ARCHITETTURA.md.
#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>

#include "hardware_map.h"
#include "hal/Mcp23008.h"
#include "hal/OutputBus.h"
#include "hal/ProbeBus.h"
#include "hal/RtcClock.h"
#include "svc/AppConfig.h"
#include "svc/AppStatus.h"
#include "svc/ChamberCtx.h"
#include "svc/ConfigStore.h"
#include "svc/StateStore.h"
#include "svc/ProfileStore.h"
#include "svc/CalibStore.h"
#include "svc/DeviceRegistry.h"
#include "svc/BootLog.h"
#include "svc/DataLog.h"
#include "svc/TimeService.h"
#include "svc/NetworkManager.h"
#include "svc/OledUi.h"
#include "svc/MqttSvc.h"
#include "svc/WebApi.h"
#include "ax/ProfileEngine.h"
#include "ax/ControlPipeline.h"
#include "ax/GravityTracker.h"
#include "ax/SafetyMonitor.h"

// Le camere di fermentazione. A N=1 il comportamento e' identico alla v2.23.2.
static ChamberCtx chambers[AX_N_CHAMBERS];

static Mcp23008 mcp;
static OutputBus outputs;
static ProbeBus probes;
static RtcClock rtc;
static TimeService timeSvc;
static BootLog bootLog;
static bool bootStamped = false;
static NetworkManager net;
static ConfigStore cfgStore;
static AppConfig cfg;
static StateStore stateStore;
static PersistState pstate;
static ProfileStore profStore;
static CalibStore calibStore;
static DeviceRegistry devices;
static DataLog dataLog;
static OledUi oled;
static MqttSvc mqtt;
static WebApi web;
// Un densimetro per camera (D7): la densita' e' una proprieta' del FERMENTATORE, non
// del dispositivo. 44 B ciascuno.
static ax::GravityTracker gravity[AX_N_CHAMBERS];
static ax::GravityTracker gravityAny;   // ultima lettura di CHIUNQUE: serve alla calibrazione

static String lastGravityDevice;  // nome del densimetro dell'ultima lettura
static bool batteryLow[AX_N_CHAMBERS] = {};  // per camera: e' la batteria DI QUEL densimetro
static bool fsOk = false;
static bool persistFault = false;  // ultima scrittura /state.json fallita
static bool logFault = false;      // ultima append al registro fallita (flag SEPARATO: un
                                   // checkpoint di stato riuscito non deve mascherare un
                                   // guasto persistente di scrittura del registro)
static bool bootStorageFault = false;  // al boot /state.json o /profile.json ESISTE ma illeggibile
static bool factoryResetPending = false;  // finestra fra cancellazione e riavvio: niente persist
static uint32_t rebootAtMs = 0;
static bool rebootPending = false;  // flag esplicito: 0 non è più sentinella (wrap di millis)
static uint32_t lastCtlMs = 0, lastCkptMs = 0, lastLogMs = 0, lastHeapMs = 0;

// ---------- failsafe su timer hardware ----------
// Il web server gira DENTRO questo loop cooperativo, e il framework legge la rete con
// attese che non hanno un tetto: gli header con readStringUntil (timeout PER BYTE) e il
// corpo multipart con `while(!client.available() && client.connected()) yield();`.
// Quelle attese chiamano yield(), che nutre sia il soft-WDT sia il watchdog hardware:
// nessun riavvio salvifico. Con il loop fermo i rele' restano LATCHATI nell'ultimo stato,
// perche' writeRel e' un digitalWrite senza alcun failsafe. Su una cotta non presidiata
// significa compressore o riscaldante in marcia per ore.
// Questo timer e' indipendente dal loop: se il tick di controllo non gira da FAILSAFE_SEC
// spegne i rele' su GPIO DIRETTI scrivendo il registro di clear.
//
// LIMITE, da conoscere: copre REL1/REL2/REL3 (GPIO 12/13/15). I rele' 4-8 passano dal
// MCP23008 via I2C e non sono comandabili da un'ISR. E' una ragione in piu' per
// l'allocazione raccomandata in docs/COLLAUDO_2CAMERE.md, che tiene ENTRAMBI i
// compressori su GPIO diretti.
// NB: una futura analogWrite()/tone() si prenderebbe timer1 e staccherebbe questa ISR.
// Non e' un guasto silenzioso: la verifica di vitalita' qui sotto se ne accorge entro
// ~10 s e alza l'allarme "failsafe non attivo". E' il motivo per cui quella verifica
// esiste — un dispositivo di sicurezza deve dire quando smette di funzionare.
static constexpr uint32_t kFailsafeHz = 5;
static constexpr uint32_t kFailsafeSec = 60;   // ampiamente oltre un'OTA normale
static volatile uint16_t failsafeIsrDaTick = 0;
static volatile bool failsafeScattato = false;
static bool failsafeDaSegnalare = false;  // resta alzato finche' non si riarma
static bool failsafeInerte = false;       // l'ISR non sta girando: la protezione NON c'e'

// Rinvia il failsafe da un percorso lungo ma LEGITTIMO che sta facendo progresso.
// Va chiamata solo da lavoro che avanza davvero (una riga di export scritta), mai da
// un'attesa: e' esattamente cio' che distingue un export dal blocco che il failsafe deve
// intercettare, dove il loop e' parcheggiato e non avanza.
void failsafeKick() { failsafeIsrDaTick = 0; }

// Nessuna chiamata di funzione qui dentro: tutto cio' che sta in flash puo' non essere
// raggiungibile da un'ISR. Si conta il numero di interruzioni invece di leggere millis().
static void IRAM_ATTR failsafeIsr() {
  if (failsafeIsrDaTick < 0xFFFF) failsafeIsrDaTick++;
  if (failsafeIsrDaTick >= kFailsafeHz * kFailsafeSec) {
    GPOC = (1u << PIN_REL1) | (1u << PIN_REL2) | (1u << PIN_REL3);
    failsafeScattato = true;
  }
}
static uint32_t lastCleanupDay = 0;

// ---------- persistenza ----------

static void persistNow() {
  // Durante la finestra del factory reset (fra cancellazione dei file e riavvio) NESSUNA
  // scrittura deve ricreare /state.json: senza, un cambio relè / comando MQTT / lettura
  // iSpindel in quel secondo farebbe rinascere il dispositivo armato con la vecchia cotta.
  if (factoryResetPending) return;
  // Fotografa OGNI camera: e' il punto in cui lo stato vivo diventa durevole, e
  // dimenticarne una significherebbe perderne la cotta al prossimo riavvio.
  // batchName e ogGravity non si copiano qui: vivono gia' in pstate e sono scritti
  // dai loro percorsi (avvio profilo, lettura iSpindel).
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
    ChamberState& s = pstate.ch[i];
    s.engine = chambers[i].engine.snapshot();
    s.guard = chambers[i].pipeline.guard().persist();
    s.coolWasOn = chambers[i].pipeline.guard().isOn();
    s.coolRel = outputs.coolRel(i);
    s.heatRel = outputs.heatRel(i);
    s.armed = chambers[i].armed;
    s.coolEstCph = chambers[i].pipeline.coolEstimatorCph();
    s.heatEstCph = chambers[i].pipeline.heatEstimatorCph();
  }
  pstate.lastSeenEpoch = timeSvc.nowEpoch();
  pstate.epochsTrusted = timeSvc.trusted();
  pstate.failsafePending = failsafeDaSegnalare;
  // L'esito NON va ignorato: una scrittura fallita (flash piena/usura) lascerebbe stato
  // e realtà divergenti in silenzio, e al riavvio si tornerebbe a uno stato vecchio.
  // Lo si espone come allarme (vedi chambers[0].safety.storageFault). Un checkpoint riuscito lo pulisce.
  if (fsOk) persistFault = !stateStore.save(pstate);
}

// ---------- configurazione -> servizi ----------

static void applyConfigLive() {
  for (uint8_t i = 0; i < ax::kMaxProbes; i++)
    probes.configureProbe(i, cfg.probeId[i], cfg.probeOffset[i]);

  ax::ControlPipeline::Cfg pc;
  pc.heatUsePid = cfg.usePid;
  pc.predictive = cfg.predictive;
  pc.toleranceC = cfg.toleranceC;
  pc.minStateSec = cfg.minStateSec;
  pc.minSwitchSec = cfg.minSwitchSec;
  pc.compressorRestSec = (uint32_t)cfg.compressorRestMin * 60;
  pc.compressorMinOnSec = cfg.compressorMinOnSec;
  pc.pid.kp = cfg.pidKp;
  pc.pid.ki = cfg.pidKi;
  pc.pid.kd = cfg.pidKd;
  pc.pid.windowMs = cfg.pidWindowMs;
  pc.pid.fullPowerBelow = cfg.pidFullPowerBelow;
  pc.pid.cutAbove = cfg.pidCutAbove;
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
    chambers[i].pipeline.configure(pc);
    // Sfasamento della finestra PID per camera (R1): con AX_N_CHAMBERS=1 l'offset e' 0
    // e il comportamento resta identico. configure() non lo azzera (e' fuori da Cfg),
    // ma lo si ri-applica qui perche' sia derivabile da un unico punto.
    chambers[i].pipeline.setStaggerStepSec(ax::kCompressorStaggerSec);
    chambers[i].pipeline.setPhaseFraction(i, AX_N_CHAMBERS);
    chambers[i].engine.configure(cfg.toleranceC);
  }

  // La soglia di staleness e' di DISPOSITIVO; la correzione e' dello STRUMENTO, quindi
  // per camera. Una camera lasciata fuori da questo ciclo userebbe i default della classe
  // (staleSec_ 3600) in silenzio e senza allarme: il ciclo copre tutte, sempre.
  // Cambio di densimetro assegnato: la lettura ancora in pancia appartiene allo
  // strumento PRECEDENTE, con la sua taratura. Tenerla significherebbe confrontarla con
  // l'OG del nuovo (o applicarle la correzione sbagliata) fino al primo invio del nuovo
  // strumento. Si azzera il tracker: la camera torna semplicemente "senza dato", che e'
  // uno stato che il sistema gia' sa gestire.
  static String ultimoDensimetro[AX_N_CHAMBERS];
  static bool primaVolta = true;
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
    if (!primaVolta && !ultimoDensimetro[i].equalsIgnoreCase(cfg.gravityDevice[i])) {
      gravity[i] = ax::GravityTracker();
      Serial.printf("[ispindel] camera %u: densimetro cambiato (%s -> %s), lettura azzerata\n",
                    (unsigned)(i + 1),
                    ultimoDensimetro[i].length() ? ultimoDensimetro[i].c_str() : "(nessuno)",
                    cfg.gravityDevice[i].length() ? cfg.gravityDevice[i].c_str() : "(nessuno)");
    }
    ultimoDensimetro[i] = cfg.gravityDevice[i];
    gravity[i].configure((uint32_t)cfg.gravityStaleMin * 60, cfg.gravityCorrection[i]);
  }
  primaVolta = false;
  gravityAny.configure((uint32_t)cfg.gravityStaleMin * 60, 0.0f);  // grezzo, senza correzione
}

// ---------- comandi (web/mqtt/seriale) ----------

// Vero se ANCHE UNA SOLA camera e' armata. Il test manuale dei rele' resta vietato in
// quel caso: i canali sono una risorsa di dispositivo e un test sulla camera A puo'
// eccitare un rele' mentre la B sta fermentando. Scelta conservativa e deliberata.
static bool anyChamberArmed() {
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++)
    if (chambers[i].armed) return true;
  return false;
}

static bool armOutputs(uint8_t ch, uint8_t coolRel, uint8_t heatRel) {
  if (ch >= AX_N_CHAMBERS) return false;
  if (outputs.testMode()) return false;
  // Difesa in profondita': la stessa regola e' gia' validata dalla UI e dalla CLI, ma
  // questo e' il punto UNICO di armamento e non deve dipendere dalla diligenza dei
  // chiamanti (e' la forma di difetto che ProfileEngine::overrideSetpoint mostra:
  // nessun clamp proprio, protezione delegata a monte).
  if (coolRel > 8 || heatRel > 8) return false;
  if (coolRel && coolRel == heatRel) return false;  // stesso rele' per freddo e caldo
  // Due camere sullo stesso rele' significherebbe che il controllo di una comanda il
  // carico dell'altra: si combatterebbero sullo stesso attuatore, ogni tick.
  if (outputs.relayTakenByOther(ch, coolRel) || outputs.relayTakenByOther(ch, heatRel)) {
    Serial.printf("[out] RIFIUTATO: rele' gia' assegnato a un'altra camera\n");
    return false;
  }
  // Spegne le sole uscite DI QUESTA camera: un remap a caldo non deve lasciare
  // eccitato il vecchio rele', ma non deve nemmeno fermare la cotta dell'altra camera.
  outputs.chamberOff(ch);
  // allOff() ha aperto fisicamente il relè del compressore, ma la guardia non lo sa:
  // senza questo, un riarmo a caldo lo richiuderebbe entro ~1 s (corto ciclo) e un
  // arm subito dopo un test manuale lo riavvierebbe a riposo zero. Lo si assume appena
  // fermato: la guardia impone il riposo pieno prima di ripartire (conservativo).
  chambers[ch].pipeline.guard().assumeJustStopped(timeSvc.nowEpoch());
  outputs.setMapping(ch, coolRel, heatRel);
  chambers[ch].armed = true;
  failsafeDaSegnalare = false;  // riarmo esplicito: l'evento e' stato preso in carico
  chambers[ch].safety.notArmed = false;
  Serial.printf("[out] camera %u ARMATA: freddo=REL%u caldo=REL%u\n", ch + 1, coolRel, heatRel);
  return true;
}

// Disarmo di DISPOSITIVO: disarma ogni camera e garantisce che TUTTI gli 8 canali
// cadano, non solo quelli mappati. La differenza non e' teorica: in modalita' test
// `rele <n> 1` puo' eccitare un canale QUALSIASI (OutputBus::testRelay accetta 1..8),
// e ne' il comando seriale `disarma` ne' POST /api/outputs/disarm hanno un gate sulla
// modalita' test. Con il solo chamberOff() per camera quel canale resterebbe ECCITATO.
// "Disarma" e' l'azione che l'utente usa come fermo d'emergenza: deve spegnere tutto.
static void disarmAllOutputs();

static void disarmOutputs(uint8_t ch) {
  if (ch >= AX_N_CHAMBERS) return;
  // Anche il disarmo prende in carico l'evento: e' un'azione esplicita dell'operatore, e
  // legarla al solo RIARMO avrebbe costretto a re-immettere a mano la mappatura dei rele'
  // per far sparire un allarme.
  failsafeDaSegnalare = false;
  chambers[ch].armed = false;
  chambers[ch].safety.notArmed = true;
  outputs.chamberOff(ch);  // solo le uscite di questa camera
  Serial.printf("[out] camera %u DISARMATA: sue uscite spente\n", ch + 1);
}

static void disarmAllOutputs() {
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) disarmOutputs(i);
  // Rete di sicurezza finale: copre i canali non mappati che la modalita' test puo'
  // aver lasciato eccitati. A N=1 ripristina esattamente il comportamento della
  // v2.23.2, dove disarmOutputs chiamava allOff().
  outputs.allOff();
}

// Vero quando la camera non ha NESSUNA sorgente di densita' possibile: nessun nome
// assegnato e nessuna modalita' "accetta chiunque". E' diverso da "assegnato ma muto",
// che resta il caso per cui esiste il timeout degli step Gravity.
// La camera 0 col campo vuoto e' in "accetta chiunque" (comportamento storico), quindi
// NON e' mai senza sorgente: a AX_N_CHAMBERS=1 questa funzione ritorna sempre false e
// il comportamento resta identico alla v2.23.2.
static bool densimetroNonAssegnato(uint8_t ch) {
  return ch > 0 && cfg.gravityDevice[ch].length() == 0;
}

// Avvio profilo: profileId 0 = step attivi correnti; >0 = carica dalla libreria
// (che diventa la copia attiva). Ritorna 0=ok, 404=profilo inesistente, 400=senza step.
static int startProfileCmd(uint8_t ch, int profileId, const String& batch, uint32_t delayedAt) {
  if (ch >= AX_N_CHAMBERS) return 404;
  if (profileId > 0) {
    // static: niente array sullo stack in fondo alla catena dei handler web
    // (esecuzione single-thread: nessun problema di rientranza).
    static ax::StepDef steps[ax::kMaxSteps];
    uint8_t n = 0;
    String name;
    if (!profStore.load((uint8_t)profileId, &name, steps, &n) || n == 0) return 404;
    // Validazione PRIMA di toccare qualsiasi cosa: un profilo senza step abilitati
    // non deve sovrascrivere né la copia attiva né gli step del motore.
    bool hasEnabled = false;
    for (uint8_t i = 0; i < n && !hasEnabled; i++) hasEnabled = steps[i].enabled;
    if (!hasEnabled) return 400;
    // Step a densita' su una camera senza densimetro: si rifiuta QUI, prima di
    // saveActive e setSteps, per la stessa ragione del controllo sopra — un profilo che
    // non puo' partire non deve lasciare dietro di se' una copia attiva su flash e un
    // motore caricato, che poi mostrerebbero un profilo inavviabile.
    if (densimetroNonAssegnato(ch))
      for (uint8_t i = 0; i < n; i++)
        if (steps[i].enabled && steps[i].type == ax::StepType::Gravity) {
          Serial.printf("[profilo] camera %u: step %u a densita' ma nessun densimetro "
                        "assegnato: avvio rifiutato\n", (unsigned)(ch + 1), (unsigned)(i + 1));
          return 409;
        }
    // Prima la copia attiva su flash, poi il motore: se la scrittura fallisce non si
    // parte con una ripresa post-blackout che userebbe gli step vecchi.
    if (!profStore.saveActive(ch, name, steps, n)) return 500;
    chambers[ch].engine.setSteps(steps, n);
    chambers[ch].activeProfileName = name;
  }
  // Stesso rifiuto per il ramo profileId == 0 (riavvio degli step ATTIVI correnti), dove
  // non c'e' nulla da caricare e quindi nulla da validare prima: qui si guarda il motore.
  // E' l'unico momento in cui l'utente e' presente, giorni prima del danno; senza questo
  // la camera resterebbe ferma sul primo step a densita' (per progetto, vedi
  // TickInput::gravitySourceMissing) e lo scoprirebbe solo dall'allarme.
  if (profileId == 0 && densimetroNonAssegnato(ch)) {
    const ax::StepDef* attivi = chambers[ch].engine.steps();
    if (attivi)
      for (uint8_t i = 0; i < ax::kMaxSteps; i++)
        if (attivi[i].enabled && attivi[i].type == ax::StepType::Gravity) {
          Serial.printf("[profilo] camera %u: step %u a densita' ma nessun densimetro "
                        "assegnato: avvio rifiutato\n", (unsigned)(ch + 1), (unsigned)(i + 1));
          return 409;
        }
  }
  ProbeBus::Reading ctl = probes.reading(chambers[ch].probeControl);
  if (!chambers[ch].engine.startProfile(timeSvc.nowEpoch(), ctl.c, ctl.valid, delayedAt)) return 400;
  pstate.ch[ch].batchName = batch;
  pstate.ch[ch].ogGravity = 0;  // nuova cotta: la densità iniziale verrà ricatturata
  batteryLow[ch] = false;  // per camera: avviare la camera B non deve spegnere l'allarme di A
  persistNow();
  Serial.printf("[profilo] avviato \"%s\" (cotta \"%s\")\n", chambers[ch].activeProfileName.c_str(),
                batch.c_str());
  return 0;
}

static void onIspindel(float g, float t, float batt, float angle, int rssi,
                       uint32_t devId, const String& devName) {
  lastGravityDevice = devName;
  // Memorizza lo strumento: la scelta del densimetro attivo si fa da un elenco,
  // non ricordando a memoria un nome che sta solo dentro il tubo.
  // seen() aggiorna SEMPRE il registro in RAM; la scrittura su flash è invece limitata a
  // una ogni 60 s (un attore LAN che invia ID sempre nuovi forzerebbe altrimenti una
  // scrittura a ogni richiesta: usura). Niente scritture nella finestra del factory reset.
  uint8_t densPrima = devices.count();
  bool densimetroNuovo = devices.seen(devName, devId, timeSvc.nowEpoch());
  // Un densimetro genuino che riempie uno slot libero (i primi kMax) va persistito SUBITO:
  // è raro e legittimo, e perderlo fino alla ritrasmissione (~15 min) sarebbe un difetto.
  // Solo le EVICTION (registro pieno → sostituzione LRU), che un flood LAN di ID sempre
  // nuovi produce, vanno limitate a una scrittura ogni 60 s (usura flash).
  bool eviction = densimetroNuovo && densPrima >= DeviceRegistry::kMax;
  static uint32_t lastDevSaveMs = 0;
  if (densimetroNuovo && fsOk && !factoryResetPending &&
      (!eviction || lastDevSaveMs == 0 || millis() - lastDevSaveMs > 60000)) {
    lastDevSaveMs = millis();
    devices.save();
    Serial.printf("[ispindel] nuovo densimetro in elenco: %s (%u)\n", devName.c_str(), devId);
  }
  // La calibrazione deve poter raccogliere punti da entrambi gli strumenti, ANCHE da
  // uno non ancora assegnato ad alcuna camera: e' proprio quello che si sta tarando.
  // Resta quindi PRIMA di ogni instradamento, e fuori da qualunque ciclo sulle camere.
  gravityAny.update(g, batt, t, timeSvc.nowEpoch(), angle, rssi, devId);

  // --- Instradamento alla camera (D7) -------------------------------------------
  // Una lettura alimenta AL PIU' UNA camera, per costruzione: due fermentatori che
  // seguono lo stesso strumento sono il difetto che questa funzione esiste per
  // impedire. Le tre regole sono documentate accanto ad AppConfig::gravityDevice[];
  // a AX_N_CHAMBERS=1 si riducono esattamente al filtro della v2.23.2.
  // Confronto senza distinzione fra maiuscole e minuscole: "iSpindel_Giallo" e
  // "ispindel_giallo" devono valere lo stesso, altrimenti il filtro fallisce in silenzio.
  uint8_t camDensimetro = 0xFF;  // 0xFF = nessuna: la lettura non alimenta alcuna camera
  if (devName.length() == 0) {
    // Regola 1 — lettura ANONIMA. Passa sempre alla camera 0, come nella v2.23.2, dove
    // la clausola devName.length() la faceva sfuggire al filtro. NON e' un residuo: il
    // Tilt inoltrato da Home Assistant (docs/HOME_ASSISTANT.md §7) manda un payload
    // senza "name", e un instradamento per solo nome lo scarterebbe in silenzio.
    camDensimetro = 0;
  } else {
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++)
      if (cfg.gravityDevice[i].length() &&
          cfg.gravityDevice[i].equalsIgnoreCase(devName)) {
        camDensimetro = i;  // Regola 2 — rivendicato: quella camera, e nessun'altra.
        break;              // primo che rivendica: l'unicita' e' garantita qui, non altrove
      }
    // Regola 3 — nome non rivendicato: alla camera 0 solo se e' in "accetta chiunque"
    // (campo vuoto), che e' il comportamento storico a densimetro non configurato.
    // Le camere con indice > 0 e campo vuoto sono NON ASSEGNATE, non "accetta chiunque":
    // estendere loro la regola riprodurrebbe D7 in una forma che sembra corretta.
    if (camDensimetro == 0xFF && cfg.gravityDevice[0].length() == 0) camDensimetro = 0;
  }
  if (camDensimetro == 0xFF) {
    Serial.printf("[ispindel] %s ignorato: nessuna camera lo ha assegnato\n",
                  devName.c_str());
    return;
  }
  gravity[camDensimetro].update(g, batt, t, timeSvc.nowEpoch(), angle, rssi, devId);
  batteryLow[camDensimetro] = batt > 0.1f && batt < cfg.batteryAlertV;
  // Cattura automatica della densità iniziale (OG): prima lettura valida DOPO l'avvio
  // di un profilo (una vera cotta). NON in termostato: lì un cold-crash/test verrebbe
  // scambiato per OG; in quel caso l'utente la imposta a mano. Reset all'avvio profilo.
  // NON durante l'attesa di partenza ritardata: lo iSpindel potrebbe essere ancora
  // nel secchio di sanificante, e quell'OG avvelenerebbe l'attenuazione della cotta.
  // NON in pausa (chambers[0].engine.running() è vero anche in pausa: lì lo iSpindel può essere
  // ancora nel sanificante) e NON durante l'attesa di partenza ritardata — usando lo
  // stato LIVE del motore (delayedStartEpoch), non chambers[0].lastTick.waitingDelayedStart che
  // resta vecchio fino a 1 s dopo l'avvio e lascerebbe passare quella lettura.
  // La cattura alimenta la camera CHE HA RICEVUTO LA LETTURA (D7): prima era cablata
  // alla camera 0, perche' il densimetro era unico e di dispositivo.
  const uint8_t cog = camDensimetro;
  if (pstate.ch[cog].ogGravity <= 0.0f && chambers[cog].engine.running() &&
      !chambers[cog].engine.paused() &&
      chambers[cog].engine.snapshot().delayedStartEpoch == 0) {
    // Stesso clamp di plausibilita' della via manuale (WebApi::handleOg) e di /ispindel:
    // e' la SECONDA via d'ingresso dell'OG e finora non era validata. Una densita'
    // assurda — un refuso dello strumento, o una correzione fuori scala — verrebbe
    // persistita in /state.json e poi riemessa a ogni /api/status via String(float, 4),
    // che su questo core non ha protezione di lunghezza. Test di appartenenza: NaN-safe.
    const float og = gravity[cog].gravity();
    const bool plato = cfg.gravityUnit == "plato";
    // Si salta la sola CATTURA, senza uscire dalla funzione: la riga diagnostica in fondo
    // deve continuare a comparire (e' quella che documenta cosa ha trasmesso lo strumento).
    if (!(og >= 1.0f && og <= (plato ? 40.0f : 1.20f))) {
      Serial.printf("[ispindel] OG %.4f fuori range: NON catturata\n", og);
    } else {
      pstate.ch[cog].ogGravity = og;
      persistNow();
      Serial.printf("[ispindel] OG catturata (camera %u): %.4f\n",
                    (unsigned)(cog + 1), pstate.ch[cog].ogGravity);
    }
  }
  Serial.printf("[ispindel] %s(%u) gravity=%.4f temp=%.1f batt=%.2f angle=%.2f rssi=%d\n",
                devName.c_str(), devId, g, t, batt, angle, rssi);
}

static void setOgGravity(uint8_t ch, float og) {
  if (ch >= AX_N_CHAMBERS) return;
  pstate.ch[ch].ogGravity = og > 0.0f ? og : 0.0f;  // <=0 = azzera (ricattura automatica)
  persistNow();
}

static void factoryReset() {
  Serial.println(F("[sys] factory reset: cancello config/stato/profilo"));
  LittleFS.remove(ConfigStore::kPath);
  LittleFS.remove(StateStore::kPath);
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++)
    LittleFS.remove(ProfileStore::activePath(i));  // il profilo attivo di OGNI camera
  LittleFS.remove(CalibStore::kPath);
  LittleFS.remove(DeviceRegistry::kPath);
  for (uint8_t i = 1; i <= ProfileStore::kMaxProfiles; i++)
    LittleFS.remove(String("/profiles/p") + i + ".json");
  LittleFS.rmdir("/profiles");
  factoryResetPending = true;  // il persist pre-riavvio NON deve ricreare lo stato
  rebootAtMs = millis() + 1000;
  rebootPending = true;
}

static void scheduleReboot(uint32_t delayMs) {
  rebootAtMs = millis() + delayMs;
  rebootPending = true;
}

// ---------- stato per UI/OLED/MQTT ----------

static AppStatus buildStatus(ChamberCtx& cam) {
  AppStatus st;
  ProbeBus::Reading c = probes.reading(cam.probeControl);
  ProbeBus::Reading a1 = cam.auxCount > 0 ? probes.reading(cam.probeAux[0]) : ProbeBus::Reading{};
  ProbeBus::Reading a2 = cam.auxCount > 1 ? probes.reading(cam.probeAux[1]) : ProbeBus::Reading{};
  st.tControl = c.c;
  st.tControlValid = c.valid;
  st.tAux1 = a1.c;
  st.tAux1Valid = a1.valid;
  st.tAux2 = a2.c;
  st.tAux2Valid = a2.valid;
  st.hasSetpoint = cam.lastTick.hasSetpoint;
  st.setpoint = cam.lastTick.setpointC;
  st.heat = cam.lastPipe.heat;
  st.cool = cam.lastPipe.cool;
  st.armed = cam.armed;
  st.testMode = outputs.testMode();
  st.mode = (uint8_t)cam.engine.mode();
  st.paused = cam.engine.paused();
  st.completed = cam.engine.completed();
  st.waitingDelayedStart = cam.lastTick.waitingDelayedStart;
  st.stepIdx = cam.engine.stepIdx();      // indice ASSOLUTO (usato lato client per indicizzare)
  st.stepCount = cam.engine.stepCount();
  // Numerazione basata sugli step ABILITATI, per la SOLA visualizzazione (i disabilitati
  // non girano): status/OLED/MQTT/riquadro mostravano numeri diversi (assoluto vs abilitati).
  {
    const ax::StepDef* ps = cam.engine.steps();
    uint8_t enTot = 0, enIdx = 0;
    for (uint8_t i = 0; ps && i < cam.engine.stepCount(); i++) {
      if (!ps[i].enabled) continue;
      if (i < st.stepIdx) enIdx++;
      enTot++;
    }
    st.stepEnIdx = enIdx;
    st.stepEnCount = enTot;
  }
  st.stepEndsAt = cam.engine.stepEndsAtEpoch();
  st.profileName = cam.activeProfileName;
  st.batchName = pstate.ch[cam.idx].batchName;
  st.stepStartEpoch = cam.engine.stepStartEpoch();
  st.stepStartTemp = cam.engine.snapshot().stepStartTemp;
  st.gravityHas = gravity[cam.idx].hasValue();
  st.gravity = gravity[cam.idx].gravity();
  st.gravityBattery = gravity[cam.idx].battery();
  uint32_t now = timeSvc.nowEpoch();
  st.gravityAgeSec = gravity[cam.idx].ageSec(now);
  st.gravityStale = gravity[cam.idx].hasValue() && !gravity[cam.idx].isFresh(now);
  st.gravityOg = pstate.ch[cam.idx].ogGravity;
  st.gravityTemp = gravity[cam.idx].temperature();
  st.gravityAngle = gravity[cam.idx].angle();
  st.gravityRssi = gravity[cam.idx].rssi();
  st.profileStartEpoch = cam.engine.snapshot().profileStartEpoch;
  // Attenuazione apparente e ABV stimato (formule birrarie standard), quando c'è OG+SG.
  if (st.gravityHas && st.gravityOg > 0) {
    float og = st.gravityOg, sg = st.gravity;
    if (cfg.gravityUnit == "plato") {
      if (og > 0.01f) st.attenuation = (og - sg) / og * 100.0f;
      auto toSg = [](float p) { return 1.0f + p / (258.6f - p / 258.2f * 227.1f); };
      st.abv = (toSg(og) - toSg(sg)) * 131.25f;
    } else {
      if (og > 1.0f) st.attenuation = (og - sg) / (og - 1.0f) * 100.0f;
      st.abv = (og - sg) * 131.25f;
    }
    if (!isfinite(st.abv) || st.abv < 0) st.abv = 0;              // mai "nan"/"inf" in /api/status
    if (!isfinite(st.attenuation) || st.attenuation < 0) st.attenuation = 0;
  }
  // Una camera MAI CONFIGURATA non e' guasta: e' semplicemente non ancora allestita.
  // Senza questa distinzione, abilitando la seconda camera prima di montarne l'hardware si
  // avrebbe un allarme "sonda guasta" acceso per sempre — rumore che nasconde i guasti veri
  // e che addestra a ignorare la riga degli allarmi.
  // NB: si tace solo l'ALLARME. Il gate di sicurezza (SafetyMonitor::controlProbeFault ->
  // outputsAllowed) resta INTATTO: senza una sonda valida quella camera non attua nulla, e
  // in piu' ControlPipeline riceve comunque tempValid=false. Nessuna protezione indebolita.
  st.alarmProbe = cam.safety.controlProbeFault && probes.reading(cam.probeControl).configured;
  st.alarmConfig = cam.safety.configInvalid;
  st.alarmRtc = cam.safety.rtcUntrusted;
  // Un densimetro che smette di trasmettere è un guasto; un densimetro che non c'è
  // non lo è, PURCHÉ lo step abbia un limite di durata: senza, il profilo resterebbe
  // fermo per sempre e togliere l'allarme lo renderebbe invisibile.
  // hasValue() da solo non basta: è volatile e un riavvio lo azzera, facendo sparire
  // l'allarme proprio su un densimetro davvero guasto. L'OG della cotta è persistita.
  const ax::StepDef* passi = cam.engine.steps();
  const uint8_t iPasso = cam.engine.snapshot().stepIdx;
  const bool senzaLimite = passi && iPasso < ax::kMaxSteps &&
                           passi[iPasso].type == ax::StepType::Gravity &&
                           passi[iPasso].maxDurationSec == 0;
  const bool densimetroVisto = gravity[cam.idx].hasValue() || pstate.ch[cam.idx].ogGravity > 0.0f;
  // NB: uno step a densità senza timeout, con dato fresco ma condizione mai soddisfatta,
  // può restare appeso. NON lo si segnala con una soglia di tempo fissa: il dominio dichiara
  // fermentazioni/lagering fino a 6 mesi, quindi qualunque soglia darebbe falsi allarmi su
  // cotte lunghe legittime. La distinzione "appeso vs in attesa" richiede il tracciamento
  // del progresso della densità (lavoro futuro). La visibilità resta: profileNoLimit (sotto)
  // avverte che lo step non ha uscita automatica, e la UI mostra densità vs target.
  // Terza via all'allarme: la camera non ha densimetro e sta eseguendo uno step a
  // densita'. Senza questa clausola resterebbe muta — densimetroVisto e' falso (nessuna
  // lettura, nessuna OG) e senzaLimite pure (tutti i profili reali hanno un timeout) —
  // e la camera aspetterebbe per sempre senza dirlo a nessuno.
  const bool sorgenteAssente = densimetroNonAssegnato(cam.idx) &&
                               passi && iPasso < ax::kMaxSteps &&
                               passi[iPasso].type == ax::StepType::Gravity;
  st.alarmGravity = cam.safety.gravityStale &&
                    (densimetroVisto || senzaLimite || sorgenteAssente);
  st.profileWaitsGravity = cam.safety.gravityStale;
  st.profileNoLimit = senzaLimite;
  st.alarmBattery = batteryLow[cam.idx];
  st.alarmOutputs = cam.safety.outputsFault;
  st.alarmRampStall = cam.safety.rampStalled;
  st.alarmStorage = cam.safety.storageFault;
  st.alarmThermal = cam.safety.thermalDeviation;
  st.alarmProbeSuspect = cam.safety.controlProbeSuspect;
  st.alarmFailsafe = failsafeDaSegnalare;
  st.alarmFailsafeInerte = failsafeInerte;
  // Più densimetri hanno trasmesso ma nessuno è scelto come attivo: le letture si
  // alternano fra fermentatori diversi (avanzano lo step o avvelenano l'OG della cotta
  // sbagliata). Reso visibile: il rimedio è scegliere il densimetro in calibrazione.
  // Ambiguita' di instradamento: esiste SOLO se la camera 0 e' in "accetta chiunque"
  // mentre trasmettono piu' strumenti. Una camera con indice > 0 senza densimetro non e'
  // ambigua — non riceve nulla — e segnalarla qui darebbe un allarme permanente col testo
  // sbagliato (la sua mancanza la segnala alarmGravity, sotto).
  st.alarmMultiGravity = cfg.gravityDevice[0].length() == 0 && devices.count() > 1;
  st.ip = net.ipString();
  st.staUp = net.staConnected();
  st.apUp = net.apActive();
  st.rssi = net.rssi();
  st.nowEpoch = now;
  st.timeTrusted = timeSvc.trusted();
  st.coolBlockedSec = cam.lastPipe.coolBlockedSec;
  st.pidDuty = cam.lastPipe.pidDuty;
  st.predCoolCph = cam.pipeline.coolEstimatorCph();
  st.predHeatCph = cam.pipeline.heatEstimatorCph();
  st.freeHeap = ESP.getFreeHeap();
  return st;
}

// ---------- tick di controllo (1 s) ----------

static void controlTick(ChamberCtx& cam, uint32_t nowMs, uint32_t dtMs) {
  uint32_t now = timeSvc.nowEpoch();
  // Tempo REALE trascorso dal tick precedente, non un 1 s assunto: se il loop rallenta
  // (handler web, riconnessione MQTT, flash) PID e stimatore predittivo lavorerebbero su
  // un tempo falso. Clamp a 10 s per non far esplodere integrale/derivata dopo uno stallo.
  float dtSec = dtMs / 1000.0f;
  if (dtSec < 0.001f) dtSec = 0.001f;
  if (dtSec > 10.0f) dtSec = 10.0f;
  ProbeBus::Reading ctl = probes.reading(cam.probeControl);

  cam.safety.controlProbeFault = !ctl.valid;
  cam.safety.configInvalid = !cfg.valid;
  cam.safety.rtcUntrusted = !timeSvc.trusted();
  // Il guasto del MCP toglie il consenso SOLO alla camera che dipende davvero da lui.
  cam.safety.outputsFault = outputs.mcpFault() && outputs.mcpNeeded(cam.idx);
  cam.safety.notArmed = !cam.armed;
  cam.safety.storageFault = persistFault || logFault || bootStorageFault;

  // Ripresa con orologio inaffidabile: al primo tick con lettura valida ri-àncora la rampa
  // in corso alla temperatura reale (continua da dove la birra si trova, non dall'origine né
  // dal target — vedi il blocco di ripresa in setup). Solo ora la temperatura è disponibile.
  if (cam.rebaseRampPending && ctl.valid) {
    cam.engine.reanchorCurrentRamp(now, ctl.c);
    cam.rebaseRampPending = false;
    persistNow();  // rende durevole subito lo stato ri-ancorato (rampReanchored): sopravvive a un reboot
  }

  ax::ProfileEngine::TickInput in;
  in.nowEpoch = now;
  in.tempC = ctl.c;
  in.tempValid = ctl.valid;
  in.gravity = gravity[cam.idx].gravity();
  in.gravityValid = gravity[cam.idx].hasValue();
  in.gravityFresh = gravity[cam.idx].isFresh(now);
  // Senza sorgente, il timeout degli step Gravity li farebbe avanzare alla cieca dopo
  // giorni, senza aver mai misurato: nei profili reali il passo successivo e' il cold
  // crash. Il flag lo disabilita; la condizione e' resa visibile da alarmGravity.
  in.gravitySourceMissing = densimetroNonAssegnato(cam.idx);
  // Attenuazione apparente per gli step "attesa % attenuazione": il calcolo dipende
  // dall'unità e dall'OG catturata, quindi vive qui e non nel motore.
  if (in.gravityValid && pstate.ch[cam.idx].ogGravity > 0) {
    float og = pstate.ch[cam.idx].ogGravity, sg = in.gravity;
    if (cfg.gravityUnit == "plato") {
      if (og > 0.01f) { in.attenuationPct = (og - sg) / og * 100.0f; in.attenuationValid = true; }
    } else if (og > 1.0f) {
      in.attenuationPct = (og - sg) / (og - 1.0f) * 100.0f;
      in.attenuationValid = true;
    }
    if (in.attenuationPct < 0) in.attenuationPct = 0;
  }
  cam.lastTick = cam.engine.tick(in);
  cam.safety.gravityStale = cam.lastTick.waitingGravity;
  cam.safety.rampStalled = cam.lastTick.waitingRampStall;

  // Freeze-guard antigelo (-1) e anti-surriscaldamento (40) all'ATTUAZIONE, non solo agli
  // ingressi: unico punto di strozzatura che copre OGNI sorgente del setpoint — termostato,
  // override, MQTT climate_mode=auto (setpoint memorizzato grezzo), e ripristino al boot di
  // un valore persistito da un firmware pre-guardia. Sotto -1 l'overshoot del frigo porta
  // la birra al congelamento; sopra 40 il riscaldatore la cuoce.
  float spAtt = cam.lastTick.setpointC;
  // Test in forma di appartenenza: è falso anche per NaN, così un eventuale setpoint NaN non passa
  // inalterato (con NaN i due confronti diretti < e > sarebbero entrambi falsi e latcherebbe i relè).
  // Difesa autosufficiente di questo unico choke point, indipendente dalle validazioni a monte.
  if (!(spAtt >= -1.0f)) spAtt = -1.0f;
  if (spAtt > 40.0f) spAtt = 40.0f;
  cam.lastPipe = cam.pipeline.tick(now, nowMs, cam.lastTick.hasSetpoint, spAtt, ctl.valid,
                           ctl.c, cam.safety.outputsAllowed(), dtSec, cam.lastTick.allowHeat,
                           cam.lastTick.allowCool);
  outputs.applyAutomation(cam.idx, cam.lastPipe.heat, cam.lastPipe.cool);

  // Allarme di fuga/scostamento termico basato sul PROGRESSO (non un gate binario): con setpoint
  // STABILE (le rampe sono escluse — il loro lag è atteso) e uscite consentite si segue lo scarto
  // |T-setpoint|. Finché resta oltre 5 °C senza rientrare di almeno 0,5 °C dal suo PICCO, si conta
  // il tempo: oltre 3 ore -> l'impianto non ce la fa (attuatore guasto, porta aperta,
  // sottodimensionato). È SOLO un allarme: non blocca mai gli output (SafetyMonitor::outputsAllowed).
  // Reso visibile su tutti i canali.
  //
  // Il riferimento del progresso è il MINIMO scarto raggiunto DA QUANDO la finestra si è aperta
  // (cam.devBestGap, ri-armato allo scarto d'ingresso ogni volta che si rientra sopra i 5 °C), NON il
  // minimo storico dall'inizio dello step. Col minimo storico, dopo essere arrivati in banda una
  // volta (scarto ~0) un'escursione successiva non poteva più battere quel minimo → falso allarme.
  // Col minimo-da-apertura: durante una discesa reale (pull-down/cold-crash) lo scarto tocca di
  // continuo nuovi minimi e il timer si azzera → niente allarme; se poi si ASSESTA sopra 5 °C
  // (impianto sottodimensionato) smette di toccare nuovi minimi → il timer matura → allarme. Copre
  // così sia l'impianto che non arriva MAI sia quello che arriva a metà e si pianta.
  // NOTA (scelta di progetto, non un falso positivo): un'escursione che sale e poi rientra solo in
  // PARTE, restando >5 °C e NETTAMENTE peggiore di 3 h prima, allarma — ed è corretto: la birra è a
  // lungo troppo fuori banda e non sta chiudendo. L'allarme NON scatta invece durante un rientro
  // che tocca nuovi minimi (progresso reale ≥0,5 °C), anche lento.
  // RESIDUO NOTO (documentato): le rampe a TEMPO restano NON coperte. Misurare lo scarto dal
  // setpoint IN MOVIMENTO di una rampa confonde "impianto guasto" con "rampa più veloce di quanto
  // l'impianto regga"; servirebbe un rilevatore sul TREND della temperatura, non sullo scarto.
  // Il timer di stallo è una DURATA accumulata (cam.devStallMs += dt sui soli tick valutabili con
  // scarto fermo), non "now - istante di partenza": così un fault transitorio della sonda si
  // gestisce semplicemente NON accumulando (congela), senza buttare il tempo già maturato, ed è
  // immune al rollover di millis().
  if (!cam.lastTick.hasSetpoint || fabsf(spAtt - cam.devSp) > 0.1f) {
    cam.devSp = spAtt; cam.devStallMs = 0; cam.devStalling = false;  // nuovo obiettivo: si riparte
  }
  const ax::StepDef* psDev = cam.engine.steps();
  const uint8_t iDev = cam.engine.stepIdx();
  const bool rampaInCorso = cam.engine.mode() == ax::RunMode::Profile && !cam.engine.completed() &&
                            psDev && iDev < ax::kMaxSteps &&
                            (psDev[iDev].type == ax::StepType::RampTime ||
                             psDev[iDev].type == ax::StepType::RampRate);
  // Incremento del timer = tempo REALE del tick, già clampato a 10 s (come PID/predittivo): dopo
  // uno stallo del loop non si conta come stallo termico l'intera pausa.
  const uint32_t dStallMs = (uint32_t)(dtSec * 1000.0f);
  bool thermalAlarm = false;
  const bool contesto = cam.lastTick.hasSetpoint && !rampaInCorso;  // c'è un obiettivo fisso da tenere
  if (contesto && ctl.valid && cam.safety.outputsAllowed()) {
    const float gap = fabsf(ctl.c - spAtt);
    // Solo se l'attuatore che servirebbe a chiudere lo scarto è CONSENTITO dallo step: uno
    // step free-rise (allowHeat=false) o crash naturale (allowCool=false) tiene di proposito
    // la temperatura fuori banda nella direzione vietata — non è un guasto da segnalare.
    const bool attuatoreConsentito = (ctl.c < spAtt && cam.lastTick.allowHeat) ||
                                     (ctl.c > spAtt && cam.lastTick.allowCool);
    if (gap <= 5.0f || !attuatoreConsentito) {
      cam.devStallMs = 0; cam.devStalling = false;  // in banda o attuatore vietato: nessuno stallo
    } else if (!cam.devStalling) {
      cam.devStalling = true; cam.devBestGap = gap; cam.devStallMs = 0;  // ingresso finestra: riferimento = scarto attuale
    } else if (gap + 0.5f < cam.devBestGap) {
      cam.devBestGap = gap; cam.devStallMs = 0;  // nuovo minimo: progresso reale, si riparte
    } else {
      // Si accumula solo FINO alla soglia: dopo lo scatto continuare non serve, ed evita il wrap
      // dell'uint32 (~49,7 giorni di stallo continuo) che farebbe rientrare l'allarme per ~3 h su
      // un guasto reale non presidiato.
      if (cam.devStallMs <= 3UL * 3600UL * 1000UL) cam.devStallMs += dStallMs;  // nessun progresso: accumula
      if (cam.devStallMs > 3UL * 3600UL * 1000UL) thermalAlarm = true;
    }
  } else if (contesto && !ctl.valid && cam.devStalling) {
    // Eravamo in stallo e la sonda di controllo è momentaneamente invalida (outputsAllowed() è
    // false PROPRIO per il controlProbeFault): CONGELA — non accumulare e non azzerare. Uno
    // sfarfallio (~15 s ogni tanto) non deve buttare il tempo di stallo e far ripartire da capo le
    // 3 h, o una sonda di controllo marginale impedirebbe per sempre la maturazione dell'allarme.
    // Un fault PROLUNGATO è comunque coperto dal suo allarme controlProbeFault.
  } else {
    cam.devStallMs = 0; cam.devStalling = false;  // nuovo obiettivo / rampa / disarmato: azzera
  }
  cam.safety.thermalDeviation = thermalAlarm;

  // Cross-check di plausibilità della sonda di CONTROLLO (SOLO allarme, NON taglia le uscite): se è
  // valida ma diverge dalla ausiliaria PIÙ VICINA di oltre 15 °C per 30 min accumulati, è probabile
  // che stia leggendo un valore plausibile-ma-sbagliato (contatto perso, sacca fredda). Serve
  // "diverge da TUTTE le ausiliarie" (=minDiff) perché le ausiliarie misurano punti diversi
  // (camera/ambiente) e una sola può legittimamente divergere (pull-down camera). Richiede >= 2
  // ausiliarie valide: con una sola "da tutte" degenera in "dall'unica" → falso allarme.
  // LIMITE NOTO (fondamentale del confronto fra sonde): se la sonda di controllo scivola nella camera
  // e legge ~lo stesso valore della sonda camera, minDiff resta piccolo e questo allarme NON scatta.
  // Rimedio reale per quel caso: fissare bene la sonda del mosto nel pozzetto. Il cross-check copre gli
  // altri casi (lettura grossolanamente fuori da tutte le altre).
  bool probeSuspect = false;
  if (ctl.valid) {
    float minDiff = 1e9f;
    uint8_t nAux = 0;
    // Solo le ausiliarie di QUESTA camera: quelle dell'altra misurano un frigo diverso
    // e un confronto con loro non direbbe nulla sulla plausibilita' di questo mosto.
    for (uint8_t k = 0; k < cam.auxCount; k++) {
      ProbeBus::Reading a = probes.reading(cam.probeAux[k]);
      if (!a.valid) continue;
      nAux++;
      float d = fabsf(ctl.c - a.c);
      if (d < minDiff) minDiff = d;  // scarto dalla ausiliaria PIÙ VICINA
    }
    if (nAux < 2) {
      // meno di 2 ausiliarie valide: non valutabile. Si CONGELA (non si azzera), coerente col caso
      // "sonda di controllo invalida": uno sfarfallio di un'ausiliaria durante un guasto reale non
      // deve buttare il tempo maturato e ritardare l'allarme.
    } else if (minDiff > 15.0f) {  // lontana da OGNI ausiliaria → sonda di controllo sospetta
      if (cam.probeSuspectMs <= 30UL * 60UL * 1000UL) cam.probeSuspectMs += dStallMs;  // cap: niente wrap uint32
      if (cam.probeSuspectMs > 30UL * 60UL * 1000UL) probeSuspect = true;
    } else {
      cam.probeSuspectMs = 0;  // mosto in accordo con almeno una ausiliaria: genuinamente non sospetto
    }
  }
  // sonda di controllo invalida: cam.probeSuspectMs CONGELATO (non azzerato), come l'allarme termico —
  // uno sfarfallio non deve buttare il tempo maturato; l'invalidità vera è già in controlProbeFault.
  cam.safety.controlProbeSuspect = probeSuspect;

  if (cam.lastTick.stepChanged || cam.lastTick.profileCompleted || cam.lastPipe.cool != cam.prevCool) {
    cam.prevCool = cam.lastPipe.cool;
    persistNow();  // transizione: indice step / epoch compressore su flash
    if (cam.lastTick.stepChanged)
      Serial.printf("[profilo] step %u/%u\n", cam.engine.stepIdx() + 1, cam.engine.stepCount());
  }
}

// ---------- campione log (ogni logSampleMin) ----------

static void logSample() {
  if (!fsOk) return;
  // Un campione per camera, ciascuno nel file della sua camera.
  // L'epoch e' di dispositivo (stesso orologio per tutte): si conserva per il cleanup,
  // che gira una volta sola a valle del ciclo.
  uint32_t epochCampione = 0;
  logFault = false;  // ri-valutato a ogni campionamento: un guasto risolto deve rientrare
  for (uint8_t ci = 0; ci < AX_N_CHAMBERS; ci++) {
  const AppStatus& st = chambers[ci].curStatus;
  DataLog::Sample s;
  s.epoch = st.nowEpoch;
  s.tControl = DataLog::encodeTemp(st.tControl, st.tControlValid);
  s.tAux1 = DataLog::encodeTemp(st.tAux1, st.tAux1Valid);
  s.tAux2 = DataLog::encodeTemp(st.tAux2, st.tAux2Valid);
  s.setpoint = DataLog::encodeTemp(st.setpoint, st.hasSetpoint);
  s.flags = (st.heat ? 0x01 : 0) | (st.cool ? 0x02 : 0) |
            (st.armed ? 0x04 : 0) |
            ((st.alarmProbe || st.alarmConfig || st.alarmRtc ||
              st.alarmGravity || st.alarmRampStall || st.alarmStorage ||
              st.alarmThermal || st.alarmBattery || st.alarmMultiGravity ||
              st.alarmProbeSuspect || st.alarmOutputs) ? 0x08 : 0) |
            (st.hasSetpoint ? 0x10 : 0);
  // SG x10000 (1.0105 -> 10105); in °Plato x100 (12.5 -> 1250): entrambi in uint16.
  float gscale = cfg.gravityUnit == "plato" ? 100.0f : 10000.0f;
  if (st.gravityHas) {
    // Clamp a [1, 65535]: una densità °Plato negativa/nulla (birra molto attenuata,
    // sidro/idromele) altrimenti traboccherebbe in uint16 (es. -0,3 °P -> 65506) o
    // verrebbe letta come "mancante" (0 = sentinella). 1 = minimo rappresentabile.
    long g = lroundf(st.gravity * gscale);
    if (g < 1) g = 1;
    if (g > 65535) g = 65535;
    s.gravity = (uint16_t)g;
  } else {
    s.gravity = 0;
  }
  s.battery = st.gravityHas ? (uint8_t)lroundf(st.gravityBattery * 20.0f) : 0;
  // Un solo flag di guasto per il registro: basta che una camera fallisca perche' la
  // scrittura sia da segnalare.
  if (!dataLog.append(s, ci)) logFault = true;
  epochCampione = s.epoch;
  }  // fine del ciclo sulle camere

  uint32_t day = epochCampione / 86400;
  static uint32_t lastCleanupMs = 0;
  uint32_t nowMs = millis();
  // A cambio giorno (retention per giorni) O almeno ogni ora. Il trigger orario è essenziale con
  // orologio inaffidabile: lì 'day' resta ~0 a lungo, il cambio-giorno non scatta e senza questo il
  // tetto di dimensione in cleanup() non girerebbe mai → /log/d0.bin crescerebbe illimitato.
  if (day != lastCleanupDay || (uint32_t)(nowMs - lastCleanupMs) >= 3600000UL) {
    lastCleanupDay = day;
    lastCleanupMs = nowMs;
    dataLog.cleanup(cfg.logRetentionDays, epochCampione);
  }
}

// ---------- CLI seriale (diagnostica milestone 1) ----------

static void serialHelp() {
  Serial.println(F("Comandi: aiuto | stato | sonde | scan i2c | scan ow | rele <1..8> <0|1> | "
                   "test off | arma [camera] <coolRel> <heatRel> | disarma [camera] | ora <epochUTC> | "
                   "heap | riavvia"));
}

static void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  if (line == "aiuto" || line == "help") {
    serialHelp();
  } else if (line == "stato") {
    Serial.printf("T=%.2f(%d) sp=%.2f(%d) heat=%d cool=%d armato=%d modo=%u step=%u/%u "
                  "epoch=%u trusted=%d heap=%u\n",
                  chambers[0].curStatus.tControl, chambers[0].curStatus.tControlValid, chambers[0].curStatus.setpoint,
                  chambers[0].curStatus.hasSetpoint, chambers[0].curStatus.heat, chambers[0].curStatus.cool, chambers[0].curStatus.armed,
                  chambers[0].curStatus.mode, chambers[0].curStatus.stepIdx + 1, chambers[0].curStatus.stepCount,
                  chambers[0].curStatus.nowEpoch, chambers[0].curStatus.timeTrusted, ESP.getFreeHeap());
  } else if (line == "sonde") {
    // Diagnostica completa delle tre sonde dalla seriale: serve quando la pagina web
    // non è raggiungibile o la password non è a portata di mano, ed è l'unico modo di
    // vedere INSIEME l'indirizzo configurato e quello che sta davvero sul bus.
    String bus[8];
    const uint8_t nBus = probes.scan(bus, 8);
    Serial.printf("bus: %u sonde\n", nBus);
    for (uint8_t i = 0; i < nBus; i++) {
      bool usata = false;
      for (uint8_t k = 0; k < ax::kMaxProbes; k++)
        if (cfg.probeId[k].equalsIgnoreCase(bus[i])) usata = true;
      Serial.printf("  %s  %s\n", bus[i].c_str(), usata ? "assegnata" : "LIBERA");
    }
    for (uint8_t i = 0; i < ax::kMaxProbes; i++) {
      ProbeBus::Reading r = probes.reading(i);
      ProbeBus::Stats st = probes.stats(i);
      Serial.printf("sonda %u \"%s\"\n", i, cfg.probeLabel[i].c_str());
      if (!cfg.probeId[i].length()) {
        Serial.println(F("  NON CONFIGURATA"));
        continue;
      }
      bool sulBus = false;
      for (uint8_t k = 0; k < nBus; k++)
        if (cfg.probeId[i].equalsIgnoreCase(bus[k])) sulBus = true;
      Serial.printf("  id=%s %s\n", cfg.probeId[i].c_str(),
                    sulBus ? "presente sul bus" : "ASSENTE DAL BUS");
      if (r.valid)
        Serial.printf("  grezzo=%.3f offset=%+.3f -> %.3f  (media 1 min %.3f, escursione %.3f su %u campioni)\n",
                      r.raw, cfg.probeOffset[i], r.c, probes.rawAverage(i),
                      probes.rawSpan(i), probes.rawSamples(i));
      else if (r.lastValidMs)
        Serial.printf("  NON VALIDA (ultimo valore noto %.2f, offset %+.3f)\n", r.c,
                      cfg.probeOffset[i]);
      else
        // Senza nemmeno una lettura buona, r.c vale ancora zero: stamparlo come
        // "ultimo valore noto" sarebbe un numero fabbricato.
        Serial.printf("  NON VALIDA: mai letta da questo avvio (offset %+.3f)\n",
                      cfg.probeOffset[i]);
      Serial.printf("  letture=%lu perse=%lu episodi=%u finestra=%lus\n",
                    (unsigned long)st.reads, (unsigned long)st.badReads, st.faults,
                    (unsigned long)probes.statsWindowSec(i));
      if (st.firstFail)
        Serial.printf("  primo tentativo fallito=%lu (%.1f%%)  di cui recuperate al 2o=%lu"
                      "  senza risposta(-127)=%lu  ultimo scarto=%.2f\n",
                      (unsigned long)st.firstFail,
                      st.reads ? 100.0f * st.firstFail / st.reads : 0.0f,
                      (unsigned long)(st.firstFail - st.badReads),
                      (unsigned long)st.badDisc, st.lastBadC);
    }
  } else if (line == "scan i2c") {
    Serial.print(F("I2C:"));
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
      yield();
    }
    Serial.println();
  } else if (line == "scan ow") {
    String ids[8];
    uint8_t n = probes.scan(ids, 8);
    Serial.printf("OneWire: %u sonde\n", n);
    for (uint8_t i = 0; i < n; i++) Serial.println("  " + ids[i]);
  } else if (line.startsWith("rele ")) {
    int sp1 = line.indexOf(' ', 5);
    if (sp1 < 0) { serialHelp(); return; }
    uint8_t n = (uint8_t)line.substring(5, sp1).toInt();
    bool on = line.substring(sp1 + 1).toInt() != 0;
    if (anyChamberArmed()) {
      // Vietato se ANCHE UNA camera e' armata: i rele' sono del dispositivo, e un test
      // sulla camera A puo' eccitare un carico mentre la B sta fermentando.
      Serial.println(F("RIFIUTATO: almeno una camera e' ARMATA. Usa 'disarma' prima del test."));
      return;
    }
    if (!outputs.testMode()) outputs.enterTestMode(anyChamberArmed());
    if (outputs.testRelay(n, on)) Serial.printf("REL%u -> %s\n", n, on ? "ON" : "OFF");
    else Serial.println(F("errore comando rele'"));
  } else if (line == "test off") {
    outputs.exitTestMode();
    Serial.println(F("modalita' test chiusa, uscite spente"));
  } else if (line.startsWith("arma ")) {
    // Due forme accettate: "arma <cool> <heat>" (camera 1: la forma storica di
    // docs/COLLAUDO.md, che deve continuare a funzionare) e "arma <camera> <cool> <heat>".
    long v[3];
    uint8_t nv = 0;
    for (int i = 5; i < (int)line.length() && nv < 3;) {
      while (i < (int)line.length() && line[i] == ' ') i++;
      if (i >= (int)line.length()) break;
      int j = line.indexOf(' ', i);
      if (j < 0) j = line.length();
      v[nv++] = line.substring(i, j).toInt();
      i = j;
    }
    uint8_t ch, coolRel, heatRel;
    // A camera singola la forma a 3 token NON esiste: la v2.23.2 leggeva sempre due
    // campi ("arma 2 3 4" -> freddo=REL2, caldo=REL3, il resto ignorato da toInt()) e
    // quel comportamento va conservato.
    if (nv == 3 && AX_N_CHAMBERS > 1) {
      if (v[0] < 1 || v[0] > AX_N_CHAMBERS) { Serial.println(F("camera fuori range")); return; }
      ch = (uint8_t)(v[0] - 1); coolRel = (uint8_t)v[1]; heatRel = (uint8_t)v[2];
    } else if (nv >= 2) {
      ch = 0; coolRel = (uint8_t)v[0]; heatRel = (uint8_t)v[1];
    } else { serialHelp(); return; }
    if (coolRel == heatRel || coolRel > 8 || heatRel > 8) {
      Serial.println(F("mapping non valido"));
      return;
    }
    outputs.exitTestMode();
    if (armOutputs(ch, coolRel, heatRel)) persistNow();
  } else if (line == "disarma") {
    disarmAllOutputs();  // senza argomenti: tutte le camere + tutti i canali
    persistNow();
  } else if (line.startsWith("disarma ")) {
    long c = line.substring(8).toInt();
    if (c < 1 || c > AX_N_CHAMBERS) { Serial.println(F("camera fuori range")); return; }
    disarmOutputs((uint8_t)(c - 1));
    persistNow();
  } else if (line.startsWith("ora ")) {
    uint32_t e = (uint32_t)strtoul(line.substring(4).c_str(), nullptr, 10);
    if (timeSvc.setManual(e)) {
      persistNow();
      // e, non nowEpoch(): con un salto grande base_ è ancora vecchio fino a
      // consumeClockJump() nel loop; e è il tempo appena impostato.
      Serial.println("ora impostata: " + timeSvc.localString(e));
    } else Serial.println(F("epoch non plausibile (>= 2020)"));
  } else if (line == "heap") {
    // Il blocco contiguo massimo e' il numero che decide se un'allocazione grande
    // riuscira': il totale libero da solo non lo dice.
    Serial.printf("free heap: %u B, frammentazione %u%%, blocco contiguo max %u B\n",
                  ESP.getFreeHeap(), ESP.getHeapFragmentation(),
                  ESP.getMaxFreeBlockSize());
  } else if (line == "riavvia") {
    scheduleReboot(500);
  } else {
    serialHelp();
  }
}

static void serialPoll() {
  static String buf;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (buf.length()) handleSerialLine(buf);
      buf = "";
    } else if (buf.length() < 96) {
      buf += ch;
    }
  }
}

// ---------- setup / loop ----------

void setup() {
  outputs.safeInitEarly();  // uscite GPIO spente PRIMA di qualunque altra cosa
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
    chambers[i].idx = i;
    chambers[i].assignDefaultProbes();
  }

  Serial.begin(115200);
  Serial.println();
  Serial.println(F("AxHTherm 2.5 — firmware locale v" AX_FW_VERSION));

  // Failsafe armato SUBITO dopo safeInitEarly(): da qui in poi un blocco del loop non
  // puo' piu' lasciare i rele' diretti eccitati oltre kFailsafeSec.
  timer1_isr_init();
  timer1_attachInterrupt(failsafeIsr);
  timer1_enable(TIM_DIV256, TIM_EDGE, TIM_LOOP);
  // Derivato da F_CPU, non dalla costante 312500: TIM_DIV256 divide il clock della CPU,
  // quindi a 160 MHz la finestra si sarebbe DIMEZZATA in silenzio.
  static_assert(F_CPU / 256UL / kFailsafeHz <= 0x7FFFFFUL, "periodo timer1 fuori dai 23 bit");
  static_assert(kFailsafeHz * kFailsafeSec <= 0xFFFFUL, "soglia oltre il contatore a 16 bit");
  timer1_write(F_CPU / 256UL / kFailsafeHz);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  if (!mcp.begin(I2C_ADDR_MCP23008))
    Serial.println(F("[hw] MCP23008 non risponde a 0x20 (REL4-8 non disponibili)"));
  outputs.attachMcp(&mcp);
  outputs.allOff();

  fsOk = LittleFS.begin();
  if (!fsOk) Serial.println(F("[fs] ERRORE LittleFS: persistenza disabilitata"));

  // Motivo dell'ultimo reset, registrato una volta per accensione: è l'unico modo
  // per distinguere a posteriori un blackout da un crash o da un aggiornamento.
  // L'ora qui non è ancora nota: viene aggiunta dal loop appena diventa attendibile.
  if (fsOk) {
    bootLog.load();
    bootLog.record(ESP.getResetReason(), 0, 0);
  }
  Serial.print(F("[boot] motivo del reset: "));
  Serial.println(ESP.getResetReason());

  // Config: assente al primo avvio -> crea i default; corrotta -> default + allarme.
  if (fsOk) {
    if (cfgStore.load(&cfg)) {
      cfg.valid = true;
    } else if (!LittleFS.exists(ConfigStore::kPath)) {
      cfg.valid = cfgStore.save(cfg);
      Serial.println(F("[cfg] primo avvio: creata configurazione di default"));
    } else {
      cfg.valid = false;
      Serial.println(F("[cfg] CONFIG CORROTTA: default + uscite disabilitate"));
    }
  }

  probes.begin();

  rtc.begin();
  if (!rtc.present()) Serial.println(F("[rtc] DS1307 non risponde a 0x68"));

  // Stato runtime: assente al primo avvio (ok), corrotto -> default + allarme. Ignorare l'esito
  // renderebbe uno /state.json corrotto indistinguibile da un primo avvio: una cotta in corso
  // ripartirebbe Off/disarmata IN SILENZIO. Come per la config, si distingue assente da illeggibile.
  const bool statoLetto = fsOk && stateStore.load(&pstate);
  if (fsOk && !statoLetto && LittleFS.exists(StateStore::kPath)) {
    bootStorageFault = true;
    Serial.println(F("[state] STATO CORROTTO all'avvio: default + allarme storage"));
  }
  if (statoLetto && pstate.failsafePending) {
    failsafeDaSegnalare = true;
    Serial.println(F("[failsafe] evento non ancora preso in carico, ripristinato dallo stato"));
  }
  if (statoLetto && stateStore.excessChambers()) {
    bootStorageFault = true;  // stato perso: reso VISIBILE, non scoperto dopo
    Serial.printf("[state] /state.json descriveva %u camere in piu' di quante ne gestisce "
                  "questo firmware: il loro stato e' PERSO\n", stateStore.excessChambers());
  }
  if (statoLetto && stateStore.migratedFromV1()) {
    // Il file era nel formato mono-camera della v2.23.2 ed e' stato migrato nella
    // camera 0. Il prossimo salvataggio lo riscrivera' in v2: conversione non
    // reversibile, quindi va detta a voce e non lasciata implicita.
    Serial.println(F("[state] stato mono-camera (schema v1) migrato nella camera 1; "
                     "il prossimo salvataggio riscrivera' /state.json in schema v2"));
  }
  timeSvc.begin(&rtc, pstate.lastSeenEpoch, cfg.ntpEnabled, cfg.ntpServer, cfg.tz);
  Serial.printf("[rtc] trusted=%d epoch=%u %s\n", timeSvc.trusted(), timeSvc.nowEpoch(),
                timeSvc.trusted() ? timeSvc.localString(timeSvc.nowEpoch()).c_str() : "(fallback millis)");

  applyConfigLive();

  // Profilo attivo + ripresa dello stato runtime, PER CAMERA.
  ax::StepDef steps[ax::kMaxSteps];
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
    ChamberCtx& cam = chambers[i];
    ChamberState& ps = pstate.ch[i];
    uint8_t nSteps = 0;
    if (fsOk && profStore.loadActive(i, &cam.activeProfileName, steps, &nSteps)) {
      cam.engine.setSteps(steps, nSteps);
    } else if (fsOk && LittleFS.exists(ProfileStore::activePath(i))) {
      // Il profilo attivo esiste ma e' illeggibile (corrotto/troncato): se lo stato dice
      // cotta in corso, la si sta abbandonando in silenzio (0 step -> mode forzato Off
      // piu' sotto). Allarme per avvisare.
      if (ps.engine.mode == ax::RunMode::Profile) bootStorageFault = true;
      Serial.printf("[profilo] camera %u: PROFILO ATTIVO ILLEGGIBILE all'avvio\n", i + 1);
    }
    if (ps.engine.stepIdx >= cam.engine.stepCount() && cam.engine.stepCount() > 0)
      ps.engine.stepIdx = (uint8_t)(cam.engine.stepCount() - 1);
    if (cam.engine.stepCount() == 0 && ps.engine.mode == ax::RunMode::Profile)
      ps.engine.mode = ax::RunMode::Off;  // profilo sparito: niente marcia a vuoto
    cam.engine.restore(ps.engine);

    // Al boot i rele' sono comunque caduti: il compressore non e' "in marcia".
    cam.pipeline.guard().restore(ps.guard, false);
    cam.pipeline.setEstimators(ps.coolEstCph, ps.heatEstCph);  // inerzia gia' appresa
    if (ps.coolWasOn) {
      // Era in marcia al momento del reset: riposo pieno prima di ripartire.
      ax::CompressorGuard::Persist g = cam.pipeline.guard().persist();
      g.lastOffEpoch = timeSvc.nowEpoch();
      cam.pipeline.guard().restore(g, false);
    }
    if (!timeSvc.trusted() || !pstate.epochsTrusted) {
      // Orologio inaffidabile ORA, oppure epoch salvati su una timeline fittizia
      // (boot precedente senza RTC): ritardo compressore assunto NON trascorso e
      // timeline del profilo ri-basata sull'epoca attuale (conservativo).
      cam.pipeline.guard().assumeJustStopped(timeSvc.nowEpoch());
      if (ps.engine.mode == ax::RunMode::Profile) {
        ax::EngineSnapshot snap = cam.engine.snapshot();
        snap.stepStartEpoch = timeSvc.nowEpoch();
        if (snap.paused) snap.pausedAtEpoch = timeSvc.nowEpoch();
        // Anche la finestra "densita' stabile" va ri-basata: epoch di timeline diverse
        // la bloccherebbero per sempre o la farebbero scattare all'istante.
        snap.stableSinceEpoch = 0;
        snap.stableRefGravity = 0;
        const ax::StepDef* pssteps = cam.engine.steps();
        const bool rampStep = pssteps && snap.stepIdx < cam.engine.stepCount() &&
            (pssteps[snap.stepIdx].type == ax::StepType::RampRate ||
             pssteps[snap.stepIdx].type == ax::StepType::RampTime);
        // Profilo COMPLETATO che tiene il target su una rampa: con stepStartEpoch
        // ri-basato (elapsed 0) il setpoint tornerebbe all'origine. Si congela al target
        // (stepStartTemp = target + rampReanchored) per mantenere l'hold di conservazione.
        if (rampStep && snap.completed) {
          snap.stepStartTemp = pssteps[snap.stepIdx].targetC;
          snap.rampReanchored = true;
        }
        cam.engine.restore(snap);
        // Rampa ATTIVA: non deve ripartire dall'origine (freddo abbandonato) ne' essere
        // congelata al target (romperebbe il rilevamento di stallo): va ri-ancorata alla
        // temperatura REALE corrente, qui non ancora disponibile. Si rinvia al primo tick
        // valido (in controlTick).
        if (rampStep && !snap.completed) cam.rebaseRampPending = true;
      }
    }
    cam.prevCool = false;

    // Mapping rele' persistito. Va RIVALIDATO, non applicato alla cieca: setMapping
    // scavalca armOutputs e quindi tutte le invarianti centralizzate li'. Uno
    // /state.json corrotto, scritto a mano o proveniente da un firmware diverso
    // potrebbe portare freddo e caldo sullo STESSO rele' — compressore e riscaldante
    // comandati insieme dallo stesso pin, aggirando l'interlock.
    const uint8_t relFreddo = ps.coolRel, relCaldo = ps.heatRel;
    const bool mappingValido = relFreddo <= 8 && relCaldo <= 8 &&
                               !(relFreddo && relFreddo == relCaldo) &&
                               !outputs.relayTakenByOther(i, relFreddo) &&
                               !outputs.relayTakenByOther(i, relCaldo);
    if (mappingValido) {
      outputs.setMapping(i, relFreddo, relCaldo);
      cam.armed = ps.armed;
    } else {
      outputs.setMapping(i, 0, 0);
      cam.armed = false;         // senza mapping valido non si attua nulla
      bootStorageFault = true;   // reso VISIBILE: non si perde un armamento in silenzio
      Serial.printf("[out] camera %u: MAPPING PERSISTITO NON VALIDO (freddo=%u caldo=%u): "
                    "uscite non assegnate e camera disarmata\n", i + 1, relFreddo, relCaldo);
    }
    cam.safety.notArmed = !cam.armed;
    Serial.printf("[out] camera %u: freddo=REL%u caldo=REL%u armato=%d\n", i + 1,
                  outputs.coolRel(i), outputs.heatRel(i), cam.armed);
  }

  // Camere che il file di stato NON descriveva (es. /state.json in schema v1, che ne
  // conosce una sola): ripartirebbero con una guardia VERGINE, e in CompressorGuard
  // lastOffEpoch == 0 significa "mai avvenuto: nessun vincolo". Dopo un riavvio a
  // compressore in marcia sarebbe il corto ciclo che la guardia esiste per impedire.
  // Si assumono appena fermate: riposo pieno.
  // NB: solo se un file c'era davvero. Al primissimo avvio (nessun file) si conserva il
  // comportamento storico, cioe' nessun vincolo sul primo avvio del compressore.
  if (statoLetto)
    for (uint8_t i = stateStore.chambersRead(); i < AX_N_CHAMBERS; i++)
      chambers[i].pipeline.guard().assumeJustStopped(timeSvc.nowEpoch());



  if (fsOk) dataLog.begin();
  if (fsOk) calibStore.load();
  if (fsOk) devices.load();  // elenco densimetri già visti  // punti di calibrazione del densimetro (se presenti)

  net.begin(cfg.hostname, cfg.ssid, cfg.wifiPassword, cfg.dhcp, cfg.ip, cfg.mask, cfg.gw,
            cfg.dns, cfg.softApPassword, cfg.softApAfterMin);

  if (oled.begin(cfg.oledEnabled, cfg.oledDriver, cfg.oledAddress, cfg.oledFlip))
    oled.splash("AxHTherm", "v" AX_FW_VERSION);

  mqtt.begin(cfg.mqttEnabled, cfg.mqttHost, cfg.mqttPort, cfg.mqttUser, cfg.mqttPass,
             cfg.mqttBaseTopic, cfg.haDiscovery, cfg.hostname);
  mqtt.setProgress(failsafeKick);  // la discovery e' lunga: vedi MqttSvc::setProgress
  mqtt.setCommandHandler([](uint8_t ch, const String& cmd, const String& payload) {
    if (ch >= AX_N_CHAMBERS) return;
    uint32_t now = timeSvc.nowEpoch();
    if (cmd == "climate_temp") {
      // Il payload DEVE essere numerico: toFloat() su "None"/"unknown"/"" darebbe 0 e
      // finirebbe come setpoint 0 °C in silenzio. E il floor è -1 (antigelo), come per
      // gli step e il cold crash — non -10, che porterebbe la birra a congelare.
      String p = payload;
      p.trim();
      char* endp = nullptr;
      double v = strtod(p.c_str(), &endp);
      if (p.length() && endp && *endp == '\0' && v >= -1.0 && v <= 40.0) {
        chambers[ch].engine.overrideSetpoint((float)v);
        persistNow();
      }
    } else if (cmd == "climate_mode") {
      if (payload == "off") {
        chambers[ch].engine.stop();
        persistNow();
      } else if (payload == "auto" && chambers[ch].engine.mode() == ax::RunMode::Off) {
        chambers[ch].engine.setThermostat(true, chambers[ch].engine.thermostatSetpoint());
        persistNow();
      }
    } else if (cmd == "profile") {
      ProbeBus::Reading ctl = probes.reading(chambers[ch].probeControl);
      // "start" via MQTT non deve riavviare un profilo GIÀ in corso: azzererebbe l'OG
      // della cotta (attenuazione/ABV persi) senza conferma, da un comando non sorvegliato
      // o da un'automazione sbagliata. Si parte solo da fermo o da termostato.
      if (payload == "start") {
        // Solo se NON è già in marcia: un profilo completato o fermo si può (ri)avviare;
        // uno attivo no (azzererebbe l'OG). running() = Profile && !completed.
        if (!chambers[ch].engine.running()) startProfileCmd(ch, 0, pstate.ch[ch].batchName, 0);
      } else if (payload == "stop") { chambers[ch].engine.stop(); persistNow(); }
      else if (payload == "pause") { chambers[ch].engine.pause(now); persistNow(); }
      else if (payload == "resume") { chambers[ch].engine.resume(now); persistNow(); }
      else if (payload == "skip") { chambers[ch].engine.skip(now, ctl.c, ctl.valid); persistNow(); }
    }
    // Aggiorna subito lo snapshot così la ripubblicazione MQTT riflette il nuovo stato
    // (non quello precedente al comando).
    chambers[ch].curStatus = buildStatus(chambers[ch]);
  });

  WebApi::Refs refs;
  refs.cfg = &cfg;
  refs.cfgStore = &cfgStore;
  refs.profStore = &profStore;
  refs.gravityAny = &gravityAny;
  refs.calib = &calibStore;
  refs.devices = &devices;
  refs.boots = &bootLog;
  refs.calibDeviceName = &lastGravityDevice;
  refs.probes = &probes;
  refs.outputs = &outputs;
  refs.time = &timeSvc;
  refs.log = &dataLog;
  // Gate del test manuale dei rele': i canali sono del DISPOSITIVO, quindi conta se
  // una camera QUALSIASI e' armata. Semantica diversa da isArmed (camera servita).
  refs.anyArmed = anyChamberArmed;
  refs.persistNow = persistNow;
  refs.applyConfigLive = applyConfigLive;
  // /api/outputs/disarm non porta un indice di camera: disarma TUTTE. Per un'azione
  // di sicurezza fare di piu' e' la scelta giusta; a N=1 e' identico a prima.
  refs.disarmAll = []() { disarmAllOutputs(); };
  // Guardia di dispositivo per la taratura: la sonda e' "in uso" se pilota i rele' di
  // una camera armata, o se un profilo in marcia la sta consumando per avanzare gli step.
  refs.progress = failsafeKick;
  refs.probeInUse = [](uint8_t idx) {
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
      if (chambers[i].probeControl != idx) continue;
      if (chambers[i].armed) return true;
      if (chambers[i].engine.running() && !chambers[i].engine.paused()) return true;
    }
    return false;
  };

  // Una vista per camera. Le lambda catturano l'indice per valore: sono le uniche
  // catturanti dell'applicazione, e restano piccole (un uint8_t).
  for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
    WebApi::ChamberRefs& cr = refs.ch[i];
    cr.chamber = i;
    cr.engine = &chambers[i].engine;
    cr.pipeline = &chambers[i].pipeline;
    cr.gravity = &gravity[i];
    cr.activeProfileName = &chambers[i].activeProfileName;
    cr.controlProbe = &chambers[i].probeControl;
    cr.auxProbe = chambers[i].probeAux;
    cr.auxCount = chambers[i].auxCount;
    // Stato fresco a ogni /api/status: la cache curStatus (1 s) mescolata alle letture
    // live del motore fatte nel handler dava risposte contraddittorie subito dopo un
    // comando (running=false ma modo/uscite/setpoint ancora vecchi). buildStatus e' una
    // lettura pura.
    cr.status = [i]() { return buildStatus(chambers[i]); };
    cr.isArmed = [i]() { return chambers[i].armed; };
    cr.startProfile = [i](int id, const String& batch, uint32_t at) {
      return startProfileCmd(i, id, batch, at);
    };
    cr.arm = [i](uint8_t c, uint8_t h) { return armOutputs(i, c, h); };
    cr.disarm = [i]() { disarmOutputs(i); };
    cr.setOg = [i](float og) { setOgGravity(i, og); };
  }
  refs.ispindelData = onIspindel;
  refs.factoryReset = factoryReset;
  refs.scheduleReboot = scheduleReboot;
  web.begin(refs);

  serialHelp();
  Serial.println(F("[sys] avvio completato"));
}

void loop() {
  uint32_t nowMs = millis();

  probes.loop(nowMs);
  timeSvc.loop(net.staConnected());
  // L'avvio è stato registrato prima che l'ora fosse nota: appena diventa
  // attendibile gli si assegna la data, una volta sola.
  // La plausibilità (>~2017) è necessaria oltre a trusted(): al primo sync NTP trusted_
  // diventa vero ma base_ salta solo in consumeClockJump (più avanti nel loop), quindi
  // nowEpoch() è ancora quello di fallback per un giro. Senza, la riga di boot prenderebbe
  // un epoch ~1970 e non verrebbe mai corretta (è un timbro una-tantum).
  if (!bootStamped && fsOk && timeSvc.trusted() && timeSvc.nowEpoch() > 1500000000UL) {
    bootLog.stampEpoch(timeSvc.nowEpoch());
    bootStamped = true;
  }
  net.loop(nowMs);
  web.loop();
  outputs.loop();
  serialPoll();

  // Salto d'orologio (NTP dopo boot senza RTC, o set manuale via /api/time o seriale).
  // Va consumato QUI: dopo web.loop()/serialPoll, che possono averlo appena accodato in
  // questo stesso giro, e PRIMA di controlTick, così le àncore (profilo, guardia,
  // latch di isteresi/interlock) sono già traslate quando il tick le usa. Consumarlo
  // prima di web.loop lasciava un giro in cui controlTick correva su nowEpoch nuovo e
  // àncore vecchie: fast-forward del profilo o riposo compressore azzerato (archetipo 10).
  // Ricampionamento OBBLIGATORIO: nowMs e' stato letto in cima al giro, PRIMA di
  // net.loop() e web.loop(). Se un handler ha bloccato il loop per secondi, il confronto
  // sul tick userebbe un valore stantio e il tick slitterebbe al giro successivo — cioe'
  // il budget del failsafe non sarebbe piu' "per operazione". Con il ricampionamento il
  // tick scatta nella STESSA iterazione dell'handler lungo e il contatore si azzera
  // appena il loop respira.
  nowMs = millis();

  int64_t clockJump = timeSvc.consumeClockJump();
  if (clockJump != 0) {
    // TUTTE le camere: dimenticarne una le lascerebbe àncore sulla vecchia timeline —
    // guardia compressore, latch d'isteresi e tempo morto dell'interlock congelati o
    // azzerati. È la forma di difetto che ControlPipeline::shiftClock centralizza
    // proprio per evitare (vedi il commento in ControlPipeline.h).
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
      chambers[i].engine.shiftClock(clockJump);
      chambers[i].pipeline.shiftClock(clockJump);  // guardia + latch isteresi/interlock
    }
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) gravity[i].shiftClock(clockJump);
    gravityAny.shiftClock(clockJump);  // altrimenti l'ultima lettura resta "nel futuro"
    persistNow();
    Serial.printf("[time] salto orologio di %lld s: timeline traslata\n",
                  (long long)clockJump);
  }

  if (nowMs - lastCtlMs >= 1000) {
    uint32_t dtMs = nowMs - lastCtlMs;
    lastCtlMs = nowMs;
    // VITALITA' del failsafe. Il contatore dell'ISR e' l'unica prova che il timer stia
    // davvero girando: azzerarlo e basta la distruggerebbe, e un guasto del timer
    // resterebbe INVISIBILE mentre OLED, /api/status e il CHANGELOG dichiarano una
    // protezione che non c'e'. A 5 Hz, in un giro da ~1 s, ci si aspettano ~5
    // incrementi; zero per 10 giri di fila significa che l'ISR non arriva.
    {
      const uint16_t battiti = failsafeIsrDaTick;
      failsafeIsrDaTick = 0;  // punto UNICO di azzeramento (non per camera)
      static uint8_t giriMuti = 0;
      if (battiti == 0) {
        if (giriMuti < 255) giriMuti++;
      } else {
        giriMuti = 0;
      }
      // Uno stallo appena SOTTO la soglia non fa scattare nulla ed e' ripetibile
      // all'infinito: senza questa riga resterebbe del tutto invisibile. dtMs e' il tempo
      // REALE fra due tick, quindi misura esattamente quanto il loop e' stato via.
      if (dtMs > 5000)
        Serial.printf("[loop] tick in ritardo di %lu ms: qualcosa ha tenuto fermo il "
                      "controllo\n", (unsigned long)(dtMs - 1000));
      const bool inerte = giriMuti >= 10;
      if (inerte && !failsafeInerte)
        Serial.println(F("[failsafe] ATTENZIONE: il timer hardware non sta girando, "
                         "la protezione contro il blocco del loop NON e' attiva"));
      failsafeInerte = inerte;
    }
    // Il failsafe hardware ha spento i rele' mentre il loop era bloccato. La guardia
    // compressore non lo sa: senza risincronizzarla, mustStayOn potrebbe rieccitare il
    // compressore appena il loop riparte, cioe' esattamente il corto ciclo che il
    // commento sull'OTA (WebApi) descrive come motivo per NON fare allOff() li'.
    // assumeJustStopped impone il riposo pieno: conservativo e corretto.
    if (failsafeScattato) {
      // Il contatore va azzerato PER PRIMO: il recupero fa transazioni I2C e una
      // Serial.println, e in quella finestra l'ISR potrebbe riscattare.
      failsafeIsrDaTick = 0;
      failsafeScattato = false;
      failsafeDaSegnalare = true;
      for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
        chambers[i].pipeline.guard().assumeJustStopped(timeSvc.nowEpoch());
        // Lo stato di attuazione VERO vive dentro la pipeline (prevCool_/prevHeat_ e gli
        // stimatori predittivi): dopo che l'ISR ha spento i rele' e' stantio, e gli
        // stimatori avrebbero osservato un ciclo che non e' avvenuto.
        chambers[i].pipeline.resetActuationState();
        chambers[i].prevCool = false;  // innesco del checkpoint, non stato di controllo
      }
      // Su un MCP gia' in guasto non c'e' nulla da spegnere, e insistere costerebbe fino a
      // ~10 s di clock stretching sull'I2C (twi_clockStretchLimit = 150 ms per bit).
      if (outputs.mcpFault()) {
        for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) outputs.chamberOff(i);
      } else {
        outputs.allOff();  // allinea anche lo stato ombra e i rele' su MCP
      }
      failsafeIsrDaTick = 0;  // il recupero stesso puo' essere durato: non farlo riscattare
      persistNow();  // l'evento deve sopravvivere al riavvio che potrebbe seguire
      Serial.println(F("[failsafe] loop bloccato oltre il limite: rele' spenti dal timer "
                       "hardware, riposo compressore riassunto"));
    }
    for (uint8_t i = 0; i < AX_N_CHAMBERS; i++) {
      controlTick(chambers[i], nowMs, dtMs);
      chambers[i].curStatus = buildStatus(chambers[i]);
    }
  }

  // Il pannello e' uno solo: con piu' camere le alterna. Il contatore di pagina e' di
  // DISPOSITIVO — un solo display, una sola pagina visibile — quindi qui una `static`
  // condivisa e' corretta, a differenza dei timer per camera finiti in ChamberCtx.
  {
    static uint8_t oledPagina = 0;
    static uint32_t oledPaginaMs = 0;
    if (AX_N_CHAMBERS > 1 && nowMs - oledPaginaMs >= OledUi::kPageMs) {
      oledPaginaMs = nowMs;
      oledPagina = (uint8_t)((oledPagina + 1) % AX_N_CHAMBERS);
    }
    oled.loop(nowMs, chambers[oledPagina].curStatus, oledPagina, AX_N_CHAMBERS);
  }
  // Una pubblicazione per camera. Il getter e' un puntatore a funzione (lambda senza
  // catture): niente allocazioni a ogni giro e nessuna copia degli AppStatus.
  mqtt.loop(nowMs, net.staConnected(), AX_N_CHAMBERS,
            [](uint8_t i) -> const AppStatus& { return chambers[i].curStatus; });

  if (nowMs - lastCkptMs >= 300000UL) {  // checkpoint ogni 5 minuti
    lastCkptMs = nowMs;
    persistNow();
  }
  if (nowMs - lastLogMs >= (uint32_t)cfg.logSampleMin * 60000UL) {
    lastLogMs = nowMs;
    logSample();
  }
  if (nowMs - lastHeapMs >= 60000UL) {
    lastHeapMs = nowMs;
    Serial.printf("[sys] heap %u B (frag %u%%, blocco max %u B) rssi %d\n",
                  ESP.getFreeHeap(), ESP.getHeapFragmentation(),
                  ESP.getMaxFreeBlockSize(), net.rssi());
  }
  if (rebootPending && (int32_t)(nowMs - rebootAtMs) > 0) {
    if (!factoryResetPending) persistNow();
    ESP.restart();
  }

  delay(2);  // respiro a WiFi/SDK; il soft-WDT viene nutrito qui
}
