#include "ax/ProfileEngine.h"

namespace ax {

static int64_t sdiff(uint32_t a, uint32_t b) { return (int64_t)a - (int64_t)b; }

void ProfileEngine::setSteps(const StepDef* steps, uint8_t count) {
  count_ = count > kMaxSteps ? kMaxSteps : count;
  for (uint8_t i = 0; i < count_; i++) steps_[i] = steps[i];
  for (uint8_t i = count_; i < kMaxSteps; i++) steps_[i] = StepDef{};
}

int8_t ProfileEngine::nextEnabled(int8_t from) const {
  for (int8_t i = from; i < (int8_t)count_; i++) {
    if (steps_[i].enabled) return i;
  }
  return -1;
}

bool ProfileEngine::startProfile(uint32_t now, float tempC, bool tempValid,
                                 uint32_t delayedStartEpoch) {
  int8_t first = nextEnabled(0);
  if (first < 0) return false;
  float thSp = s_.thermostatSetpoint;  // il setpoint termostato sopravvive al profilo
  s_ = EngineSnapshot{};
  s_.thermostatSetpoint = thSp;
  s_.mode = RunMode::Profile;
  s_.profileStartEpoch = now;
  s_.delayedStartEpoch = delayedStartEpoch;
  if (delayedStartEpoch == 0 || sdiff(now, delayedStartEpoch) >= 0) {
    s_.delayedStartEpoch = 0;
    enterStep((uint8_t)first, now, tempC, tempValid, steps_[first].targetC);
  } else {
    s_.stepIdx = (uint8_t)first;  // step pronto, partirà al raggiungimento dell'epoch
    s_.stepStartEpoch = 0;
  }
  return true;
}

void ProfileEngine::stop() {
  float thSp = s_.thermostatSetpoint;
  s_ = EngineSnapshot{};
  s_.thermostatSetpoint = thSp;
  s_.mode = RunMode::Off;
}

void ProfileEngine::pause(uint32_t now) {
  if (s_.mode == RunMode::Profile && !s_.paused && !s_.completed) {
    s_.paused = true;
    s_.pausedAtEpoch = now;
  }
}

void ProfileEngine::resume(uint32_t now) {
  if (s_.mode == RunMode::Profile && s_.paused) {
    // Trasla l'inizio step del tempo di pausa: le rampe riprendono da dove erano.
    // (Con partenza ritardata non ancora scattata stepStartEpoch è 0: niente da traslare.)
    int64_t pausedFor = sdiff(now, s_.pausedAtEpoch);
    if (pausedFor > 0 && s_.stepStartEpoch != 0) s_.stepStartEpoch += (uint32_t)pausedFor;
    // Anche la finestra "densità stabile" è su epoch assoluti: senza traslazione il
    // tempo di pausa varrebbe come stabilità osservata (mai guardata durante la pausa).
    if (pausedFor > 0 && s_.stableSinceEpoch != 0) s_.stableSinceEpoch += (uint32_t)pausedFor;
    s_.paused = false;
    s_.pausedAtEpoch = 0;
  }
}

bool ProfileEngine::confirm(uint32_t now, float tempC, bool tempValid) {
  if (s_.mode != RunMode::Profile || s_.completed || !s_.waitingConfirm) return false;
  return skip(now, tempC, tempValid);
}

bool ProfileEngine::skip(uint32_t now, float tempC, bool tempValid) {
  if (s_.mode != RunMode::Profile || s_.completed) return false;
  s_.waitingConfirm = false;  // saltare vale anche come conferma
  int8_t next = nextEnabled((int8_t)(s_.stepIdx + 1));
  if (next < 0) {
    s_.completed = true;  // ultimo step saltato: profilo completo, setpoint mantenuto
    // Se si stava ancora aspettando la partenza ritardata, va azzerata: altrimenti al
    // suo epoch il tick (che valuta la partenza ritardata PRIMA del ramo completed)
    // riazzererebbe completed e farebbe ripartire lo step "saltato".
    s_.delayedStartEpoch = 0;
    return true;
  }
  // Skip mentre si aspetta la partenza ritardata: lo step non è ancora iniziato
  // (stepStartEpoch==0). Si avanza l'indice pendente ma si RESTA in attesa, senza
  // entrare nello step ora (enterStep lo farebbe partire subito, ignorando il ritardo).
  if (s_.delayedStartEpoch != 0) {
    s_.stepIdx = (uint8_t)next;
    s_.stepStartEpoch = 0;
    return true;
  }
  float fallback = steps_[s_.stepIdx].targetC;
  enterStep((uint8_t)next, now, tempC, tempValid, fallback);
  // Skip durante la pausa: il nuovo step riparte congelato da ORA, altrimenti alla
  // ripresa la traslazione includerebbe la pausa già trascorsa e l'inizio dello step
  // finirebbe nel futuro (elapsed negativo per giorni).
  if (s_.paused) s_.pausedAtEpoch = now;
  return true;
}

void ProfileEngine::setThermostat(bool on, float setpointC) {
  // Difesa in profondità: il setpoint del termostato non resta MAI fuori [-1,40], qualunque sia la
  // sorgente. L'HTTP lo valida già e rifiuta; ma MQTT climate_mode=auto passa il valore memorizzato
  // grezzo e una migrazione (firmware pre-C17) poteva persistirlo fuori range: qui si clampa. Il
  // floor -1 è l'antigelo, il cap 40 evita di cuocere la birra. Il test in forma di APPARTENENZA
  // (non due confronti separati) è falso anche per NaN, così un eventuale NaN non passa inalterato:
  // +inf/alto -> 40, tutto il resto (bassi, -inf, NaN) -> -1. (Mantiene la promessa "clampa QUALSIASI
  // valore" su cui si appoggia il ramo off di handleThermostat.)
  if (!(setpointC >= -1.0f && setpointC <= 40.0f))
    setpointC = (setpointC > 40.0f) ? 40.0f : -1.0f;
  s_ = EngineSnapshot{};
  s_.thermostatSetpoint = setpointC;
  s_.mode = on ? RunMode::Thermostat : RunMode::Off;
}

void ProfileEngine::overrideSetpoint(float setpointC) {
  if (s_.mode == RunMode::Profile && !s_.completed) {
    s_.overrideActive = true;
    s_.overrideSetpoint = setpointC;
  } else if (s_.mode == RunMode::Thermostat) {
    s_.thermostatSetpoint = setpointC;
  }
}

void ProfileEngine::restore(const EngineSnapshot& snap) { s_ = snap; }

void ProfileEngine::enterStep(uint8_t idx, uint32_t now, float tempC, bool tempValid,
                              float fallbackSp) {
  s_.stepIdx = idx;
  s_.stepStartEpoch = now;
  s_.stepStartTemp = tempValid ? tempC : fallbackSp;
  s_.stepStartTempValid = tempValid;
  s_.overrideActive = false;
  s_.completed = false;
  s_.rampReanchored = false;  // nuovo step: l'origine torna quella configurata (startC/fromCurrent)
  s_.waitingTarget = steps_[idx].type == StepType::Fixed && steps_[idx].waitTarget;
  s_.waitTimedOut = false;
  // La stabilità della densità si misura DENTRO lo step: la finestra riparte.
  s_.stableRefGravity = 0.0f;
  s_.stableSinceEpoch = 0;
  s_.waitingConfirm = false;
  stableOut_ = 0;
  stableLastReading_ = 0;
}

float ProfileEngine::stepSetpoint(const StepDef& st, int64_t elapsedSec) const {
  if (elapsedSec < 0) elapsedSec = 0;
  switch (st.type) {
    case StepType::Fixed:
    case StepType::Gravity:
      return st.targetC;
    case StepType::RampTime: {
      if (st.durationSec == 0) return st.targetC;
      // Origine: stepStartTemp se fromCurrent OPPURE se la rampa è stata ri-ancorata alla
      // ripresa (orologio inaffidabile) — così non si tocca st.startC (la copia RAM resta
      // identica al profilo persistito). Altrimenti l'origine configurata startC.
      float from = (st.fromCurrent || s_.rampReanchored) ? s_.stepStartTemp : st.startC;
      float f = (float)elapsedSec / (float)st.durationSec;
      if (f > 1.0f) f = 1.0f;
      return from + (st.targetC - from) * f;
    }
    case StepType::RampRate: {
      float rate = st.degPerHour < 0 ? -st.degPerHour : st.degPerHour;
      if (rate < 0.01f) return st.targetC;
      float from = s_.stepStartTemp;
      float delta = rate * (float)elapsedSec / 3600.0f;
      if (st.targetC >= from) {
        float sp = from + delta;
        return sp > st.targetC ? st.targetC : sp;
      }
      float sp = from - delta;
      return sp < st.targetC ? st.targetC : sp;
    }
  }
  return st.targetC;
}

bool ProfileEngine::gravityConditionMet(const StepDef& st, const TickInput& in) {
  switch (st.gravityMode) {
    case GravityMode::Below:
      return in.gravity <= st.gravityTarget;
    case GravityMode::Attenuation:
      return in.attenuationValid && in.attenuationPct >= st.gravityTarget;
    case GravityMode::Stable: {
      // Riferimento mobile: appena la densità esce dalla banda la finestra riparte.
      // Una discesa lenta ma continua NON risulta mai stabile finché il calo nella
      // finestra supera la banda: è il comportamento voluto.
      const float band = st.stableBand > 0.0001f ? st.stableBand : 0.0001f;
      const bool fuori = in.gravity > s_.stableRefGravity + band ||
                         in.gravity < s_.stableRefGravity - band;
      const bool nuova = in.gravityEpoch == 0 || in.gravityEpoch != stableLastReading_;
      if (nuova) stableLastReading_ = in.gravityEpoch;
      if (s_.stableSinceEpoch == 0) {
        s_.stableRefGravity = in.gravity;
        s_.stableSinceEpoch = in.nowEpoch;
        stableOut_ = 0;
        return false;
      }
      if (!fuori) {
        if (nuova) stableOut_ = 0;
        return sdiff(in.nowEpoch, s_.stableSinceEpoch) >= (int64_t)(st.stableHours * 3600.0f);
      }
      // 3.16.0: un valore ISOLATO fuori banda (rumore del densimetro, ±0,001-0,002 SG su un
      // iSpindel) non azzera la finestra: serve una seconda lettura fuori banda di fila.
      // Finche' non e' confermato lo step non avanza. Senza l'istante della lettura
      // (gravityEpoch = 0) ogni tick e' una lettura nuova: comportamento di prima.
      if (nuova && stableOut_ < 255) stableOut_++;
      if (in.gravityEpoch == 0 || stableOut_ >= 2) {
        s_.stableRefGravity = in.gravity;
        s_.stableSinceEpoch = in.nowEpoch;
        stableOut_ = 0;
      }
      return false;
    }
  }
  return false;
}

bool ProfileEngine::stepDone(const StepDef& st, int64_t elapsedSec, const TickInput& in,
                             float sp, bool* waitingGravity) {
  switch (st.type) {
    case StepType::Fixed:
      if (s_.waitingTarget) return false;  // il tempo non e' ancora partito
      return elapsedSec >= (int64_t)st.durationSec;
    case StepType::RampTime:
      return elapsedSec >= (int64_t)st.durationSec;
    case StepType::RampRate: {
      // Timeout di sicurezza (0 = nessuno): un impianto che non porta la temperatura
      // in tolleranza non deve tenere lo step in ostaggio per sempre. Stessa semantica
      // del timeout degli step Gravity.
      if (st.maxDurationSec > 0 && elapsedSec >= (int64_t)st.maxDurationSec) return true;
      bool spAtTarget = (sp == st.targetC);
      return spAtTarget && in.tempValid &&
             (in.tempC >= st.targetC - tol_ && in.tempC <= st.targetC + tol_);
    }
    case StepType::Gravity: {
      // Timeout di sicurezza PRIMA di tutto: un densimetro fermo/incrostato non
      // deve tenere in ostaggio il profilo (0 = nessun timeout).
      if (st.maxDurationSec > 0 && elapsedSec >= (int64_t)st.maxDurationSec) return true;
      // Mai avanzare su dato assente o stantio.
      if (!in.gravityValid || !in.gravityFresh) {
        *waitingGravity = true;
        return false;
      }
      if (st.gravityMode == GravityMode::Attenuation && !in.attenuationValid) {
        *waitingGravity = true;  // manca l'OG: l'attenuazione non è calcolabile
        return false;
      }
      return gravityConditionMet(st, in);
    }
  }
  return false;
}

uint32_t ProfileEngine::stepEndsAtEpoch() const {
  if (s_.mode != RunMode::Profile || s_.completed || s_.stepStartEpoch == 0) return 0;
  const StepDef& st = steps_[s_.stepIdx];
  if (st.type == StepType::Fixed && s_.waitingTarget) return 0;  // non e' ancora partito
  if (st.type == StepType::Fixed || st.type == StepType::RampTime) {
    return s_.stepStartEpoch + st.durationSec;
  }
  return 0;
}

ProfileEngine::TickOutput ProfileEngine::tick(const TickInput& in) {
  TickOutput out;
  switch (s_.mode) {
    case RunMode::Off:
      return out;
    case RunMode::Thermostat:
      out.hasSetpoint = true;
      out.setpointC = s_.thermostatSetpoint;
      return out;
    case RunMode::Profile:
      break;
  }

  // Partenza ritardata: uscite ferme finché non si raggiunge l'epoch di avvio.
  // In pausa l'attivazione resta sospesa (lo step non deve entrare da paused).
  if (s_.delayedStartEpoch != 0) {
    if (s_.paused || sdiff(in.nowEpoch, s_.delayedStartEpoch) < 0) {
      out.waitingDelayedStart = true;
      return out;
    }
    s_.delayedStartEpoch = 0;
    enterStep(s_.stepIdx, in.nowEpoch, in.tempC, in.tempValid, steps_[s_.stepIdx].targetC);
    out.stepChanged = true;
  }

  const StepDef& st = steps_[s_.stepIdx];
  int64_t elapsed = sdiff(s_.paused ? s_.pausedAtEpoch : in.nowEpoch, s_.stepStartEpoch);
  float sp = stepSetpoint(st, elapsed);

  if (s_.completed || s_.paused) {
    out.hasSetpoint = true;
    out.setpointC = s_.overrideActive ? s_.overrideSetpoint : sp;
    out.allowHeat = st.allowHeat;  // a profilo completo la politica dell'ultimo step resta
    out.allowCool = st.allowCool;
    return out;
  }

  // "Attendi il target": il tempo dello step parte quando il mosto entra in tolleranza
  // (stepStartEpoch riparte da li'). Se in kWaitMaxSec non ci arriva parte comunque, e lo si
  // segnala: un frigo che non ce la fa non deve bloccare il profilo per sempre.
  // Solo uno step Fixed puo' attendere: un /state.json di un profilo diverso non deve
  // applicare l'attesa a una rampa o a uno step a densita'.
  if (s_.waitingTarget && st.type != StepType::Fixed) s_.waitingTarget = false;
  if (s_.waitingTarget) {
    // Arrivo misurato su cio' che il controllo insegue davvero: con un setpoint manuale
    // diverso dal target, il target non verrebbe mai raggiunto.
    const float rif = s_.overrideActive ? s_.overrideSetpoint : st.targetC;
    const bool arrivato = in.tempValid && in.tempC >= rif - tol_ && in.tempC <= rif + tol_;
    // Attesa scaduta mentre la scheda era spenta (blackout): il mosto non ha mai potuto
    // arrivarci, l'attesa riparte da adesso invece di far partire il tempo.
    if (!arrivato && elapsed >= kWaitMaxSec && elapsed - kWaitMaxSec > kResumeGapSec) {
      s_.stepStartEpoch = in.nowEpoch;
      elapsed = 0;
      out.stepChanged = true;  // da salvare: il nuovo inizio dell'attesa
    }
    if (arrivato || elapsed >= kWaitMaxSec) {
      s_.waitingTarget = false;
      s_.waitTimedOut = !arrivato;
      s_.stepStartEpoch = in.nowEpoch;
      elapsed = 0;
      out.stepChanged = true;  // transizione da salvare: l'inizio vero dello step
    } else {
      out.waitingTarget = true;
      out.hasSetpoint = true;
      out.setpointC = s_.overrideActive ? s_.overrideSetpoint : sp;
      out.allowHeat = st.allowHeat;
      out.allowCool = st.allowCool;
      return out;
    }
  }

  // "Attendi conferma" (3.16.0): lo step e' finito e aspetta l'utente. Il setpoint resta
  // quello di fine step (il target per step fissi, rampe concluse e step a densita').
  if (s_.waitingConfirm) {
    out.waitingConfirm = true;
    out.hasSetpoint = true;
    out.setpointC = s_.overrideActive ? s_.overrideSetpoint : sp;
    out.allowHeat = st.allowHeat;
    out.allowCool = st.allowCool;
    return out;
  }

  bool waiting = false;
  // Avanza gli step scaduti. In un tick normale ne scade uno alla volta; dopo
  // un'interruzione lunga (blackout, scheda ferma) si avanza di UNO solo e lo step nuovo
  // parte da adesso: dare per fatte soste mai avvenute, con il controllo spento,
  // falserebbe la cotta (e un timeout a densita' successivo risulterebbe gia' scaduto).
  uint8_t guard = 0;
  while (guard++ <= kMaxSteps &&
         stepDone(steps_[s_.stepIdx], elapsed, in, sp, &waiting)) {
    if (steps_[s_.stepIdx].confirm) {
      s_.waitingConfirm = true;
      out.waitingConfirm = true;
      out.stepChanged = true;  // da salvare: l'attesa deve sopravvivere a un riavvio
      sp = stepSetpoint(steps_[s_.stepIdx], elapsed);
      break;
    }
    int8_t next = nextEnabled((int8_t)(s_.stepIdx + 1));
    if (next < 0) {
      s_.completed = true;
      out.profileCompleted = true;
      out.stepChanged = true;
      // Fine profilo: si mantiene l'ultimo setpoint (comportamento configurato D7=a).
      sp = stepSetpoint(steps_[s_.stepIdx], elapsed);
      break;
    }
    uint32_t startAt;
    const StepDef& done = steps_[s_.stepIdx];
    if (done.type == StepType::Fixed || done.type == StepType::RampTime) {
      // Lo step successivo parte dall'istante in cui il precedente è realmente scaduto.
      startAt = s_.stepStartEpoch + done.durationSec;
    } else {
      startAt = in.nowEpoch;
    }
    const bool ripresa = sdiff(in.nowEpoch, startAt) > kResumeGapSec;
    if (ripresa) startAt = in.nowEpoch;
    enterStep((uint8_t)next, startAt, in.tempC, in.tempValid, done.targetC);
    out.stepChanged = true;
    elapsed = sdiff(in.nowEpoch, s_.stepStartEpoch);
    sp = stepSetpoint(steps_[s_.stepIdx], elapsed);
    if (ripresa) break;  // al piu' un avanzamento dopo un'interruzione
  }
  out.waitingTarget = s_.waitingTarget;

  out.waitingGravity = waiting;
  // Stallo della rampa °C/ora: il setpoint è saturato al target ma la temperatura
  // non entra in tolleranza. Lo si segnala solo un'ora DOPO la fine nominale della
  // rampa, così una rampa sana che si assesta non fa scattare un falso allarme; una
  // rampa che l'impianto non raggiunge resta segnalata all'infinito (guasto reso
  // visibile). Con maxDurationSec impostato lo step ha comunque un'uscita garantita.
  {
    const StepDef& cur = steps_[s_.stepIdx];
    if (!s_.completed && cur.type == StepType::RampRate && in.tempValid &&
        sp == cur.targetC &&
        (in.tempC < cur.targetC - tol_ || in.tempC > cur.targetC + tol_)) {
      float rate = cur.degPerHour < 0 ? -cur.degPerHour : cur.degPerHour;
      int64_t nominalSec;
      if (rate < 0.01f) {
        // Stessa soglia di stepSetpoint: sotto 0.01 °C/h il setpoint è al target da t0,
        // quindi la rampa nominale dura 0 e lo stallo va segnalato dopo la sola grazia.
        nominalSec = 0;
      } else {
        float span = cur.targetC - s_.stepStartTemp;
        if (span < 0) span = -span;
        nominalSec = (int64_t)(span / rate * 3600.0f);
      }
      if (sdiff(in.nowEpoch, s_.stepStartEpoch) >= nominalSec + 3600) out.waitingRampStall = true;
    }
  }
  out.hasSetpoint = true;
  out.setpointC = s_.overrideActive ? s_.overrideSetpoint : sp;
  out.allowHeat = steps_[s_.stepIdx].allowHeat;  // dopo gli eventuali avanzamenti
  out.allowCool = steps_[s_.stepIdx].allowCool;
  return out;
}

}  // namespace ax
