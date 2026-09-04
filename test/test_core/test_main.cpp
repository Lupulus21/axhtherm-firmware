// Test nativi del motore di controllo (pio test -e native).
// Coprono i requisiti di sicurezza del mandato: interlock, short-cycle del compressore
// anche dopo riavvio simulato, ripresa degli step da epoch assoluti, sonda guasta ->
// uscite spente, densità stantia -> mai avanzare.
#include <unity.h>
#include "ax/Types.h"
#include "ax/HysteresisController.h"
#include "ax/PidController.h"
#include "ax/CompressorGuard.h"
#include "ax/Interlock.h"
#include "ax/ControlPipeline.h"
#include "ax/PeakEstimator.h"
#include "ax/ProfileEngine.h"
#include "ax/GravityTracker.h"
#include "ax/SafetyMonitor.h"

using namespace ax;

static constexpr uint32_t T0 = 1750000000;  // epoch di comodo

// ---------- Isteresi ----------

static void test_hysteresis_thresholds() {
  HysteresisController h;
  h.configure(0.5f, 0);
  OutputRequest o = h.evaluate(T0, 20.0f, 20.4f);
  TEST_ASSERT_FALSE(o.cool);
  TEST_ASSERT_FALSE(o.heat);
  o = h.evaluate(T0 + 1, 20.0f, 20.6f);  // sopra set+tol
  TEST_ASSERT_TRUE(o.cool);
  o = h.evaluate(T0 + 2, 20.0f, 20.2f);  // ancora sopra set: resta acceso
  TEST_ASSERT_TRUE(o.cool);
  o = h.evaluate(T0 + 3, 20.0f, 19.9f);  // sotto set: spegne
  TEST_ASSERT_FALSE(o.cool);
  o = h.evaluate(T0 + 4, 20.0f, 19.4f);  // sotto set-tol: caldo
  TEST_ASSERT_TRUE(o.heat);
  TEST_ASSERT_FALSE(o.cool);
  o = h.evaluate(T0 + 5, 20.0f, 20.0f);  // a set: caldo spento
  TEST_ASSERT_FALSE(o.heat);
}

static void test_hysteresis_min_state_time() {
  HysteresisController h;
  h.configure(0.5f, 60);
  OutputRequest o = h.evaluate(T0, 20.0f, 20.6f);
  TEST_ASSERT_TRUE(o.cool);
  o = h.evaluate(T0 + 10, 20.0f, 19.9f);  // vorrebbe spegnere ma è passato troppo poco
  TEST_ASSERT_TRUE(o.cool);
  o = h.evaluate(T0 + 61, 20.0f, 19.9f);
  TEST_ASSERT_FALSE(o.cool);
}

// ---------- Interlock ----------

static void test_interlock_never_both() {
  Interlock il;
  il.configure(120);
  OutputRequest both{true, true};
  Interlock::Result r = il.arbitrate(T0, both);
  TEST_ASSERT_FALSE(r.out.heat);
  TEST_ASSERT_FALSE(r.out.cool);
  TEST_ASSERT_TRUE(r.conflict);
}

static void test_interlock_switch_dead_time() {
  Interlock il;
  il.configure(120);
  il.update(T0, false, true);        // freddo acceso
  il.update(T0 + 100, false, false); // freddo spento a T0+100
  Interlock::Result r = il.arbitrate(T0 + 150, {true, false});  // caldo dopo 50 s
  TEST_ASSERT_FALSE(r.out.heat);
  TEST_ASSERT_TRUE(r.switchBlocked);
  r = il.arbitrate(T0 + 221, {true, false});  // dopo 121 s: ok
  TEST_ASSERT_TRUE(r.out.heat);
}

// ---------- Guardia compressore ----------

static void test_compressor_rest_across_reboot() {
  CompressorGuard g;
  g.configure(300, 60);
  g.update(T0, true);         // acceso
  g.update(T0 + 600, false);  // spento a T0+600
  CompressorGuard::Persist p = g.persist();

  // Riavvio simulato: nuova istanza, stato ripristinato dal file persistito.
  CompressorGuard g2;
  g2.configure(300, 60);
  g2.restore(p, false);
  TEST_ASSERT_FALSE(g2.canStart(T0 + 700));   // 100 s dopo lo stop: bloccato
  TEST_ASSERT_TRUE(g2.blockedForSec(T0 + 700) == 200);
  TEST_ASSERT_TRUE(g2.canStart(T0 + 901));    // 301 s dopo lo stop: ok
}

// Dopo un blackout tutte le camere chiamano assumeJustStopped() con lo STESSO epoch:
// senza sfasamento i riposi scadrebbero nello stesso secondo e i compressori
// partirebbero insieme, sommando spunti di corrente di 5-8x la corrente di marcia.
static void test_compressor_stagger_desynchronizes_starts() {
  CompressorGuard a, b;
  a.configure(300, 60);
  b.configure(300, 60);
  a.setStartStagger(0);   // camera 0
  b.setStartStagger(30);  // camera 1
  const uint32_t t0 = 1000000;
  a.assumeJustStopped(t0);  // stesso identico istante: e' il caso del blackout
  b.assumeJustStopped(t0);

  TEST_ASSERT_FALSE(a.canStart(t0 + 299));
  TEST_ASSERT_TRUE(a.canStart(t0 + 300));   // camera 0: riposo pieno
  TEST_ASSERT_FALSE(b.canStart(t0 + 300));  // camera 1: ancora bloccata
  TEST_ASSERT_FALSE(b.canStart(t0 + 329));
  TEST_ASSERT_TRUE(b.canStart(t0 + 330));   // ...parte 30 s dopo

  // Il conto alla rovescia mostrato all'utente deve riflettere lo sfasamento.
  TEST_ASSERT_EQUAL_UINT32(30, b.blockedForSec(t0 + 300));
  TEST_ASSERT_EQUAL_UINT32(0, a.blockedForSec(t0 + 300));
}

// Lo sfasamento non deve MAI accorciare il riposo: e' una protezione, non un tuning.
static void test_compressor_stagger_never_shortens_rest() {
  CompressorGuard g;
  g.configure(300, 60);
  const uint32_t t0 = 1000000;
  g.assumeJustStopped(t0);
  for (uint32_t st = 0; st <= 120; st += 30) {
    g.setStartStagger(st);
    TEST_ASSERT_FALSE(g.canStart(t0 + 299));       // mai prima del riposo nominale
    TEST_ASSERT_FALSE(g.canStart(t0 + 300 + st - 1));
    TEST_ASSERT_TRUE(g.canStart(t0 + 300 + st));
  }
}

static void test_compressor_rtc_untrusted_assumes_not_elapsed() {
  CompressorGuard g;
  g.configure(300, 60);
  // RTC inaffidabile: il boot assume il ritardo NON trascorso (base millis, epoch piccoli).
  g.assumeJustStopped(10);
  TEST_ASSERT_FALSE(g.canStart(100));
  TEST_ASSERT_TRUE(g.canStart(311));
}

static void test_compressor_clock_backwards_blocks() {
  CompressorGuard g;
  g.configure(300, 60);
  g.update(T0, true);
  g.update(T0 + 400, false);
  // L'orologio torna indietro: elapsed negativo -> bloccato (mai avviare nel dubbio).
  TEST_ASSERT_FALSE(g.canStart(T0 + 100));
}

// ---------- Pipeline ----------

static ControlPipeline::Cfg pipeCfgHyst() {
  ControlPipeline::Cfg c;
  c.heatUsePid = false;
  c.toleranceC = 0.5f;
  c.minStateSec = 0;
  c.minSwitchSec = 0;
  c.compressorRestSec = 300;
  c.compressorMinOnSec = 0;
  return c;
}

static void test_pipeline_probe_fault_kills_outputs() {
  ControlPipeline p;
  p.configure(pipeCfgHyst());
  ControlPipeline::Status st = p.tick(T0, 0, true, 20.0f, true, 25.0f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  // La sonda sparisce: tutto spento, subito.
  st = p.tick(T0 + 1, 1000, true, 20.0f, false, 25.0f, true, 1.0f);
  TEST_ASSERT_FALSE(st.cool);
  TEST_ASSERT_FALSE(st.heat);
}

// --- Indipendenza fra camere -------------------------------------------------------
// Sono l'invariante centrale del passaggio a 2 camere: il guasto di una camera non deve
// fermare l'altra, e le due catene non devono condividere stato.

// Guasto della sonda di controllo della camera A: le SUE uscite cadono, quelle della
// camera B no. E' il degrado che rende utilizzabile la seconda camera anche quando il
// MCP23008 non e' montato (in quel caso e' outputsFault ad alzarsi, stessa struttura).
static void test_due_camere_il_guasto_di_una_non_ferma_l_altra() {
  ControlPipeline a, b;
  a.configure(pipeCfgHyst());
  b.configure(pipeCfgHyst());
  SafetyMonitor sa, sb;
  sa.notArmed = sb.notArmed = false;  // entrambe armate e sane

  // Tick 0: entrambe sopra setpoint -> entrambe raffreddano.
  ControlPipeline::Status ra = a.tick(T0, 0, true, 20.0f, true, 25.0f, sa.outputsAllowed(), 1.0f);
  ControlPipeline::Status rb = b.tick(T0, 0, true, 20.0f, true, 25.0f, sb.outputsAllowed(), 1.0f);
  TEST_ASSERT_TRUE(ra.cool);
  TEST_ASSERT_TRUE(rb.cool);

  // La sonda della camera A si guasta. Solo la A perde il consenso.
  sa.controlProbeFault = true;
  TEST_ASSERT_FALSE(sa.outputsAllowed());
  TEST_ASSERT_TRUE(sb.outputsAllowed());

  ra = a.tick(T0 + 1, 1000, true, 20.0f, false, 25.0f, sa.outputsAllowed(), 1.0f);
  rb = b.tick(T0 + 1, 1000, true, 20.0f, true, 25.0f, sb.outputsAllowed(), 1.0f);
  TEST_ASSERT_FALSE(ra.cool);   // camera A: stato sicuro
  TEST_ASSERT_FALSE(ra.heat);
  TEST_ASSERT_TRUE(rb.cool);    // camera B: continua a lavorare
}

// Due camere con richieste OPPOSTE non devono influenzarsi: se condividessero il latch
// d'isteresi, la guardia compressore o il tempo morto dell'interlock, la seconda
// seguirebbe la prima. E' il difetto che ChamberCtx esiste per rendere impossibile.
static void test_due_camere_richieste_opposte_restano_indipendenti() {
  ControlPipeline caldo, freddo;
  caldo.configure(pipeCfgHyst());
  freddo.configure(pipeCfgHyst());

  for (uint32_t k = 0; k < 5; k++) {
    // camera "caldo": ben SOTTO setpoint -> deve riscaldare
    ControlPipeline::Status rc =
        caldo.tick(T0 + k, k * 1000, true, 20.0f, true, 15.0f, true, 1.0f);
    // camera "freddo": ben SOPRA setpoint -> deve raffreddare
    ControlPipeline::Status rf =
        freddo.tick(T0 + k, k * 1000, true, 20.0f, true, 25.0f, true, 1.0f);
    TEST_ASSERT_TRUE(rc.heat);
    TEST_ASSERT_FALSE(rc.cool);
    TEST_ASSERT_TRUE(rf.cool);
    TEST_ASSERT_FALSE(rf.heat);
  }
}

// La guardia compressore di una camera non deve consumare quella dell'altra: dopo che A
// ha acceso e spento, B deve poter partire subito (guardia vergine), e viceversa A deve
// restare bloccata dal proprio riposo.
static void test_due_camere_guardia_compressore_non_condivisa() {
  ControlPipeline a, b;
  ControlPipeline::Cfg c = pipeCfgHyst();
  c.compressorRestSec = 300;
  c.compressorMinOnSec = 0;   // niente marcia minima: isoliamo il RIPOSO
  a.configure(c);
  b.configure(c);

  // A accende e poi rientra in banda: si spegne e comincia il suo riposo.
  TEST_ASSERT_TRUE(a.tick(T0, 0, true, 20.0f, true, 25.0f, true, 1.0f).cool);
  TEST_ASSERT_FALSE(a.tick(T0 + 61, 61000, true, 20.0f, true, 19.0f, true, 1.0f).cool);
  // Subito dopo, A non puo' ripartire (riposo in corso)...
  TEST_ASSERT_FALSE(a.tick(T0 + 62, 62000, true, 20.0f, true, 25.0f, true, 1.0f).cool);
  // ...ma B, che non ha mai acceso, parte senza vincoli.
  TEST_ASSERT_TRUE(b.tick(T0 + 62, 62000, true, 20.0f, true, 25.0f, true, 1.0f).cool);
}

static void test_pipeline_not_armed_kills_outputs() {
  ControlPipeline p;
  p.configure(pipeCfgHyst());
  ControlPipeline::Status st = p.tick(T0, 0, true, 20.0f, true, 25.0f, false, 1.0f);
  TEST_ASSERT_FALSE(st.cool);
  TEST_ASSERT_FALSE(st.heat);
}

static void test_pipeline_no_short_cycle() {
  ControlPipeline p;
  p.configure(pipeCfgHyst());
  ControlPipeline::Status st = p.tick(T0, 0, true, 20.0f, true, 21.0f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  st = p.tick(T0 + 100, 0, true, 20.0f, true, 19.8f, true, 1.0f);  // sotto set: off
  TEST_ASSERT_FALSE(st.cool);
  st = p.tick(T0 + 150, 0, true, 20.0f, true, 21.5f, true, 1.0f);  // caldo di nuovo, ma riposo!
  TEST_ASSERT_FALSE(st.cool);
  TEST_ASSERT_TRUE(st.coolBlockedSec > 0);
  st = p.tick(T0 + 100 + 301, 0, true, 20.0f, true, 21.5f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
}

static void test_pipeline_pid_heat_and_cool_priority() {
  ControlPipeline::Cfg c;
  c.heatUsePid = true;
  c.toleranceC = 0.5f;
  c.minStateSec = 0;
  c.minSwitchSec = 0;
  c.compressorRestSec = 0;
  c.compressorMinOnSec = 0;
  c.pid.windowMs = 10000;
  ControlPipeline p;
  p.configure(c);
  // Molto sotto il set point: piena potenza -> heat on in qualunque punto della finestra.
  ControlPipeline::Status st = p.tick(T0, 3000, true, 20.0f, true, 10.0f, true, 1.0f);
  TEST_ASSERT_TRUE(st.heat);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, st.pidDuty);
  // Molto sopra: freddo acceso e caldo forzato spento.
  st = p.tick(T0 + 1, 3000, true, 20.0f, true, 25.0f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  TEST_ASSERT_FALSE(st.heat);
}

// ---------- PID ----------

static void test_pid_asymmetric_thresholds() {
  PidController pid;
  PidController::Cfg c;
  c.fullPowerBelow = 5.0f;
  c.cutAbove = 0.5f;
  pid.configure(c);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, pid.update(20.0f, 14.0f, 1.0f));  // e=6 >= 5
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.update(20.0f, 20.6f, 1.0f));    // e=-0.6 <= -0.5
}

static void test_pid_window_output() {
  PidController pid;
  PidController::Cfg c;
  c.kp = 10.0f; c.ki = 0.0f; c.kd = 0.0f;
  c.windowMs = 10000;
  c.fullPowerBelow = 50.0f;
  pid.configure(c);
  pid.update(20.0f, 15.0f, 1.0f);  // e=5 -> duty 50%
  TEST_ASSERT_EQUAL_FLOAT(50.0f, pid.duty());
  TEST_ASSERT_TRUE(pid.windowOutput(1000));   // 1 s nella finestra: ON
  TEST_ASSERT_FALSE(pid.windowOutput(6000));  // 6 s: OFF
}

// Due camere che condividono windowMs NON devono accendere il riscaldante nello stesso
// istante: la fase deriva da millis(), che è globale. Senza sfasamento i due spunti di
// corrente si sommerebbero sullo stesso alimentatore a ogni finestra.
static void test_pid_window_phase_offset_desynchronizes() {
  PidController a, b;
  PidController::Cfg c;
  c.kp = 10.0f; c.ki = 0.0f; c.kd = 0.0f;
  c.windowMs = 10000;
  c.fullPowerBelow = 50.0f;
  a.configure(c);
  b.configure(c);
  a.setPhaseFraction(0, 2);  // camera 0 di 2
  b.setPhaseFraction(1, 2);  // camera 1 di 2
  a.update(20.0f, 15.0f, 1.0f);  // e=5 -> duty 50%
  b.update(20.0f, 15.0f, 1.0f);
  TEST_ASSERT_EQUAL_FLOAT(50.0f, a.duty());
  TEST_ASSERT_EQUAL_FLOAT(50.0f, b.duty());

  uint16_t insieme = 0, accesiA = 0, accesiB = 0;
  for (uint32_t t = 0; t < 20000; t += 100) {  // due finestre intere
    const bool oa = a.windowOutput(t), ob = b.windowOutput(t);
    if (oa && ob) insieme++;
    if (oa) accesiA++;
    if (ob) accesiB++;
  }
  TEST_ASSERT_EQUAL_UINT16(0, insieme);   // mai accesi insieme
  TEST_ASSERT_EQUAL_UINT16(100, accesiA); // ciascuno acceso metà del tempo
  TEST_ASSERT_EQUAL_UINT16(100, accesiB);
}

// Il default (idx 0 di 1) NON deve spostare la fase: con una sola camera il
// comportamento resta identico a quello precedente all'introduzione dello sfasamento.
static void test_pid_window_phase_default_is_unshifted() {
  PidController pid, esplicito;
  PidController::Cfg c;
  c.kp = 10.0f; c.ki = 0.0f; c.kd = 0.0f;
  c.windowMs = 10000;
  c.fullPowerBelow = 50.0f;
  pid.configure(c);
  esplicito.configure(c);
  esplicito.setPhaseFraction(0, 1);  // unica camera: offset 0
  pid.update(20.0f, 15.0f, 1.0f);
  esplicito.update(20.0f, 15.0f, 1.0f);
  for (uint32_t t = 0; t < 10000; t += 250) {
    TEST_ASSERT_EQUAL(pid.windowOutput(t), esplicito.windowOutput(t));
  }
  TEST_ASSERT_TRUE(pid.windowOutput(1000));   // identico al test storico
  TEST_ASSERT_FALSE(pid.windowOutput(6000));
}

// ---------- ProfileEngine ----------

static StepDef fixedStep(float t, uint32_t durSec) {
  StepDef s;
  s.type = StepType::Fixed;
  s.enabled = true;
  s.targetC = t;
  s.durationSec = durSec;
  return s;
}

static void test_profile_fixed_advances() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[2] = {fixedStep(18.0f, 3600), fixedStep(22.0f, 3600)};
  e.setSteps(steps, 2);
  TEST_ASSERT_TRUE(e.startProfile(T0, 18.0f, true));
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 3599; in.tempC = 18.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_EQUAL_FLOAT(18.0f, out.setpointC);
  TEST_ASSERT_EQUAL_UINT8(0, e.stepIdx());
  in.nowEpoch = T0 + 3601;
  out = e.tick(in);
  TEST_ASSERT_TRUE(out.stepChanged);
  TEST_ASSERT_EQUAL_UINT8(1, e.stepIdx());
  TEST_ASSERT_EQUAL_FLOAT(22.0f, out.setpointC);
  // L'inizio dello step 2 è l'istante di scadenza teorica dello step 1, non il tick.
  TEST_ASSERT_EQUAL_UINT32(T0 + 3600, e.stepStartEpoch());
}

static void test_profile_ramp_time_midpoint() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampTime;
  s.enabled = true;
  s.fromCurrent = false;
  s.startC = 18.0f;
  s.targetC = 24.0f;
  s.durationSec = 6 * 3600;
  e.setSteps(&s, 1);
  e.startProfile(T0, 18.0f, true);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 3 * 3600; in.tempC = 20.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.0f, out.setpointC);
}

static void test_profile_resume_after_blackout() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampTime;
  s.enabled = true;
  s.fromCurrent = false;
  s.startC = 10.0f;
  s.targetC = 20.0f;
  s.durationSec = 10 * 3600;
  e.setSteps(&s, 1);
  e.startProfile(T0, 10.0f, true);
  EngineSnapshot snap = e.snapshot();

  // Riavvio simulato: nuova istanza, restore, tick 4 ore dopo l'inizio.
  ProfileEngine e2;
  e2.configure(0.5f);
  e2.setSteps(&s, 1);
  e2.restore(snap);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 4 * 3600; in.tempC = 13.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e2.tick(in);
  TEST_ASSERT_TRUE(out.hasSetpoint);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 14.0f, out.setpointC);  // riprende dalla posizione oraria
  TEST_ASSERT_EQUAL_UINT8(0, e2.stepIdx());
}

static void test_profile_blackout_past_multiple_steps() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[3] = {fixedStep(18.0f, 3600), fixedStep(20.0f, 3600), fixedStep(22.0f, 7200)};
  e.setSteps(steps, 3);
  e.startProfile(T0, 18.0f, true);
  EngineSnapshot snap = e.snapshot();

  ProfileEngine e2;
  e2.configure(0.5f);
  e2.setSteps(steps, 3);
  e2.restore(snap);
  // Blackout di 2h30: al risveglio deve trovarsi nello step 3 (iniziato a T0+7200).
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 9000; in.tempC = 20.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e2.tick(in);
  TEST_ASSERT_EQUAL_UINT8(2, e2.stepIdx());
  TEST_ASSERT_EQUAL_UINT32(T0 + 7200, e2.stepStartEpoch());
  TEST_ASSERT_EQUAL_FLOAT(22.0f, out.setpointC);
  TEST_ASSERT_FALSE(e2.completed());
}

static void test_profile_ramp_rate() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampRate;
  s.enabled = true;
  s.targetC = 24.0f;
  s.degPerHour = 1.0f;
  e.setSteps(&s, 1);
  e.startProfile(T0, 18.0f, true);  // parte dalla temperatura misurata: 18
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 2 * 3600; in.tempC = 19.5f; in.tempValid = true;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, out.setpointC);
  // Al target ma temperatura lontana: NON avanza (non c'è altro step: non completa).
  in.nowEpoch = T0 + 10 * 3600; in.tempC = 20.0f;
  out = e.tick(in);
  TEST_ASSERT_EQUAL_FLOAT(24.0f, out.setpointC);
  TEST_ASSERT_FALSE(e.completed());
  // Target raggiunto entro tolleranza: completa.
  in.nowEpoch = T0 + 11 * 3600; in.tempC = 23.8f;
  out = e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
}

static void test_profile_gravity_stale_never_advances() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::Gravity;
  s.enabled = true;
  s.targetC = 20.0f;
  s.gravityTarget = 1.010f;
  e.setSteps(&s, 1);
  e.startProfile(T0, 20.0f, true);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 100; in.tempC = 20.0f; in.tempValid = true;
  in.gravity = 1.005f; in.gravityValid = true; in.gravityFresh = false;  // sotto target ma STANTIA
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FALSE(e.completed());
  TEST_ASSERT_TRUE(out.waitingGravity);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, out.setpointC);  // continua a regolare
  in.gravityFresh = true;  // dato fresco sotto target: avanza (qui: completa)
  out = e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
}

static void test_profile_completed_holds_last_setpoint() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[1] = {fixedStep(19.0f, 60)};
  e.setSteps(steps, 1);
  e.startProfile(T0, 19.0f, true);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 61; in.tempC = 19.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_TRUE(out.profileCompleted);
  in.nowEpoch = T0 + 10000;
  out = e.tick(in);
  TEST_ASSERT_TRUE(out.hasSetpoint);
  TEST_ASSERT_EQUAL_FLOAT(19.0f, out.setpointC);
}

static void test_profile_override_cleared_on_step_change() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[2] = {fixedStep(18.0f, 3600), fixedStep(22.0f, 3600)};
  e.setSteps(steps, 2);
  e.startProfile(T0, 18.0f, true);
  e.overrideSetpoint(16.0f);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 100; in.tempC = 18.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_EQUAL_FLOAT(16.0f, out.setpointC);
  in.nowEpoch = T0 + 3601;  // cambio step: override annullato
  out = e.tick(in);
  TEST_ASSERT_EQUAL_FLOAT(22.0f, out.setpointC);
}

static void test_profile_skip() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[2] = {fixedStep(18.0f, 3600), fixedStep(22.0f, 3600)};
  e.setSteps(steps, 2);
  e.startProfile(T0, 18.0f, true);
  TEST_ASSERT_TRUE(e.skip(T0 + 100, 18.0f, true));
  TEST_ASSERT_EQUAL_UINT8(1, e.stepIdx());
  TEST_ASSERT_TRUE(e.skip(T0 + 200, 18.0f, true));  // salta l'ultimo: completa
  TEST_ASSERT_TRUE(e.completed());
}

static void test_profile_delayed_start() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[1] = {fixedStep(18.0f, 3600)};
  e.setSteps(steps, 1);
  e.startProfile(T0, 21.0f, true, T0 + 500);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 100; in.tempC = 21.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_TRUE(out.waitingDelayedStart);
  TEST_ASSERT_FALSE(out.hasSetpoint);  // uscite ferme in attesa
  in.nowEpoch = T0 + 501;
  out = e.tick(in);
  TEST_ASSERT_TRUE(out.hasSetpoint);
  TEST_ASSERT_EQUAL_FLOAT(18.0f, out.setpointC);
  TEST_ASSERT_EQUAL_UINT32(T0 + 501, e.stepStartEpoch());
}

static void test_profile_pause_resume() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampTime;
  s.enabled = true;
  s.fromCurrent = false;
  s.startC = 10.0f;
  s.targetC = 20.0f;
  s.durationSec = 10000;
  e.setSteps(&s, 1);
  e.startProfile(T0, 10.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 12.0f; in.tempValid = true;
  in.nowEpoch = T0 + 5000;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, out.setpointC);
  e.pause(T0 + 5000);
  in.nowEpoch = T0 + 8000;  // in pausa: la rampa resta congelata
  out = e.tick(in);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, out.setpointC);
  e.resume(T0 + 9000);      // 4000 s di pausa
  in.nowEpoch = T0 + 9000;
  out = e.tick(in);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, out.setpointC);  // riprende da dove era
  in.nowEpoch = T0 + 14000; // fine rampa traslata a T0+14000
  out = e.tick(in);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, out.setpointC);
}

// ---------- Regressioni dalla review adversariale ----------

static void test_interlock_blocks_same_tick_reversal() {
  // Il tempo morto vale anche nel tick in cui l'altro lato è ANCORA acceso.
  Interlock il;
  il.configure(120);
  il.update(T0, false, true);  // freddo fisicamente acceso
  Interlock::Result r = il.arbitrate(T0 + 50, {true, false});
  TEST_ASSERT_FALSE(r.out.heat);
  TEST_ASSERT_TRUE(r.switchBlocked);
  // Con minSwitch=0 lo scambio nello stesso tick resta permesso.
  Interlock il0;
  il0.configure(0);
  il0.update(T0, false, true);
  r = il0.arbitrate(T0 + 1, {true, false});
  TEST_ASSERT_TRUE(r.out.heat);
}

static void test_thermostat_setpoint_clamped() {
  // Difesa in profondità: un setpoint fuori [-1,40] (migrazione pre-C17, MQTT climate_mode=auto,
  // +inf da "1e40") viene clampato al range, non memorizzato grezzo — altrimenti romperebbe il JSON
  // di /status o attiverebbe un valore pericoloso.
  ProfileEngine e;
  e.configure(0.5f);
  e.setThermostat(true, 50.0f);
  TEST_ASSERT_EQUAL_FLOAT(40.0f, e.thermostatSetpoint());
  e.setThermostat(true, -10.0f);
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, e.thermostatSetpoint());
  e.setThermostat(false, 18.5f);  // in range: invariato, anche a spegnimento
  TEST_ASSERT_EQUAL_FLOAT(18.5f, e.thermostatSetpoint());
}

static void test_profile_start_preserves_thermostat_setpoint() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[1] = {fixedStep(18.0f, 3600)};
  e.setSteps(steps, 1);
  e.setThermostat(true, 4.0f);  // cold crash in termostato
  e.startProfile(T0, 18.0f, true);
  e.stop();
  TEST_ASSERT_EQUAL_FLOAT(4.0f, e.thermostatSetpoint());
}

static void test_profile_delayed_start_not_while_paused() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[1] = {fixedStep(18.0f, 3600)};
  e.setSteps(steps, 1);
  e.startProfile(T0, 21.0f, true, T0 + 500);
  e.pause(T0 + 100);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 600;  // oltre l'epoch di partenza, ma in pausa
  in.tempC = 21.0f;
  in.tempValid = true;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_TRUE(out.waitingDelayedStart);
  TEST_ASSERT_FALSE(out.hasSetpoint);
  e.resume(T0 + 700);
  in.nowEpoch = T0 + 700;
  out = e.tick(in);
  TEST_ASSERT_TRUE(out.hasSetpoint);
  TEST_ASSERT_EQUAL_UINT32(T0 + 700, e.stepStartEpoch());
}

static void test_engine_shift_clock_keeps_elapsed() {
  // Salto d'orologio (NTP dopo boot con RTC morto): la posizione nella rampa
  // non deve cambiare — niente fast-forward a COMPLETED.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampTime;
  s.enabled = true;
  s.fromCurrent = false;
  s.startC = 10.0f;
  s.targetC = 20.0f;
  s.durationSec = 10000;
  e.setSteps(&s, 1);
  e.startProfile(100, 10.0f, true);  // timeline fittizia (boot senza RTC)
  ProfileEngine::TickInput in{};
  in.tempC = 12.0f;
  in.tempValid = true;
  in.nowEpoch = 5100;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, e.tick(in).setpointC);
  const int64_t jump = (int64_t)T0 - 5100;  // NTP porta l'ora a T0
  e.shiftClock(jump);
  in.nowEpoch = T0;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, out.setpointC);
  TEST_ASSERT_FALSE(e.completed());
}

static void test_guard_shift_clock_keeps_rest() {
  CompressorGuard g;
  g.configure(300, 60);
  g.assumeJustStopped(100);  // boot con RTC inaffidabile
  TEST_ASSERT_FALSE(g.canStart(120));
  g.shiftClock((int64_t)T0 - 120);  // salto NTP
  TEST_ASSERT_FALSE(g.canStart(T0 + 100));  // riposo ancora NON trascorso
  TEST_ASSERT_TRUE(g.canStart(T0 + 301));
}

static void test_profile_eight_steps() {
  // kMaxSteps è passato da 5 a 8: la macchina a stati deve percorrerli tutti.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[8];
  for (int i = 0; i < 8; i++) steps[i] = fixedStep(15.0f + i, 3600);
  e.setSteps(steps, 8);
  TEST_ASSERT_EQUAL_UINT8(8, e.stepCount());
  e.startProfile(T0, 15.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 18.0f;
  in.tempValid = true;
  in.nowEpoch = T0 + 7 * 3600 + 10;  // dentro l'ottavo step
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_EQUAL_UINT8(7, e.stepIdx());
  TEST_ASSERT_EQUAL_FLOAT(22.0f, out.setpointC);
  TEST_ASSERT_FALSE(e.completed());
  in.nowEpoch = T0 + 8 * 3600 + 1;
  out = e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
}

// ---------- Spegnimento predittivo (PeakEstimator) ----------

static void test_estimator_learns_from_cycle() {
  PeakEstimator e;
  e.configureSide(true);  // freddo: la temperatura scende oltre lo spegnimento
  // Marcia di 600 s (temperatura in discesa; con coeff 0 non chiede mai lo stop).
  for (int i = 0; i < 60; i++)
    TEST_ASSERT_FALSE(e.tick(true, 21.0f - i * 0.01f, 20.0f, 10.0f));
  // Spento a 20.4: la temperatura deriva fino a 20.1, poi risale oltre la banda.
  e.tick(false, 20.4f, 20.0f, 1.0f);            // fronte di discesa: apre l'osservazione
  for (int i = 0; i < 30; i++) e.tick(false, 20.4f - i * 0.01f, 20.0f, 10.0f);
  e.tick(false, 20.25f, 20.0f, 10.0f);          // +0.14 sopra il minimo 20.11: chiude
  // Deriva osservata 20.4-20.11=0.29 °C in 600 s di marcia -> 1.74 °C/h; EMA 0.4.
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.4f * 1.74f, e.coefficientCph());
}

static void test_estimator_early_stop_fires() {
  PeakEstimator e;
  e.configureSide(true);
  e.setCoefficientCph(1.8f);
  // Temperatura ferma a 0.25 sopra il set: lo stop scatta quando la deriva prevista
  // arriva a 0.25 °C, cioè dopo 500 s di marcia (1.8 °C/h * 500/3600 = 0.25).
  bool stopped = false;
  float stopAtSec = 0;
  for (int i = 1; i <= 120 && !stopped; i++) {
    stopped = e.tick(true, 20.25f, 20.0f, 10.0f);
    stopAtSec = i * 10.0f;
  }
  TEST_ASSERT_TRUE(stopped);
  TEST_ASSERT_FLOAT_WITHIN(11.0f, 500.0f, stopAtSec);
}

static void test_estimator_short_run_never_stops_nor_learns() {
  PeakEstimator e;
  e.configureSide(true);
  e.setCoefficientCph(10.0f);  // anche col coefficiente al massimo...
  // ...sotto kMinRunSec non chiede mai lo stop.
  TEST_ASSERT_FALSE(e.tick(true, 25.0f, 20.0f, 60.0f));
  // Marcia corta: lo spegnimento non apre l'osservazione, il coefficiente non cambia.
  e.tick(false, 24.0f, 20.0f, 1.0f);
  for (int i = 0; i < 20; i++) e.tick(false, 23.0f, 20.0f, 10.0f);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, e.coefficientCph());
}

static void test_estimator_clamps() {
  PeakEstimator e;
  e.setCoefficientCph(50.0f);
  TEST_ASSERT_EQUAL_FLOAT(PeakEstimator::kMaxCoeffCph, e.coefficientCph());
  e.setCoefficientCph(-3.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, e.coefficientCph());
}

static void test_estimator_heat_side_symmetric() {
  PeakEstimator e;
  e.configureSide(false);  // caldo: la temperatura sale oltre lo spegnimento
  e.setCoefficientCph(1.8f);
  bool stopped = false;
  for (int i = 1; i <= 120 && !stopped; i++)
    stopped = e.tick(true, 19.75f, 20.0f, 10.0f);  // 0.25 sotto il set
  TEST_ASSERT_TRUE(stopped);
}

static void test_pipeline_predictive_early_off_no_rearm() {
  ControlPipeline p;
  ControlPipeline::Cfg c = pipeCfgHyst();
  c.predictive = true;
  c.compressorRestSec = 0;
  p.configure(c);
  p.setEstimators(3.6f, 0.0f);  // deriva prevista: 0.001 °C per secondo di marcia
  // Sopra set+tol: il freddo parte.
  ControlPipeline::Status st = p.tick(T0, 0, true, 20.0f, true, 20.6f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  // Temperatura ferma a 20.4 (sotto set+tol, sopra set): l'isteresi da sola non
  // spegnerebbe MAI. Il predittivo spegne quando la deriva prevista copre 0.4 °C
  // (400 s di marcia) e l'isteresi non deve riarmare a metà banda.
  bool wentOff = false;
  uint32_t offAt = 0;
  for (uint32_t s = 1; s <= 600; s++) {
    st = p.tick(T0 + s, s * 1000, true, 20.0f, true, 20.4f, true, 1.0f);
    if (!st.cool && !wentOff) { wentOff = true; offAt = s; }
    if (wentOff) TEST_ASSERT_FALSE(st.cool);  // niente riaccensione sotto set+tol
  }
  TEST_ASSERT_TRUE(wentOff);
  TEST_ASSERT_TRUE(offAt >= (uint32_t)PeakEstimator::kMinRunSec && offAt <= 450);
  // Alla soglia piena set+tol riparte normalmente.
  st = p.tick(T0 + 601, 601000, true, 20.0f, true, 20.6f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
}

static void test_pipeline_predictive_disabled_is_pure_hysteresis() {
  ControlPipeline p;
  ControlPipeline::Cfg c = pipeCfgHyst();
  c.predictive = false;
  c.compressorRestSec = 0;
  p.configure(c);
  p.setEstimators(10.0f, 10.0f);  // coefficienti alti ma interruttore spento
  ControlPipeline::Status st = p.tick(T0, 0, true, 20.0f, true, 20.6f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  for (uint32_t s = 1; s <= 600; s++) {
    st = p.tick(T0 + s, s * 1000, true, 20.0f, true, 20.4f, true, 1.0f);
    TEST_ASSERT_TRUE(st.cool);  // sopra il set: l'isteresi pura non spegne
  }
}

static void test_pipeline_predictive_pid_heat_untouched() {
  ControlPipeline::Cfg c;
  c.heatUsePid = true;
  c.predictive = true;
  c.toleranceC = 0.5f;
  c.minStateSec = 0;
  c.minSwitchSec = 0;
  c.compressorRestSec = 0;
  c.compressorMinOnSec = 0;
  ControlPipeline p;
  p.configure(c);
  p.setEstimators(0.0f, 10.0f);  // stimatore caldo carico: col PID va ignorato
  // Molto sotto il set: piena potenza PID, per oltre kMinRunSec.
  for (uint32_t s = 0; s <= 300; s++) {
    ControlPipeline::Status st = p.tick(T0 + s, 3000, true, 20.0f, true, 10.0f, true, 1.0f);
    TEST_ASSERT_TRUE(st.heat);
    TEST_ASSERT_FALSE(st.predStop);
  }
}

// ---------- Regressioni dalla review adversariale del predittivo ----------

static void test_estimator_single_sample_cannot_saturate() {
  // Un campione anomalo (marcia minima, deriva enorme) non deve saturare il
  // coefficiente in un colpo: campione clampato a 10 e passo massimo 1 °C/h.
  PeakEstimator e;
  e.configureSide(true);
  e.setCoefficientCph(2.0f);
  for (int i = 0; i < 12; i++) e.tick(true, 25.0f, 20.0f, 10.0f);  // marcia 120 s
  e.tick(false, 25.0f, 20.0f, 1.0f);                               // stop, osservazione
  for (int i = 0; i < 6; i++) e.tick(false, 25.0f - (i + 1) * 0.2f, 20.0f, 30.0f);
  e.tick(false, 24.0f, 20.0f, 30.0f);   // minimo 23.8 (osservati 210 s)...
  e.tick(false, 23.95f, 20.0f, 30.0f);  // ...inversione: chiude (deriva 1.2 °C in 120 s)
  // obsCph grezzo = 1.2/(120/3600) = 36 -> clamp 10; passo 0.4*(10-2)=3.2 -> clamp 1.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f, e.coefficientCph());
}

static void test_estimator_interrupted_observation_discarded() {
  // Marcia che riparte subito dopo lo stop: l'osservazione (durata ~0) non ha
  // misurato nulla e NON deve erodere il coefficiente appreso.
  PeakEstimator e;
  e.configureSide(true);
  e.setCoefficientCph(2.0f);
  for (int i = 0; i < 13; i++) e.tick(true, 25.0f, 20.0f, 10.0f);  // marcia 130 s
  e.tick(false, 20.3f, 20.0f, 1.0f);   // stop: apre l'osservazione
  e.tick(true, 20.3f, 20.0f, 1.0f);    // riparte subito: deriva osservata 0, scarta
  TEST_ASSERT_EQUAL_FLOAT(2.0f, e.coefficientCph());
}

static void test_estimator_quick_noise_reversal_discarded() {
  // 2 LSB di rumore DS18B20 (0.125 °C) pochi secondi dopo lo stop superano la
  // banda d'inversione: l'osservazione è troppo corta per essere fisica e NON
  // deve erodere il coefficiente con una deriva ~0.
  PeakEstimator e;
  e.configureSide(true);
  e.setCoefficientCph(2.0f);
  for (int i = 0; i < 30; i++) e.tick(true, 20.6f, 20.0f, 10.0f);  // marcia 300 s
  e.tick(false, 20.3f, 20.0f, 1.0f);            // stop
  e.tick(false, 20.175f, 20.0f, 1.0f);          // -0.125: rumore
  e.tick(false, 20.3f, 20.0f, 1.0f);            // +0.125: inversione a 3 s -> scarta
  TEST_ASSERT_EQUAL_FLOAT(2.0f, e.coefficientCph());
}

static void test_estimator_ema_decreases_after_overestimate() {
  // Deriva reale piccola con coefficiente alto: il coefficiente deve SCENDERE,
  // al massimo di 1 °C/h per ciclo.
  PeakEstimator e;
  e.configureSide(true);
  e.setCoefficientCph(5.0f);
  for (int i = 0; i < 60; i++) e.tick(true, 20.6f, 20.0f, 10.0f);  // marcia 600 s
  e.tick(false, 20.55f, 20.0f, 1.0f);
  for (int i = 0; i < 30; i++) e.tick(false, 20.45f, 20.0f, 10.0f);  // minimo stabile
  e.tick(false, 20.56f, 20.0f, 10.0f);  // inversione: deriva 0.1 -> obsCph 0.6
  // passo = 0.4*(0.6-5) = -1.76 -> clamp -1.0 -> 4.0
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.0f, e.coefficientCph());
}

static void test_estimator_observe_timeout_learns() {
  // Deriva lentissima che non inverte mai: il timeout (1800 s) chiude comunque
  // l'osservazione e la lezione viene appresa.
  PeakEstimator e;
  e.configureSide(true);
  for (int i = 0; i < 60; i++) e.tick(true, 21.0f - i * 0.01f, 20.0f, 10.0f);  // 600 s
  e.tick(false, 20.4f, 20.0f, 1.0f);
  for (int i = 1; i <= 18; i++) e.tick(false, 20.4f - i * 0.005f, 20.0f, 100.0f);
  // Chiusa al 18° tick (1800 s): deriva 0.09 -> obsCph 0.54 -> coeff 0.4*0.54.
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.216f, e.coefficientCph());
}

static void test_pipeline_force_off_respects_min_state() {
  // Con minStateSec=300 lo stop anticipato deve aspettare il tempo minimo di stato.
  ControlPipeline p;
  ControlPipeline::Cfg c = pipeCfgHyst();
  c.predictive = true;
  c.minStateSec = 300;
  c.compressorRestSec = 0;
  p.configure(c);
  p.setEstimators(10.0f, 0.0f);
  ControlPipeline::Status st = p.tick(T0, 0, true, 20.0f, true, 20.6f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  bool onAt250 = false, offAt301 = false;
  for (uint32_t s = 1; s <= 301; s++) {
    st = p.tick(T0 + s, s * 1000, true, 20.0f, true, 20.4f, true, 1.0f);
    if (s == 250) onAt250 = st.cool;   // condizione predittiva già vera, ma minState no
    if (s == 301) offAt301 = !st.cool; // trascorso il minimo: stop applicato
  }
  TEST_ASSERT_TRUE(onAt250);
  TEST_ASSERT_TRUE(offAt301);
}

static void test_pipeline_estimators_learn_even_when_predictive_off() {
  // Gli stimatori seguono sempre il relè: a predittivo spento imparano comunque
  // (niente stato stantio quando poi lo si riattiva), ma non spengono mai.
  ControlPipeline p;
  ControlPipeline::Cfg c = pipeCfgHyst();
  c.predictive = false;
  c.compressorRestSec = 0;
  p.configure(c);
  ControlPipeline::Status st = p.tick(T0, 0, true, 20.0f, true, 20.6f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  uint32_t s = 1;
  for (; s <= 300; s++) {  // marcia 300 s
    st = p.tick(T0 + s, s * 1000, true, 20.0f, true, 20.6f, true, 1.0f);
    TEST_ASSERT_TRUE(st.cool);
  }
  st = p.tick(T0 + s, s * 1000, true, 20.0f, true, 19.9f, true, 1.0f);  // sotto set: off
  TEST_ASSERT_FALSE(st.cool);
  s++;
  for (int i = 0; i < 200; i++, s++)  // deriva fino a 19.7 in 200 s (> kMinObserveSec)
    p.tick(T0 + s, s * 1000, true, 20.0f, true, 19.9f - (i + 1) * 0.001f, true, 1.0f);
  p.tick(T0 + s, s * 1000, true, 20.0f, true, 19.85f, true, 1.0f);  // inversione 0.15
  TEST_ASSERT_TRUE(p.coolEstimatorCph() > 0.5f);  // ha imparato (deriva 0.2 su 301 s)
}

static void test_pipeline_opposite_side_discards_observation() {
  // Il caldo che parte mentre si osserva la deriva post-freddo contamina il
  // campione: va scartato, il coefficiente freddo non deve cambiare.
  ControlPipeline p;
  ControlPipeline::Cfg c = pipeCfgHyst();  // heatUsePid=false: caldo a isteresi
  c.predictive = true;
  c.compressorRestSec = 0;
  p.configure(c);
  ControlPipeline::Status st = p.tick(T0, 0, true, 20.0f, true, 20.6f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  uint32_t s = 1;
  for (; s <= 200; s++) p.tick(T0 + s, s * 1000, true, 20.0f, true, 20.6f, true, 1.0f);
  p.tick(T0 + s, s * 1000, true, 20.0f, true, 19.9f, true, 1.0f);  // freddo off, osserva
  s++;
  for (int i = 0; i < 50; i++, s++) {  // crolla fino a 19.4: parte il CALDO
    st = p.tick(T0 + s, s * 1000, true, 20.0f, true, 19.9f - (i + 1) * 0.01f, true, 1.0f);
  }
  TEST_ASSERT_TRUE(st.heat);  // sotto set-tol il caldo è legittimo
  for (int i = 0; i < 30; i++, s++)  // risalita spinta dal caldo
    p.tick(T0 + s, s * 1000, true, 20.0f, true, 19.4f + i * 0.02f, true, 1.0f);
  // Senza lo scarto, la risalita verrebbe letta come inversione naturale e il
  // coefficiente freddo imparerebbe una deriva di ~0.5 °C. Deve restare 0.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, p.coolEstimatorCph());
}

static void test_pipeline_estimators_roundtrip() {
  ControlPipeline p;
  p.configure(pipeCfgHyst());
  p.setEstimators(1.5f, 0.8f);
  TEST_ASSERT_EQUAL_FLOAT(1.5f, p.coolEstimatorCph());
  TEST_ASSERT_EQUAL_FLOAT(0.8f, p.heatEstimatorCph());
  // Una riconfigurazione (salvataggio impostazioni) non azzera l'appreso.
  p.configure(pipeCfgHyst());
  TEST_ASSERT_EQUAL_FLOAT(1.5f, p.coolEstimatorCph());
}

// ---------- Nuove condizioni di step (v2.12: libreria profili ampliata) ----------

static StepDef gravStep(float tC, ax::GravityMode gm, float target, uint32_t maxDur = 0) {
  StepDef s;
  s.type = StepType::Gravity;
  s.enabled = true;
  s.targetC = tC;
  s.gravityMode = gm;
  s.gravityTarget = target;
  s.maxDurationSec = maxDur;
  return s;
}

static void test_gravity_attenuation_trigger() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = gravStep(18.0f, ax::GravityMode::Attenuation, 50.0f);
  e.setSteps(&s, 1);
  e.startProfile(T0, 18.0f, true);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 100; in.tempC = 18.0f; in.tempValid = true;
  in.gravity = 1.030f; in.gravityValid = true; in.gravityFresh = true;
  in.attenuationPct = 45.0f; in.attenuationValid = true;
  e.tick(in);
  TEST_ASSERT_FALSE(e.completed());  // 45% < 50%
  in.attenuationPct = 55.0f;
  e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
}

static void test_gravity_attenuation_without_og_waits_then_timeout() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = gravStep(18.0f, ax::GravityMode::Attenuation, 50.0f, 7200);
  e.setSteps(&s, 1);
  e.startProfile(T0, 18.0f, true);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 100; in.tempC = 18.0f; in.tempValid = true;
  in.gravity = 1.010f; in.gravityValid = true; in.gravityFresh = true;
  in.attenuationValid = false;  // OG mai catturata: attenuazione incalcolabile
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FALSE(e.completed());
  TEST_ASSERT_TRUE(out.waitingGravity);
  in.nowEpoch = T0 + 7201;  // il timeout di sicurezza sblocca comunque
  e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
}

static void test_gravity_timeout_beats_stale_sensor() {
  // iSpindel muto/incrostato: dato stantio per sempre, ma il timeout avanza lo step.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = gravStep(20.0f, ax::GravityMode::Below, 1.010f, 3 * 86400);
  e.setSteps(&s, 1);
  e.startProfile(T0, 20.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 20.0f; in.tempValid = true;
  in.gravity = 1.030f; in.gravityValid = true; in.gravityFresh = false;  // STANTIA
  in.nowEpoch = T0 + 2 * 86400;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FALSE(e.completed());
  TEST_ASSERT_TRUE(out.waitingGravity);
  in.nowEpoch = T0 + 3 * 86400 + 1;
  e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
}

static void test_gravity_nessuna_sorgente_non_scade_mai() {
  // D7: camera SENZA densimetro assegnato. Il timeout non deve avanzare lo step:
  // avanzerebbe alla cieca, senza aver mai misurato, mandando tipicamente la
  // camera al cold crash su una fermentazione ancora attiva.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = gravStep(20.0f, ax::GravityMode::Below, 1.010f, 3 * 86400);
  e.setSteps(&s, 1);
  e.startProfile(T0, 20.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 20.0f; in.tempValid = true;
  in.gravityValid = false; in.gravityFresh = false;  // nessuna lettura, mai
  in.gravitySourceMissing = true;                    // ...e nessuna sorgente assegnata
  in.nowEpoch = T0 + 3 * 86400 + 1;                  // ben oltre il timeout
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FALSE(e.completed());
  TEST_ASSERT_TRUE(out.waitingGravity);
  in.nowEpoch = T0 + 30 * 86400;  // un mese dopo: ancora fermo
  out = e.tick(in);
  TEST_ASSERT_FALSE(e.completed());
  TEST_ASSERT_TRUE(out.waitingGravity);
}

static void test_gravity_sorgente_presente_ma_muta_scade_ancora() {
  // Il contrario del precedente: sorgente ASSEGNATA ma muta. Qui il timeout deve
  // continuare a funzionare come prima — e' la rete di sicurezza per cui esiste.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = gravStep(20.0f, ax::GravityMode::Below, 1.010f, 3 * 86400);
  e.setSteps(&s, 1);
  e.startProfile(T0, 20.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 20.0f; in.tempValid = true;
  in.gravityValid = false; in.gravityFresh = false;
  in.gravitySourceMissing = false;  // assegnata, ma non trasmette
  in.nowEpoch = T0 + 3 * 86400 + 1;
  e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
}

static void test_gravity_sorgente_assente_ma_dato_valido_avanza() {
  // Caso limite: la sorgente risulta "assente" ma una lettura valida c'e' comunque
  // (es. OG immessa a mano e densimetro riassegnato). La condizione di densita'
  // deve poter essere soddisfatta: il flag disabilita SOLO la scorciatoia del timeout.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = gravStep(20.0f, ax::GravityMode::Below, 1.010f, 3 * 86400);
  e.setSteps(&s, 1);
  e.startProfile(T0, 20.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 20.0f; in.tempValid = true;
  in.gravity = 1.008f; in.gravityValid = true; in.gravityFresh = true;
  in.gravitySourceMissing = true;
  in.nowEpoch = T0 + 3600;
  e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
}

static void test_due_camere_densimetri_indipendenti() {
  // D7, il cuore: due motori con densita' DIVERSE non si contaminano.
  // La camera A e' a 1.040 (fermenta ancora), la B a 1.008 (finita).
  ProfileEngine a, b;
  a.configure(0.5f); b.configure(0.5f);
  StepDef sa = gravStep(20.0f, ax::GravityMode::Below, 1.010f, 0);
  StepDef sb = gravStep(20.0f, ax::GravityMode::Below, 1.010f, 0);
  a.setSteps(&sa, 1); b.setSteps(&sb, 1);
  a.startProfile(T0, 20.0f, true); b.startProfile(T0, 20.0f, true);
  ProfileEngine::TickInput ia{}, ib{};
  ia.tempC = ib.tempC = 20.0f; ia.tempValid = ib.tempValid = true;
  ia.nowEpoch = ib.nowEpoch = T0 + 3600;
  ia.gravity = 1.040f; ia.gravityValid = true; ia.gravityFresh = true;
  ib.gravity = 1.008f; ib.gravityValid = true; ib.gravityFresh = true;
  a.tick(ia); b.tick(ib);
  TEST_ASSERT_FALSE(a.completed());  // A deve restare ferma
  TEST_ASSERT_TRUE(b.completed());   // B deve avanzare
}

static void test_gravity_stable_window() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = gravStep(19.0f, ax::GravityMode::Stable, 0.0f);
  s.stableBand = 0.002f;
  s.stableHours = 1.0f;  // finestra corta per il test
  e.setSteps(&s, 1);
  e.startProfile(T0, 19.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 19.0f; in.tempValid = true;
  in.gravityValid = true; in.gravityFresh = true;
  // Densità in calo: ogni uscita dalla banda fa ripartire la finestra.
  in.gravity = 1.020f; in.nowEpoch = T0 + 1; e.tick(in);
  in.gravity = 1.017f; in.nowEpoch = T0 + 1800; e.tick(in);  // -3 punti: reset
  in.gravity = 1.017f; in.nowEpoch = T0 + 1800 + 3599; e.tick(in);
  TEST_ASSERT_FALSE(e.completed());  // stabile da 3599 s < 3600
  in.nowEpoch = T0 + 1800 + 3601; e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
}

static void test_gravity_stable_survives_restart() {
  StepDef s = gravStep(19.0f, ax::GravityMode::Stable, 0.0f);
  s.stableBand = 0.002f;
  s.stableHours = 1.0f;
  ProfileEngine e;
  e.configure(0.5f);
  e.setSteps(&s, 1);
  e.startProfile(T0, 19.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 19.0f; in.tempValid = true;
  in.gravityValid = true; in.gravityFresh = true;
  in.gravity = 1.015f; in.nowEpoch = T0 + 1; e.tick(in);      // apre la finestra
  in.nowEpoch = T0 + 1800; e.tick(in);                        // 30 min stabile
  EngineSnapshot snap = e.snapshot();

  ProfileEngine e2;  // riavvio: la stabilità maturata non si perde
  e2.configure(0.5f);
  e2.setSteps(&s, 1);
  e2.restore(snap);
  in.nowEpoch = T0 + 3602; e2.tick(in);  // 3601 s dalla prima lettura in banda
  TEST_ASSERT_TRUE(e2.completed());
}

static void test_stable_window_shifted_by_pause() {
  // Il tempo di pausa NON vale come stabilità osservata: la finestra trasla.
  StepDef s = gravStep(19.0f, ax::GravityMode::Stable, 0.0f);
  s.stableBand = 0.002f;
  s.stableHours = 1.0f;
  ProfileEngine e;
  e.configure(0.5f);
  e.setSteps(&s, 1);
  e.startProfile(T0, 19.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 19.0f; in.tempValid = true;
  in.gravityValid = true; in.gravityFresh = true; in.gravity = 1.012f;
  in.nowEpoch = T0 + 1; e.tick(in);       // apre la finestra
  in.nowEpoch = T0 + 600; e.tick(in);     // 10 min in banda
  e.pause(T0 + 600);
  e.resume(T0 + 600 + 3600);              // 1 h di pausa
  in.nowEpoch = T0 + 600 + 3600 + 10; e.tick(in);
  TEST_ASSERT_FALSE(e.completed());       // senza fix: 1+3600 s "stabili" -> completo
  in.nowEpoch = T0 + 1 + 3600 + 3600 + 10; e.tick(in);  // 1 h REALE in banda
  TEST_ASSERT_TRUE(e.completed());
}

static void test_skip_while_paused_reanchors() {
  // Skip in pausa: alla ripresa lo step NON deve avere inizio nel futuro.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[2] = {fixedStep(18.0f, 3600), fixedStep(22.0f, 3600)};
  e.setSteps(steps, 2);
  e.startProfile(T0, 18.0f, true);
  e.pause(T0 + 100);
  TEST_ASSERT_TRUE(e.skip(T0 + 200, 18.0f, true));  // skip durante la pausa
  e.resume(T0 + 500);
  // Senza fix: pausedFor=400 -> stepStart=200+400=600 > 500 (futuro).
  TEST_ASSERT_EQUAL_UINT32(T0 + 500, e.stepStartEpoch());
  ProfileEngine::TickInput in{};
  in.tempC = 22.0f; in.tempValid = true;
  in.nowEpoch = T0 + 500 + 3601;
  e.tick(in);
  TEST_ASSERT_TRUE(e.completed());  // il fixed da 1 h scade dopo 1 h reale
}

static void test_step_actuation_policy_output() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = fixedStep(20.0f, 3600);
  s.allowHeat = false;  // free-rise con tetto
  e.setSteps(&s, 1);
  e.startProfile(T0, 20.0f, true);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 10; in.tempC = 20.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FALSE(out.allowHeat);
  TEST_ASSERT_TRUE(out.allowCool);
  // Fuori dal profilo (termostato) la politica torna piena.
  e.setThermostat(true, 18.0f);
  out = e.tick(in);
  TEST_ASSERT_TRUE(out.allowHeat);
  TEST_ASSERT_TRUE(out.allowCool);
}

static void test_pipeline_policy_masks_outputs() {
  ControlPipeline p;
  ControlPipeline::Cfg c = pipeCfgHyst();
  c.compressorRestSec = 0;
  p.configure(c);
  // Caldo vietato: sotto set-tol non scalda comunque.
  ControlPipeline::Status st =
      p.tick(T0, 0, true, 20.0f, true, 19.0f, true, 1.0f, false, true);
  TEST_ASSERT_FALSE(st.heat);
  // Freddo vietato: sopra set+tol non raffredda comunque.
  st = p.tick(T0 + 1, 1000, true, 20.0f, true, 21.0f, true, 1.0f, true, false);
  TEST_ASSERT_FALSE(st.cool);
  // Politica piena: comportamento normale.
  st = p.tick(T0 + 2, 2000, true, 20.0f, true, 21.0f, true, 1.0f, true, true);
  TEST_ASSERT_TRUE(st.cool);
}

static void test_profile_twelve_steps() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[12];
  for (int i = 0; i < 12; i++) steps[i] = fixedStep(10.0f + i, 3600);
  e.setSteps(steps, 12);
  TEST_ASSERT_EQUAL_UINT8(12, e.stepCount());
  e.startProfile(T0, 10.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 15.0f; in.tempValid = true;
  in.nowEpoch = T0 + 11 * 3600 + 10;  // dentro il 12° step
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_EQUAL_UINT8(11, e.stepIdx());
  TEST_ASSERT_EQUAL_FLOAT(21.0f, out.setpointC);
  in.nowEpoch = T0 + 12 * 3600 + 1;
  e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
}

// ---------- Regressioni dalle correzioni dei rilievi critici ----------

// C12: i salti d'orologio traslano i latch di isteresi e interlock, altrimenti un
// salto all'indietro li congela per la sua intera ampiezza.
static void test_hysteresis_shift_clock_backward() {
  HysteresisController h;
  h.configure(0.5f, 60);
  OutputRequest o = h.evaluate(T0, 20.0f, 20.6f);  // freddo acceso, coolChangedAt_=T0
  TEST_ASSERT_TRUE(o.cool);
  h.shiftClock(-1000);  // l'orologio torna indietro di 1000 s
  // Sulla timeline nuova, 61 s dopo il cambio traslato, la temperatura sotto set deve
  // poter spegnere. Senza shiftClock elapsed sarebbe -939 e il latch resterebbe acceso.
  o = h.evaluate(T0 - 1000 + 61, 20.0f, 19.9f);
  TEST_ASSERT_FALSE(o.cool);
}

static void test_interlock_shift_clock_backward() {
  Interlock il;
  il.configure(120);
  il.update(T0, false, true);         // freddo acceso
  il.update(T0 + 100, false, false);  // freddo spento a T0+100 (coolOffAt_=T0+100)
  il.shiftClock(-1000);               // salto indietro: coolOffAt_ -> T0-900
  Interlock::Result r = il.arbitrate(T0 - 900 + 121, {true, false});
  TEST_ASSERT_TRUE(r.out.heat);       // 121 s dopo lo spegnimento traslato: caldo ok
}

static void test_pipeline_shift_clock_unfreezes_latch() {
  ControlPipeline p;
  ControlPipeline::Cfg c = pipeCfgHyst();
  c.minStateSec = 60;
  c.compressorRestSec = 0;
  p.configure(c);
  ControlPipeline::Status st = p.tick(T0, 0, true, 20.0f, true, 20.6f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  p.shiftClock(-3600);  // NTP corregge un RTC avanti di un'ora
  // Sulla timeline nuova, temperatura sotto set: il freddo deve spegnersi dopo
  // minStateSec, non restare congelato per l'ampiezza del salto.
  st = p.tick(T0 - 3600 + 61, 61000, true, 20.0f, true, 19.8f, true, 1.0f);
  TEST_ASSERT_FALSE(st.cool);
}

// C14: lo step RampRate onora maxDurationSec come timeout di sicurezza, e segnala lo
// stallo quando il setpoint è al target ma la temperatura non arriva.
static void test_profile_ramprate_safety_timeout() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampRate;
  s.enabled = true;
  s.targetC = 24.0f;
  s.degPerHour = 1.0f;             // rampa nominale 18->24 = 6 h
  s.maxDurationSec = 8 * 3600;     // timeout di sicurezza oltre la grazia di stallo
  e.setSteps(&s, 1);
  e.startProfile(T0, 18.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 20.0f; in.tempValid = true;  // l'impianto si ferma a 20, non arriva a 24
  in.nowEpoch = T0 + 8 * 3600 - 10;       // prima del timeout: stallo visibile, non completo
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FALSE(e.completed());
  TEST_ASSERT_TRUE(out.waitingRampStall);
  in.nowEpoch = T0 + 8 * 3600 + 1;        // timeout scaduto: avanza (qui completa)
  out = e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
  TEST_ASSERT_FALSE(out.waitingRampStall);  // completato per timeout: niente falso stallo
}

static void test_profile_ramprate_stall_flag() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampRate;
  s.enabled = true;
  s.targetC = 24.0f;
  s.degPerHour = 1.0f;  // rampa nominale 18->24 = 6 h; nessun maxDurationSec
  e.setSteps(&s, 1);
  e.startProfile(T0, 18.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 20.0f; in.tempValid = true;  // l'impianto si ferma a 20, non arriva a 24
  in.nowEpoch = T0 + 3 * 3600;            // durante la rampa: nessuno stallo
  TEST_ASSERT_FALSE(e.tick(in).waitingRampStall);
  in.nowEpoch = T0 + 6 * 3600 + 60;       // subito dopo la fine nominale: grazia 1 h
  TEST_ASSERT_FALSE(e.tick(in).waitingRampStall);
  in.nowEpoch = T0 + 7 * 3600 + 5;        // 1 h oltre e ancora fuori tolleranza: stallo
  TEST_ASSERT_TRUE(e.tick(in).waitingRampStall);
  TEST_ASSERT_FALSE(e.completed());       // senza timeout non avanza da solo
}

// ---------- GravityTracker / SafetyMonitor ----------

static void test_gravity_staleness() {
  GravityTracker g;
  g.configure(3600, 0.0f);
  TEST_ASSERT_FALSE(g.isFresh(T0));
  g.update(1.020f, 3.9f, 19.0f, T0);
  TEST_ASSERT_TRUE(g.isFresh(T0 + 3599));
  TEST_ASSERT_FALSE(g.isFresh(T0 + 3601));
  TEST_ASSERT_EQUAL_UINT32(100, g.ageSec(T0 + 100));
}

static void test_safety_monitor_rules() {
  SafetyMonitor sm;
  sm.notArmed = false;
  TEST_ASSERT_TRUE(sm.outputsAllowed());
  sm.controlProbeFault = true;
  TEST_ASSERT_FALSE(sm.outputsAllowed());
  sm.controlProbeFault = false;
  sm.rtcUntrusted = true;  // solo allarme: le uscite restano consentite
  TEST_ASSERT_TRUE(sm.outputsAllowed());
  TEST_ASSERT_TRUE(sm.anyAlarm());
  sm.notArmed = true;
  TEST_ASSERT_FALSE(sm.outputsAllowed());
}

// Ripresa con orologio inaffidabile: reanchorCurrentRamp fa continuare la rampa dalla
// temperatura reale corrente (non dall'origine, non dal target), per ogni tipo di rampa.
static void test_reanchor_ramprate_continues_from_current() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampRate; s.enabled = true; s.targetC = 2.0f; s.degPerHour = 1.0f;
  e.setSteps(&s, 1);
  e.startProfile(T0, 18.0f, true);
  e.reanchorCurrentRamp(T0 + 100000, 8.0f);  // ripresa: birra a 8
  TEST_ASSERT_EQUAL_UINT32(T0 + 100000, e.snapshot().stepStartEpoch);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 100000; in.tempC = 8.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 8.0f, out.setpointC);   // riparte da 8, non da 18 né da 2
  TEST_ASSERT_FALSE(out.waitingRampStall);                // span=|2-8|=6h: niente falso stallo
  in.nowEpoch = T0 + 100000 + 3600;
  out = e.tick(in);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 7.0f, out.setpointC);   // scende di 1 °C in 1 h
}

static void test_reanchor_bails_on_completed_and_nonramp() {
  // Non deve ri-ancorare un profilo completato (romperebbe l'hold al target) né uno step
  // non-rampa (a cui la finestra di boot puo' essere avanzata): stepStartEpoch resta invariato.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[1] = {fixedStep(2.0f, 60)};  // Fixed, non rampa
  e.setSteps(steps, 1);
  e.startProfile(T0, 2.0f, true);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 61; in.tempC = 2.0f; in.tempValid = true;
  e.tick(in);
  TEST_ASSERT_TRUE(e.completed());
  const uint32_t se = e.snapshot().stepStartEpoch;
  e.reanchorCurrentRamp(T0 + 100000, 18.0f);  // completato + non-rampa: no-op
  TEST_ASSERT_EQUAL_UINT32(se, e.snapshot().stepStartEpoch);
}

static void test_reanchor_ramptime_fixedstart() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampTime; s.enabled = true; s.fromCurrent = false;
  s.startC = 20.0f; s.targetC = 2.0f; s.durationSec = 5 * 86400;
  e.setSteps(&s, 1);
  e.startProfile(T0, 20.0f, true);
  e.reanchorCurrentRamp(T0 + 100000, 8.0f);  // ripresa a 8: startC aggiornato a 8
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 100000; in.tempC = 8.0f; in.tempValid = true;
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 8.0f, e.tick(in).setpointC);  // riparte da 8, non da 20
}

static void test_reanchor_works_when_paused() {
  // reanchorCurrentRamp è chiamata SOLO al boot, dove pausedAtEpoch è già ri-basato a "now"
  // insieme a stepStartEpoch (main.cpp): niente desync. Un profilo in pausa su una rampa DEVE
  // poter ri-ancorare l'origine alla temperatura reale, altrimenti alla ripresa ripartirebbe
  // dall'origine stantia (es. riscaldando la birra al vecchio start).
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampRate; s.enabled = true; s.targetC = 2.0f; s.degPerHour = 1.0f;
  e.setSteps(&s, 1);
  e.startProfile(T0, 20.0f, true);
  e.pause(T0 + 500);
  e.reanchorCurrentRamp(T0 + 1000, 15.0f);  // in pausa: ri-àncora comunque
  TEST_ASSERT_TRUE(e.snapshot().rampReanchored);
  TEST_ASSERT_EQUAL_FLOAT(15.0f, e.snapshot().stepStartTemp);
  TEST_ASSERT_EQUAL_UINT32(T0 + 1000, e.snapshot().stepStartEpoch);
}

// C14 (regressione): con rate < 0.01 °C/h il setpoint è al target da subito (stepSetpoint),
// quindi lo stallo va segnalato dopo la sola grazia di 1 h, non dopo ~41 giorni.
static void test_profile_ramprate_stall_zero_rate() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s{};
  s.type = StepType::RampRate;
  s.enabled = true;
  s.targetC = 24.0f;
  s.degPerHour = 0.0f;  // rate < 0.01: salto immediato al target
  e.setSteps(&s, 1);
  e.startProfile(T0, 18.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 18.0f; in.tempValid = true;  // non raggiunge mai 24
  in.nowEpoch = T0 + 1800;                // dentro la grazia: nessuno stallo
  TEST_ASSERT_FALSE(e.tick(in).waitingRampStall);
  in.nowEpoch = T0 + 3700;                // oltre 1 h: stallo visibile (non 41 giorni)
  TEST_ASSERT_TRUE(e.tick(in).waitingRampStall);
}

// C19: skip() durante l'attesa di partenza ritardata non deve lasciare delayedStartEpoch
// impostato (il profilo "completato" risorgerebbe all'epoch), e deve restare in attesa
// se avanza a uno step successivo.
static void test_skip_during_delayed_start_no_resurrect() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[1] = {fixedStep(18.0f, 3600)};
  e.setSteps(steps, 1);
  e.startProfile(T0, 21.0f, true, T0 + 500);  // partenza ritardata
  TEST_ASSERT_TRUE(e.skip(T0 + 100, 21.0f, true));
  TEST_ASSERT_TRUE(e.completed());
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 600; in.tempC = 21.0f; in.tempValid = true;  // oltre l'epoch
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_TRUE(e.completed());          // NON risorge
  TEST_ASSERT_FALSE(out.waitingDelayedStart);
}

static void test_skip_during_delayed_start_advances_pending() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[2] = {fixedStep(18.0f, 3600), fixedStep(22.0f, 3600)};
  e.setSteps(steps, 2);
  e.startProfile(T0, 21.0f, true, T0 + 500);
  TEST_ASSERT_TRUE(e.skip(T0 + 100, 21.0f, true));
  TEST_ASSERT_EQUAL_UINT8(1, e.stepIdx());  // avanzato al secondo step
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 200; in.tempC = 21.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_TRUE(out.waitingDelayedStart); // ancora in attesa, non partito subito
  in.nowEpoch = T0 + 501;                    // scatta: parte dal SECONDO step
  out = e.tick(in);
  TEST_ASSERT_TRUE(out.hasSetpoint);
  TEST_ASSERT_EQUAL_FLOAT(22.0f, out.setpointC);
}

// C26: shiftClock non deve traslare profileStartEpoch (istante reale d'inizio cotta),
// altrimenti dopo un blackout con RTC morto + sync NTP finisce nel futuro.
static void test_shift_clock_preserves_profile_start() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[1] = {fixedStep(18.0f, 100000)};
  e.setSteps(steps, 1);
  e.startProfile(T0, 18.0f, true);
  TEST_ASSERT_EQUAL_UINT32(T0, e.snapshot().profileStartEpoch);
  e.shiftClock(1000000);
  TEST_ASSERT_EQUAL_UINT32(T0, e.snapshot().profileStartEpoch);          // NON traslato
  TEST_ASSERT_EQUAL_UINT32(T0 + 1000000, e.snapshot().stepStartEpoch);   // traslato
}

// I flag di solo-allarme (rampStalled, storageFault, come rtcUntrusted/gravityStale)
// NON devono mai bloccare le uscite: un guasto di scrittura o una rampa ferma non
// devono spegnere il compressore. Contratto blindato contro regressioni.
static void test_safety_monitor_alarm_only_flags() {
  SafetyMonitor sm;
  sm.notArmed = false;
  TEST_ASSERT_TRUE(sm.outputsAllowed());
  sm.rampStalled = true;
  TEST_ASSERT_TRUE(sm.outputsAllowed());
  TEST_ASSERT_TRUE(sm.anyAlarm());
  sm.rampStalled = false;
  sm.storageFault = true;
  TEST_ASSERT_TRUE(sm.outputsAllowed());
  TEST_ASSERT_TRUE(sm.anyAlarm());
  sm.storageFault = false;
  sm.controlProbeFault = true;  // questo invece blocca (controprova)
  TEST_ASSERT_FALSE(sm.outputsAllowed());
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hysteresis_thresholds);
  RUN_TEST(test_hysteresis_min_state_time);
  RUN_TEST(test_interlock_never_both);
  RUN_TEST(test_interlock_switch_dead_time);
  RUN_TEST(test_compressor_rest_across_reboot);
  RUN_TEST(test_compressor_stagger_desynchronizes_starts);
  RUN_TEST(test_compressor_stagger_never_shortens_rest);
  RUN_TEST(test_compressor_rtc_untrusted_assumes_not_elapsed);
  RUN_TEST(test_compressor_clock_backwards_blocks);
  RUN_TEST(test_pipeline_probe_fault_kills_outputs);
  RUN_TEST(test_due_camere_il_guasto_di_una_non_ferma_l_altra);
  RUN_TEST(test_due_camere_richieste_opposte_restano_indipendenti);
  RUN_TEST(test_due_camere_guardia_compressore_non_condivisa);
  RUN_TEST(test_pipeline_not_armed_kills_outputs);
  RUN_TEST(test_pipeline_no_short_cycle);
  RUN_TEST(test_pipeline_pid_heat_and_cool_priority);
  RUN_TEST(test_pid_asymmetric_thresholds);
  RUN_TEST(test_pid_window_output);
  RUN_TEST(test_pid_window_phase_offset_desynchronizes);
  RUN_TEST(test_pid_window_phase_default_is_unshifted);
  RUN_TEST(test_profile_fixed_advances);
  RUN_TEST(test_profile_ramp_time_midpoint);
  RUN_TEST(test_profile_resume_after_blackout);
  RUN_TEST(test_profile_blackout_past_multiple_steps);
  RUN_TEST(test_profile_ramp_rate);
  RUN_TEST(test_profile_gravity_stale_never_advances);
  RUN_TEST(test_profile_completed_holds_last_setpoint);
  RUN_TEST(test_profile_override_cleared_on_step_change);
  RUN_TEST(test_profile_skip);
  RUN_TEST(test_profile_delayed_start);
  RUN_TEST(test_profile_pause_resume);
  RUN_TEST(test_gravity_staleness);
  RUN_TEST(test_safety_monitor_rules);
  RUN_TEST(test_interlock_blocks_same_tick_reversal);
  RUN_TEST(test_thermostat_setpoint_clamped);
  RUN_TEST(test_profile_start_preserves_thermostat_setpoint);
  RUN_TEST(test_profile_delayed_start_not_while_paused);
  RUN_TEST(test_engine_shift_clock_keeps_elapsed);
  RUN_TEST(test_guard_shift_clock_keeps_rest);
  RUN_TEST(test_profile_eight_steps);
  RUN_TEST(test_estimator_learns_from_cycle);
  RUN_TEST(test_estimator_early_stop_fires);
  RUN_TEST(test_estimator_short_run_never_stops_nor_learns);
  RUN_TEST(test_estimator_clamps);
  RUN_TEST(test_estimator_heat_side_symmetric);
  RUN_TEST(test_pipeline_predictive_early_off_no_rearm);
  RUN_TEST(test_pipeline_predictive_disabled_is_pure_hysteresis);
  RUN_TEST(test_pipeline_predictive_pid_heat_untouched);
  RUN_TEST(test_pipeline_estimators_roundtrip);
  RUN_TEST(test_estimator_single_sample_cannot_saturate);
  RUN_TEST(test_estimator_interrupted_observation_discarded);
  RUN_TEST(test_estimator_quick_noise_reversal_discarded);
  RUN_TEST(test_estimator_ema_decreases_after_overestimate);
  RUN_TEST(test_estimator_observe_timeout_learns);
  RUN_TEST(test_pipeline_force_off_respects_min_state);
  RUN_TEST(test_pipeline_estimators_learn_even_when_predictive_off);
  RUN_TEST(test_pipeline_opposite_side_discards_observation);
  RUN_TEST(test_gravity_attenuation_trigger);
  RUN_TEST(test_gravity_attenuation_without_og_waits_then_timeout);
  RUN_TEST(test_gravity_timeout_beats_stale_sensor);
  RUN_TEST(test_gravity_nessuna_sorgente_non_scade_mai);
  RUN_TEST(test_gravity_sorgente_presente_ma_muta_scade_ancora);
  RUN_TEST(test_gravity_sorgente_assente_ma_dato_valido_avanza);
  RUN_TEST(test_due_camere_densimetri_indipendenti);
  RUN_TEST(test_gravity_stable_window);
  RUN_TEST(test_gravity_stable_survives_restart);
  RUN_TEST(test_stable_window_shifted_by_pause);
  RUN_TEST(test_skip_while_paused_reanchors);
  RUN_TEST(test_step_actuation_policy_output);
  RUN_TEST(test_pipeline_policy_masks_outputs);
  RUN_TEST(test_profile_twelve_steps);
  RUN_TEST(test_hysteresis_shift_clock_backward);
  RUN_TEST(test_interlock_shift_clock_backward);
  RUN_TEST(test_pipeline_shift_clock_unfreezes_latch);
  RUN_TEST(test_profile_ramprate_safety_timeout);
  RUN_TEST(test_profile_ramprate_stall_flag);
  RUN_TEST(test_profile_ramprate_stall_zero_rate);
  RUN_TEST(test_reanchor_ramprate_continues_from_current);
  RUN_TEST(test_reanchor_ramptime_fixedstart);
  RUN_TEST(test_reanchor_bails_on_completed_and_nonramp);
  RUN_TEST(test_reanchor_works_when_paused);
  RUN_TEST(test_skip_during_delayed_start_no_resurrect);
  RUN_TEST(test_skip_during_delayed_start_advances_pending);
  RUN_TEST(test_shift_clock_preserves_profile_start);
  RUN_TEST(test_safety_monitor_alarm_only_flags);
  return UNITY_END();
}
