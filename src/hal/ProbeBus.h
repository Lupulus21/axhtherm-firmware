#pragma once
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "hardware_map.h"
#include "ax_dimensions.h"
#include "ax/ProbeFault.h"

// Bus OneWire con conversione asincrona: il loop di controllo non si ferma mai.
// Letture fuori dal range fisico plausibile (incluse -127 e 85 di power-on) = non valide.
// Un fault scatta dopo 3 letture consecutive cattive; il rientro dopo 2 buone.
class ProbeBus {
 public:
  // Sonde totali sul bus. Il ruolo (controllo/ausiliaria) NON e' piu' implicito
  // nell'indice: appartiene alla camera (ChamberCtx), non al bus. Il bus e' una
  // risorsa di dispositivo condivisa da tutte le camere.
  static constexpr uint8_t kProbes = ax::kMaxProbes;
  static constexpr float kMinValidC = -20.0f;
  static constexpr float kMaxValidC = 60.0f;
  static constexpr uint32_t kMaxAgeMs = 30000;

  struct Reading {
    float c = 0.0f;        // valore con offset applicato: quello che usa il controllo
    float raw = 0.0f;      // valore grezzo del sensore: quello su cui si calcola l'offset
    bool valid = false;
    bool configured = false;
    uint32_t lastValidMs = 0;
  };

  // Statistiche di affidabilità, azzerate a ogni avvio. Servono a rispondere alla
  // domanda "questa sonda è affidabile?" senza dover esportare e analizzare il log.
  struct Stats {
    uint32_t reads = 0;        // tentativi di lettura
    uint32_t badReads = 0;     // letture fuori range o assenti
    uint16_t faults = 0;       // episodi in cui la sonda è stata dichiarata non valida
    uint32_t lastFaultMs = 0;  // ultimo episodio (0 = mai)
    // firstFail conta i PRIMI tentativi falliti, badReads solo i cicli in cui è fallito
    // anche il secondo. La differenza fra i due è la marginalità del bus: senza
    // firstFail, una sonda che sbaglia sempre al primo colpo ma si riprende al secondo
    // risulterebbe perfetta, ed è esattamente il guasto intermittente che cerchiamo.
    uint32_t firstFail = 0;
    float lastBadC = 0.0f;     // ultimo valore rifiutato: dice DI CHE tipo è il guasto
    uint32_t badDisc = 0;      // quante volte era -127 (nessuna risposta / CRC fallito)
    // Tentativi falliti per tipo di guasto (indice = ax::ProbeFault; 0 = recuperati alla
    // rilettura), e l'ultimo tipo visto. Dicono dove guardare: connettore, cavo, pull-up,
    // alimentazione.
    uint16_t kind[ax::kProbeFaultKinds] = {0};
    uint8_t lastKind = 0;
  };
  Stats stats(uint8_t idx) const { return idx < kProbes ? stat_[idx] : Stats{}; }
  void resetStats() {
    for (uint8_t i = 0; i < kProbes; i++) { stat_[i] = Stats{}; statsSinceMs_[i] = millis(); }
  }
  // Finestra per sonda: riconfigurarne una azzera le sue statistiche ma non quelle
  // delle altre, e dichiarare una finestra unica le farebbe leggere tutte male.
  uint32_t statsWindowSec(uint8_t idx) const {
    return idx < kProbes ? (millis() - statsSinceMs_[idx]) / 1000 : 0;
  }

  // Finestra scorrevole dei valori grezzi: la taratura si fa sulla media di un
  // minuto, non su una lettura istantanea, e solo se il bagno è in equilibrio.
  static constexpr uint32_t kReadPeriodMs = 5000;   // periodo di interrogazione del bus
  // A 12 bit la conversione del DS18B20 dichiara 750 ms. Aspettarne 800 lasciava 50 ms
  // di margine, e chi veniva letto per primo li pagava tutti: un sensore ancora in
  // conversione risponde con zeri, il CRC fallisce e la libreria restituisce -127.
  // Con 250 ms di margine il caso non si presenta, e il ciclo resta di 5 s.
  static constexpr uint32_t kConvMs = 1000;
  // Soglia di stallo: molto più di un ciclo, ma ben sotto kMaxAgeMs — altrimenti
  // gli stalli fra un ciclo e 30 s continuerebbero a fabbricare guasti.
  static constexpr uint32_t kStallMs = 3 * kReadPeriodMs;
  // La grazia deve durare più dell'antirimbalzo (3 letture cattive consecutive =
  // ~15,8 s), altrimenti la scadenza per età emette il verdetto prima che
  // l'antirimbalzo arrivi al suo, proprio quando i disturbi sono più probabili.
  // Resta comunque sotto kMaxAgeMs, quindi non allunga la finestra di dato vecchio.
  static constexpr uint32_t kGraziaMs = 4 * kReadPeriodMs;
  static constexpr float kCalibMaxSpanC = 0.2f;     // escursione ammessa per tarare
  static constexpr uint8_t kAvgWindow = 12;  // 12 letture x 5 s = 60 s
  uint8_t rawSamples(uint8_t idx) const { return idx < kProbes ? nRaw_[idx] : 0; }
  float rawAverage(uint8_t idx) const {
    if (idx >= kProbes || nRaw_[idx] == 0) return 0.0f;
    float s = 0;
    for (uint8_t i = 0; i < nRaw_[idx]; i++) s += raw_[idx][i];
    return s / nRaw_[idx];
  }
  float rawSpan(uint8_t idx) const {  // escursione nella finestra: misura l'equilibrio
    if (idx >= kProbes || nRaw_[idx] == 0) return 0.0f;
    float lo = raw_[idx][0], hi = raw_[idx][0];
    for (uint8_t i = 1; i < nRaw_[idx]; i++) {
      if (raw_[idx][i] < lo) lo = raw_[idx][i];
      if (raw_[idx][i] > hi) hi = raw_[idx][i];
    }
    return hi - lo;
  }

  void begin() {
    oneWire_ = new OneWire(PIN_ONEWIRE);
    dallas_ = new DallasTemperature(oneWire_);
    dallas_->begin();
    dallas_->setResolution(12);
    dallas_->setWaitForConversion(false);
  }

  // addr: 16 caratteri hex (ID ROM DS18B20) o stringa vuota = non configurata.
  void configureProbe(uint8_t idx, const String& addrHex, float offsetC) {
    if (idx >= kProbes) return;
    DeviceAddress nuovo;
    bool ok = parseAddr(addrHex, nuovo);
    // Azzerare la lettura a ogni salvataggio di configurazione fabbricherebbe un
    // guasto della sonda di controllo: il controllo entrerebbe in stato sicuro,
    // fermando il compressore fuori dal suo tempo minimo di marcia e facendo
    // ripartire da zero il riposo. Si riparte da capo SOLO se cambia la sonda.
    bool cambiata = (ok != cfg_[idx].configured) ||
                    (ok && memcmp(nuovo, cfg_[idx].addr, sizeof(DeviceAddress)) != 0);
    cfg_[idx].configured = ok;
    if (ok) memcpy(cfg_[idx].addr, nuovo, sizeof(DeviceAddress));
    cfg_[idx].offset = offsetC;
    if (cambiata) {
      st_[idx] = Reading{};
      badCount_[idx] = 0;
      goodCount_[idx] = 0;
      stat_[idx] = Stats{};        // le statistiche di un'altra sonda non valgono più
      nRaw_[idx] = rawHead_[idx] = 0;
      statsSinceMs_[idx] = millis();
    } else if (st_[idx].valid) {
      st_[idx].c = st_[idx].raw + offsetC;  // il nuovo offset entra subito, senza guasti
    }
    st_[idx].configured = cfg_[idx].configured;
  }

  // Cambia SOLO l'offset, senza toccare lo stato di validità: usato dalla taratura.
  void setOffset(uint8_t idx, float offsetC) {
    if (idx >= kProbes) return;
    cfg_[idx].offset = offsetC;
    if (st_[idx].valid) st_[idx].c = st_[idx].raw + offsetC;
  }

  void loop(uint32_t nowMs) {
    if (!dallas_) return;

    // --- rilevamento stallo, PRIMA di toccare il bus ---
    // Se fra due passaggi è trascorso molto più di un ciclo di lettura, il loop è
    // stato fermo (download del registro, upload OTA, riconnessione MQTT): il bus
    // non è stato interrogato, e questo non è un guasto delle sonde. Il rilevamento
    // deve stare qui: farlo a valle della lettura significherebbe correggere
    // timestamp appena scritti, spingendoli nel futuro.
    uint32_t dal = (lastLoopMs_ == 0) ? 0 : (nowMs - lastLoopMs_);
    lastLoopMs_ = nowMs;
    if (dal > kStallMs) {
      // La conversione eventualmente in corso è da buttare: rileggere adesso lo
      // scratchpad restituirebbe il valore di PRIMA dello stallo timbrandolo come
      // fresco. Meglio ripartire da una richiesta nuova.
      state_ = State::Idle;
      lastRequestMs_ = nowMs - kReadPeriodMs;  // la richiesta riparte già in questo giro
      // Finestra di grazia sull'età: breve, quanto basta a completare una
      // conversione nuova. Non falsifica alcun timestamp — sospende solo il
      // verdetto "troppo vecchia" per il tempo in cui non potevamo leggere.
      for (uint8_t i = 0; i < kProbes; i++) { apriGrazia(i, nowMs); nRaw_[i] = 0; rawHead_[i] = 0; }
    }

    if (state_ == State::Idle && nowMs - lastRequestMs_ >= kReadPeriodMs) {
      dallas_->requestTemperatures();
      lastRequestMs_ = nowMs;
      state_ = State::Converting;
    } else if (state_ == State::Converting && nowMs - lastRequestMs_ >= kConvMs) {
      // L'ordine di lettura ruota a ogni ciclo. Leggere sempre nello stesso ordine
      // concentra ogni residuo di marginalità sulla stessa sonda — e con l'ordine
      // fisso quella sonda era l'indice 0, cioè proprio quella che pilota i relè.
      for (uint8_t k = 0; k < kProbes; k++) {
        const uint8_t i = (uint8_t)((k + primaSonda_) % kProbes);
        if (!cfg_[i].configured) continue;
        float t = dallas_->getTempC(cfg_[i].addr);
        // Il primo tentativo viene contato SEMPRE, riuscito o no: è la misura della
        // marginalità del bus, e va registrata prima che il ritentativo la cancelli.
        if (!(t > kMinValidC && t < kMaxValidC)) {
          stat_[i].firstFail++;
          stat_[i].lastBadC = t;
          if (t < -100.0f) stat_[i].badDisc++;
          t = rileggi(i, t);
        }
        stat_[i].reads++;
        if (t > kMinValidC && t < kMaxValidC) {
          badCount_[i] = 0;
          if (goodCount_[i] < 200) goodCount_[i]++;
          if (goodCount_[i] >= 2 || st_[i].lastValidMs == 0 || st_[i].valid) {
            st_[i].raw = t;
            raw_[i][rawHead_[i]] = t;
            rawHead_[i] = (rawHead_[i] + 1) % kAvgWindow;
            if (nRaw_[i] < kAvgWindow) nRaw_[i]++;
            st_[i].c = t + cfg_[i].offset;
            st_[i].valid = true;
            st_[i].lastValidMs = nowMs;
            // Solo per QUESTA sonda: chiudere la grazia di tutte perché una ausiliaria
            // ha letto bene condannerebbe la sonda di controllo rimasta indietro.
            graziaFinoMs_[i] = 0;
          }
        } else {
          goodCount_[i] = 0;
          stat_[i].badReads++;   // fallito anche il ritentativo: lettura davvero persa
          if (badCount_[i] < 200) badCount_[i]++;
          if (badCount_[i] >= 3) {
            if (st_[i].valid) {  // conta l'EPISODIO, non ogni lettura persa
              stat_[i].faults++;
              stat_[i].lastFaultMs = nowMs;
            }
            invalida(i);
          }
        }
      }
      primaSonda_ = (uint8_t)((primaSonda_ + 1) % kProbes);
      state_ = State::Idle;
    }

    // Una lettura può anche essere "buona" ma vecchia: dopo kMaxAgeMs non è più valida.
    for (uint8_t i = 0; i < kProbes; i++) {
      if (graziaFinoMs_[i] && (int32_t)(nowMs - graziaFinoMs_[i]) >= 0) graziaFinoMs_[i] = 0;
      if (graziaFinoMs_[i]) continue;  // il loop era fermo: il verdetto è sospeso
      if (st_[i].valid && nowMs - st_[i].lastValidMs > kMaxAgeMs) {
        invalida(i);
        stat_[i].faults++;
        stat_[i].lastFaultMs = nowMs;
      }
    }
  }

  // Unico punto in cui una sonda perde validità: azzera anche la finestra dei
  // grezzi, altrimenti dopo un guasto la taratura si sbloccherebbe su campioni
  // raccolti prima del guasto stesso, contandoli come "un minuto di letture".
  // 0 significa "nessuna grazia": una scadenza che cadesse esattamente su 0 la
  // disattiverebbe in silenzio, quindi si sposta di un millisecondo.
  void apriGrazia(uint8_t idx, uint32_t nowMs) {
    graziaFinoMs_[idx] = nowMs + kGraziaMs;
    if (!graziaFinoMs_[idx]) graziaFinoMs_[idx] = 1;
  }

  void invalida(uint8_t idx) {
    st_[idx].valid = false;
    nRaw_[idx] = 0;
    rawHead_[idx] = 0;
  }

  // Da chiamare prima di ogni accesso diagnostico al bus: dichiara persa la
  // conversione in corso e apre una breve grazia, così l'interruzione non si
  // trasforma né in un valore vecchio spacciato per nuovo né in un falso guasto.
  void annullaConversione() {
    if (state_ == State::Converting) {
      state_ = State::Idle;
      uint32_t ora = millis();
      lastRequestMs_ = ora - kReadPeriodMs;
      for (uint8_t i = 0; i < kProbes; i++) apriGrazia(i, ora);
    }
  }

  Reading reading(uint8_t idx) const { return idx < kProbes ? st_[idx] : Reading{}; }

  // Scansione del bus per la diagnostica: ritorna gli ID trovati come stringhe hex.
  uint8_t scan(String out[], uint8_t maxOut) {
    if (!dallas_) return 0;
    // La scansione resetta il bus e aborta la conversione in corso: se non lo si
    // dichiara, il giro successivo leggerebbe uno scratchpad vecchio timbrandolo
    // come fresco. Si riparte da una richiesta nuova.
    annullaConversione();
    dallas_->begin();
    uint8_t n = 0;
    DeviceAddress a;
    oneWire_->reset_search();
    while (n < maxOut && oneWire_->search(a)) {
      if (OneWire::crc8(a, 7) != a[7]) continue;
      out[n] = addrToHex(a);
      n++;
    }
    return n;
  }

  // Lettura per la diagnostica di QUALUNQUE ID sul bus: le conversioni globali
  // girano già ogni 5 s, quindi è una lettura del risultato più recente, senza blocchi.
  bool tempById(const String& hex, float* out) {
    uint8_t a[8];
    if (!dallas_ || !parseAddr(hex, a)) return false;
    annullaConversione();
    float t = dallas_->getTempC(a);
    if (t <= kMinValidC || t >= kMaxValidC) return false;
    *out = t;
    return true;
  }

  static String addrToHex(const uint8_t* a) {
    char buf[17];
    for (uint8_t i = 0; i < 8; i++) sprintf(buf + i * 2, "%02X", a[i]);
    return String(buf);
  }

 private:
  // Lettura fallita: si rilegge lo scratchpad (gli stessi dati della conversione appena
  // fatta, ~12 ms) per capire il PERCHE', e la rilettura vale da ritentativo. Un guasto
  // transitorio (nessuna risposta, linea alta o a massa, CRC) si ritenta fino a due volte:
  // un disturbo non deve fermare il compressore. Gli altri non cambiano rileggendo.
  // Il tipo si conta una volta per ciclo (l'esito finale), come le letture perse.
  // Famiglie diverse dalla DS18B20: il ritentativo di sempre, con la libreria.
  float rileggi(uint8_t i, float t) {
    if (!ax::isB20Family(cfg_[i].addr[0]))
      return t < -100.0f ? dallas_->getTempC(cfg_[i].addr) : t;
    ax::ProbeFault f = ax::ProbeFault::NoResponse;
    float out = t;
    for (uint8_t tent = 0; tent < 2; tent++) {
      uint8_t sp[9];
      const bool pres = dallas_->readScratchPad(cfg_[i].addr, sp);
      float tt = t;
      f = ax::classifyScratchpad(pres, sp, cfg_[i].addr[0], kMinValidC, kMaxValidC, &tt);
      if (f == ax::ProbeFault::None) { out = tt; break; }
      if (f == ax::ProbeFault::Reset) {
        // La sonda si e' riavviata (alimentazione instabile): riparte con la risoluzione
        // salvata nella sua EEPROM. Si riporta a 12 bit; la libreria scrive solo se diversa,
        // quindi di norma e' una sola lettura in piu'.
        dallas_->setResolution(cfg_[i].addr, 12, true);
        break;
      }
      if (f == ax::ProbeFault::OutOfRange) break;
    }
    Stats& s = stat_[i];
    if (s.kind[(uint8_t)f] < 0xFFFF) s.kind[(uint8_t)f]++;
    if (f != ax::ProbeFault::None) s.lastKind = (uint8_t)f;
    return out;
  }

  static bool parseAddr(const String& hex, uint8_t out[8]) {
    if (hex.length() != 16) return false;
    for (uint8_t i = 0; i < 8; i++) {
      char hi = hex[i * 2], lo = hex[i * 2 + 1];
      auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int h = nib(hi), l = nib(lo);
      if (h < 0 || l < 0) return false;
      out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
  }

  struct ProbeCfg {
    uint8_t addr[8] = {0};
    float offset = 0.0f;
    bool configured = false;
  };

  enum class State { Idle, Converting };

  OneWire* oneWire_ = nullptr;
  DallasTemperature* dallas_ = nullptr;
  ProbeCfg cfg_[kProbes];
  Reading st_[kProbes];
  Stats stat_[kProbes];
  uint32_t statsSinceMs_[kProbes] = {0};
  uint32_t lastLoopMs_ = 0;
  uint8_t primaSonda_ = 0;   // da quale indice comincia il giro di letture
  uint32_t graziaFinoMs_[kProbes] = {0};
  float raw_[kProbes][kAvgWindow] = {{0}};
  uint8_t rawHead_[kProbes] = {0};
  uint8_t nRaw_[kProbes] = {0};
  uint8_t badCount_[kProbes] = {0};
  uint8_t goodCount_[kProbes] = {0};
  uint32_t lastRequestMs_ = 0;
  State state_ = State::Idle;
};
