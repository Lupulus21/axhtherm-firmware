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
#include "ax/HeaterGuard.h"
#include "ax/SlopeEstimator.h"
#include "ax/AirLimit.h"
#include "ax/CoolWatch.h"
#include "ax/RetryPenalty.h"
#include "ax/FwImageCheck.h"
#include "ax/ProbeFault.h"
#include "ax/ProbeOffsets.h"
#include "ax/CrashMonitor.h"
#include "ax/MinMaxGroup.h"
#include "ax/EventRing.h"
#include "ax/GravityTrend.h"
#include "ax/FanControl.h"
#include <strings.h>
#include <stdio.h>
#include <string>
#include <vector>

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
  // Blackout di 2h30 (dalla 3.12.0, rilievo CT-16): lo step 1 e' scaduto durante
  // l'interruzione. Si avanza di UNO solo e lo step 2 parte da adesso: la sosta a 20 °C non
  // e' mai avvenuta, con il controllo spento, e non va data per fatta.
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 9000; in.tempC = 20.0f; in.tempValid = true;
  ProfileEngine::TickOutput out = e2.tick(in);
  TEST_ASSERT_EQUAL_UINT8(1, e2.stepIdx());
  TEST_ASSERT_EQUAL_UINT32(T0 + 9000, e2.stepStartEpoch());
  TEST_ASSERT_EQUAL_FLOAT(20.0f, out.setpointC);
  TEST_ASSERT_FALSE(e2.completed());
  // Il tick dopo non avanza ancora: lo step 2 ha appena cominciato.
  in.nowEpoch = T0 + 9001;
  e2.tick(in);
  TEST_ASSERT_EQUAL_UINT8(1, e2.stepIdx());
}

static void test_profile_interruzione_breve_tiene_il_passo() {
  // Un'interruzione sotto i 10 minuti (riavvio, aggiornamento) non sposta gli step: il
  // successivo parte dall'istante in cui il precedente e' scaduto davvero.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[2] = {fixedStep(18.0f, 3600), fixedStep(20.0f, 3600)};
  e.setSteps(steps, 2);
  e.startProfile(T0, 18.0f, true);
  ProfileEngine::TickInput in{};
  in.nowEpoch = T0 + 3600 + 300; in.tempC = 18.0f; in.tempValid = true;
  e.tick(in);
  TEST_ASSERT_EQUAL_UINT8(1, e.stepIdx());
  TEST_ASSERT_EQUAL_UINT32(T0 + 3600, e.stepStartEpoch());
}

static void test_profile_attendi_il_target() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef steps[2] = {fixedStep(18.0f, 2 * 3600), fixedStep(20.0f, 3600)};
  steps[0].waitTarget = true;
  e.setSteps(steps, 2);
  e.startProfile(T0, 22.0f, true);  // mosto a 22 °C, step a 18
  ProfileEngine::TickInput in{};
  in.tempValid = true;
  // Dopo 3 ore il mosto e' a 19: ancora fuori tolleranza, il tempo non conta.
  in.nowEpoch = T0 + 3 * 3600; in.tempC = 19.0f;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_TRUE(out.waitingTarget);
  TEST_ASSERT_EQUAL_UINT8(0, e.stepIdx());
  TEST_ASSERT_EQUAL_FLOAT(18.0f, out.setpointC);
  TEST_ASSERT_EQUAL_UINT32(0, e.stepEndsAtEpoch());
  // Il mosto arriva a 18,3: da qui partono le 2 ore dello step.
  in.nowEpoch = T0 + 4 * 3600; in.tempC = 18.3f;
  out = e.tick(in);
  TEST_ASSERT_FALSE(out.waitingTarget);
  TEST_ASSERT_TRUE(out.stepChanged);
  TEST_ASSERT_EQUAL_UINT32(T0 + 4 * 3600, e.stepStartEpoch());
  TEST_ASSERT_FALSE(e.snapshot().waitTimedOut);
  // Il riavvio non perde il punto: lo stato si salva e si ripristina.
  ProfileEngine e2;
  e2.configure(0.5f);
  e2.setSteps(steps, 2);
  e2.restore(e.snapshot());
  in.nowEpoch = T0 + 5 * 3600 + 1800; in.tempC = 18.0f;
  e2.tick(in);
  TEST_ASSERT_EQUAL_UINT8(0, e2.stepIdx());   // 1h30 dall'arrivo: non ancora
  in.nowEpoch = T0 + 6 * 3600 + 1; in.tempC = 18.0f;
  e2.tick(in);
  TEST_ASSERT_EQUAL_UINT8(1, e2.stepIdx());   // 2 ore dall'arrivo: avanti
}

static void test_profile_attendi_il_target_scade() {
  // Il frigo non ce la fa: dopo 24 ore il tempo parte comunque, e lo si segnala.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = fixedStep(10.0f, 3600);
  s.waitTarget = true;
  e.setSteps(&s, 1);
  e.startProfile(T0, 14.0f, true);
  ProfileEngine::TickInput in{};
  in.tempValid = true; in.tempC = 12.0f;
  in.nowEpoch = T0 + 24 * 3600 - 1;
  TEST_ASSERT_TRUE(e.tick(in).waitingTarget);
  in.nowEpoch = T0 + 24 * 3600;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_FALSE(out.waitingTarget);
  TEST_ASSERT_TRUE(e.snapshot().waitTimedOut);
  TEST_ASSERT_EQUAL_UINT32(T0 + 24 * 3600 + 3600, e.stepEndsAtEpoch());
}

static void test_profile_attendi_il_target_blackout() {
  // Blackout di due giorni durante l'attesa: il mosto non ha mai potuto arrivare, l'attesa
  // riparte da adesso invece di far partire il tempo dello step.
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = fixedStep(10.0f, 3600);
  s.waitTarget = true;
  e.setSteps(&s, 1);
  e.startProfile(T0, 14.0f, true);
  ProfileEngine::TickInput in{};
  in.tempValid = true; in.tempC = 13.0f;
  in.nowEpoch = T0 + 48 * 3600;
  ProfileEngine::TickOutput out = e.tick(in);
  TEST_ASSERT_TRUE(out.waitingTarget);
  TEST_ASSERT_EQUAL_UINT32(T0 + 48 * 3600, e.stepStartEpoch());
  TEST_ASSERT_FALSE(e.snapshot().waitTimedOut);
  // Con un setpoint manuale a 12 °C l'arrivo si misura su quello.
  e.overrideSetpoint(12.0f);
  in.nowEpoch = T0 + 49 * 3600; in.tempC = 12.2f;
  out = e.tick(in);
  TEST_ASSERT_FALSE(out.waitingTarget);
  TEST_ASSERT_FALSE(e.snapshot().waitTimedOut);
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
  // Un tick per ora, come in marcia: un salto di ore in un tick solo sarebbe una ripresa
  // dopo un'interruzione, che avanza di uno step soltanto.
  ProfileEngine::TickOutput out;
  for (int h = 1; h <= 7; h++) {
    in.nowEpoch = T0 + h * 3600 + 10;
    out = e.tick(in);
  }
  TEST_ASSERT_EQUAL_UINT8(7, e.stepIdx());  // dentro l'ottavo step
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

static void test_pipeline_stop_forzato_non_insegna() {
  // Stesso ciclo del test precedente, ma lo stop lo impone un divieto (limite sull'aria):
  // la deriva che segue non e' inerzia dell'impianto e non deve insegnare nulla.
  ControlPipeline p;
  ControlPipeline::Cfg c = pipeCfgHyst();
  c.predictive = false;
  c.compressorRestSec = 0;
  p.configure(c);
  ControlPipeline::Status st = p.tick(T0, 0, true, 20.0f, true, 20.6f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  uint32_t s = 1;
  for (; s <= 300; s++) st = p.tick(T0 + s, s * 1000, true, 20.0f, true, 20.6f, true, 1.0f);
  TEST_ASSERT_TRUE(st.cool);
  for (int i = 0; i < 200; i++, s++) {  // freddo vietato: deriva in discesa per 200 s
    st = p.tick(T0 + s, s * 1000, true, 20.0f, true, 20.6f - (i + 1) * 0.001f, true, 1.0f,
                true, false);
    TEST_ASSERT_FALSE(st.cool);
  }
  p.tick(T0 + s, s * 1000, true, 20.0f, true, 20.55f, true, 1.0f);  // consenso tornato
  TEST_ASSERT_EQUAL_FLOAT(0.0f, p.coolEstimatorCph());
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

static void test_gravity_sorgente_presente_ma_muta_scade_ancora() {
  // Densimetro muto: il timeout deve far avanzare lo step, e' la rete di sicurezza
  // per cui esiste (un densimetro incrostato non tiene in ostaggio il profilo).
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = gravStep(20.0f, ax::GravityMode::Below, 1.010f, 3 * 86400);
  e.setSteps(&s, 1);
  e.startProfile(T0, 20.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 20.0f; in.tempValid = true;
  in.gravityValid = false; in.gravityFresh = false;
  in.nowEpoch = T0 + 3 * 86400 + 1;
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
  ProfileEngine::TickOutput out;
  for (int h = 1; h <= 11; h++) {  // un tick per ora, come in marcia
    in.nowEpoch = T0 + h * 3600 + 10;
    out = e.tick(in);
  }
  TEST_ASSERT_EQUAL_UINT8(11, e.stepIdx());  // dentro il 12° step
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

// ---------- HeaterGuard (rilevatori di guasto del riscaldatore) ----------

// Esegue n tick da 1 s con lo stesso ingresso; ritorna il primo blocco (o None).
static HeatLock hgRun(HeaterGuard& g, uint32_t n, float t, float sp, bool heat, bool cool,
                      float dTperTick = 0.0f, bool active = true, bool valid = true) {
  for (uint32_t i = 0; i < n; i++) {
    HeatLock r = g.tick(1.0f, active, valid, t, true, sp, heat, cool);
    if (r != HeatLock::None) return r;
    t += dTperTick;
  }
  return HeatLock::None;
}

static void test_heater_timeout_scatta_al_limite() {
  HeaterGuard g;
  HeaterGuard::Cfg c; c.maxOnSec = 3600; c.stuckSec = 0;  // solo il tempo massimo
  g.configure(c);
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 3599, 18.0f, 20.0f, true, false));
  TEST_ASSERT_EQUAL(HeatLock::HeatTimeout, hgRun(g, 1, 18.0f, 20.0f, true, false));
}

static void test_heater_timeout_si_azzera_quando_il_caldo_si_spegne() {
  // PID a finestra: il caldo si accende e spegne, non e' mai continuo -> nessun blocco.
  HeaterGuard g;
  HeaterGuard::Cfg c; c.maxOnSec = 600; c.stuckSec = 0;
  g.configure(c);
  for (int k = 0; k < 100; k++) {
    TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 300, 19.5f, 20.0f, true, false));
    TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 30, 19.5f, 20.0f, false, false));
  }
}

static void test_heater_sonda_ferma_a_caldo_pieno() {
  HeaterGuard g;
  HeaterGuard::Cfg c; c.stuckSec = 3600; c.maxOnSec = 0;
  g.configure(c);
  // Sonda congelata a 18,0 °C con il caldo acceso di continuo.
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 3599, 18.0f, 20.0f, true, false));
  TEST_ASSERT_EQUAL(HeatLock::ProbeStuck, hgRun(g, 1, 18.0f, 20.0f, true, false));
}

static void test_heater_sonda_che_sale_non_blocca() {
  HeaterGuard g;
  HeaterGuard::Cfg c; c.stuckSec = 3600; c.maxOnSec = 0;
  g.configure(c);
  // 0,3 °C/h: lento ma reale. Per tre finestre di fila non deve scattare.
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 3 * 3600, 15.0f, 20.0f, true, false, 0.3f / 3600));
}

static void test_heater_sonda_che_si_ferma_dopo_aver_salito() {
  // Sale per un'ora, poi la sonda si congela: deve scattare nella finestra successiva.
  HeaterGuard g;
  HeaterGuard::Cfg c; c.stuckSec = 3600; c.maxOnSec = 0;
  g.configure(c);
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 3600, 15.0f, 20.0f, true, false, 1.0f / 3600));
  TEST_ASSERT_EQUAL(HeatLock::ProbeStuck, hgRun(g, 3600, 16.0f, 20.0f, true, false));
}

static void test_heater_salita_a_caldo_spento() {
  HeaterGuard g;
  g.configure(HeaterGuard::Cfg{});
  // Dentro i 10 minuti di assestamento una salita non conta (calore residuo del tappetino).
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 599, 20.0f, 20.0f, false, false, 1.5f / 600));
  // Dopo l'assestamento: +1,2 °C in 15 minuti a caldo spento -> SSR incollato.
  TEST_ASSERT_EQUAL(HeatLock::RiseHeatOff, hgRun(g, 900, 21.5f, 20.0f, false, false, 1.2f / 900));
}

static void test_heater_lenta_deriva_a_caldo_spento_non_blocca() {
  // Fermentazione esotermica: +0,5 °C/h senza freddo -> nessun blocco.
  HeaterGuard g;
  g.configure(HeaterGuard::Cfg{});
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 6 * 3600, 18.0f, 18.0f, false, false, 0.5f / 3600));
}

static void test_heater_freddo_inefficace() {
  HeaterGuard g;
  g.configure(HeaterGuard::Cfg{});
  // Freddo acceso, 3 °C sopra il setpoint, temperatura ferma per 2 ore.
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 7199, 21.0f, 18.0f, false, true));
  TEST_ASSERT_EQUAL(HeatLock::CoolIneffective, hgRun(g, 1, 21.0f, 18.0f, false, true));
}

static void test_heater_freddo_che_raffredda_non_blocca() {
  // Pull-down reale: -1 °C/h col freddo acceso per 6 ore.
  HeaterGuard g;
  g.configure(HeaterGuard::Cfg{});
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 6 * 3600, 24.0f, 10.0f, false, true, -1.0f / 3600));
}

static void test_heater_freddo_vicino_al_setpoint_non_blocca() {
  // Cold crash al limite del frigo: 1,8 °C sopra il setpoint a -1, fermo. Non e' un guasto
  // del riscaldatore: sotto la soglia coolAboveSpC il rilevatore non guarda.
  HeaterGuard g;
  g.configure(HeaterGuard::Cfg{});
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 6 * 3600, 0.8f, -1.0f, false, true));
}

static void test_heater_salto_della_sonda_non_blocca() {
  // Sonda spostata da un frigo a 2 °C al pozzetto di un mosto a 20 °C, a caldo spento:
  // e' un salto istantaneo, non la rampa di un riscaldatore incollato.
  HeaterGuard g;
  g.configure(HeaterGuard::Cfg{});
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 1200, 2.0f, 18.0f, false, false));
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 3600, 20.0f, 18.0f, false, false));
  // Dopo il salto il rilevatore resta attivo: una rampa vera scatta ancora.
  TEST_ASSERT_EQUAL(HeatLock::RiseHeatOff, hgRun(g, 900, 20.0f, 18.0f, false, false, 1.2f / 900));
}

static void test_heater_disarmato_azzera_e_sonda_invalida_congela() {
  HeaterGuard g;
  HeaterGuard::Cfg c; c.maxOnSec = 100; c.stuckSec = 0;
  g.configure(c);
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 90, 18.0f, 20.0f, true, false));
  // Sonda invalida per 50 s: si congela, non si accumula.
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 50, 18.0f, 20.0f, true, false, 0, true, false));
  TEST_ASSERT_EQUAL(HeatLock::HeatTimeout, hgRun(g, 10, 18.0f, 20.0f, true, false));
  // Disarmo: tutto azzerato, si riparte da zero.
  g.reset();
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 90, 18.0f, 20.0f, true, false));
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 1, 18.0f, 20.0f, true, false, 0, false));
  TEST_ASSERT_EQUAL(HeatLock::None, hgRun(g, 99, 18.0f, 20.0f, true, false));
}

// Scratchpad DS18B20 integro con la temperatura grezza `raw` e il registro `cfg`.
static void spFill(uint8_t sp[9], int16_t raw, uint8_t cfg = 0x7F) {
  sp[0] = (uint8_t)(raw & 0xFF); sp[1] = (uint8_t)((uint16_t)raw >> 8);
  sp[2] = 0x4B; sp[3] = 0x46; sp[4] = cfg; sp[5] = 0xFF; sp[6] = 0x0C; sp[7] = 0x10;
  sp[8] = dallasCrc8(sp, 8);
}

static void test_sonda_classificazione_guasti() {
  // CRC-8 Dallas: esempio della nota applicativa Maxim 27 (famiglia 02, CRC A2).
  const uint8_t rom[7] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_HEX8(0xA2, dallasCrc8(rom, 7));
  uint8_t sp[9];
  float t = -99.0f;
  spFill(sp, 344);  // 21,5 °C
  TEST_ASSERT_EQUAL(ProbeFault::None, classifyScratchpad(true, sp, 0x28, -20.0f, 60.0f, &t));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.5f, t);
  spFill(sp, -162);  // -10,125 °C
  TEST_ASSERT_EQUAL(ProbeFault::None, classifyScratchpad(true, sp, 0x28, -20.0f, 60.0f, &t));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -10.125f, t);
  spFill(sp, 0x0157, 0x1F);  // 9 bit: i tre bit bassi non contano -> 21,0
  TEST_ASSERT_EQUAL(ProbeFault::None, classifyScratchpad(true, sp, 0x28, -20.0f, 60.0f, &t));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, t);
  spFill(sp, 0x0550);  // 85 °C di accensione
  TEST_ASSERT_EQUAL(ProbeFault::Reset, classifyScratchpad(true, sp, 0x28, -20.0f, 60.0f, &t));
  spFill(sp, -50 * 16);
  TEST_ASSERT_EQUAL(ProbeFault::OutOfRange, classifyScratchpad(true, sp, 0x28, -20.0f, 60.0f, &t));
  spFill(sp, 344);
  sp[3] ^= 0x10;  // un bit cambiato per strada
  TEST_ASSERT_EQUAL(ProbeFault::Crc, classifyScratchpad(true, sp, 0x28, -20.0f, 60.0f, &t));
  for (uint8_t i = 0; i < 9; i++) sp[i] = 0xFF;
  TEST_ASSERT_EQUAL(ProbeFault::AllOnes, classifyScratchpad(true, sp, 0x28, -20.0f, 60.0f, &t));
  for (uint8_t i = 0; i < 9; i++) sp[i] = 0x00;
  TEST_ASSERT_EQUAL(ProbeFault::AllZeros, classifyScratchpad(true, sp, 0x28, -20.0f, 60.0f, &t));
  TEST_ASSERT_EQUAL(ProbeFault::NoResponse, classifyScratchpad(false, sp, 0x28, -20.0f, 60.0f, &t));
  // Altra famiglia: solo il controllo d'integrita', la temperatura non viene toccata.
  spFill(sp, 344);
  t = -99.0f;
  TEST_ASSERT_EQUAL(ProbeFault::None, classifyScratchpad(true, sp, 0x10, -20.0f, 60.0f, &t));
  TEST_ASSERT_EQUAL_FLOAT(-99.0f, t);
}

// Cold crash: il mosto scende di dCph °C/h per `ore` ore, un tick al minuto.
static float crashRun(CrashMonitor& m, float t, float dCph, float ore) {
  for (int i = 0; i < (int)(ore * 60); i++) {
    m.tick(60.0f, true, t);
    t -= dCph / 60.0f;
  }
  return t;
}

static void test_cold_crash_stima_e_allarme() {
  CrashMonitor m;
  bool misurata = false;
  // Appena partito: nessun dato, stima sulla velocita' programmata (0,5 °C/h).
  TEST_ASSERT_EQUAL_INT32(36 * 60, m.etaMin(20.0f, 2.0f, 0.5f, 0.5f, &misurata));
  TEST_ASSERT_FALSE(misurata);
  // Alla velocita' dolce (0,25 °C/h) la stima programmata raddoppia.
  TEST_ASSERT_EQUAL_INT32(72 * 60, m.etaMin(20.0f, 2.0f, 0.5f, 0.25f, &misurata));
  // Frigo che scende a 0,4 °C/h (piu' lento del programma ma sopra la soglia).
  float t = crashRun(m, 20.0f, 0.4f, 7.0f);
  TEST_ASSERT_TRUE(m.full());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 2.4f, m.drop());  // 6 ore x 0,4
  int32_t eta = m.etaMin(t, 2.0f, 0.5f, 0.5f, &misurata);
  TEST_ASSERT_TRUE(misurata);
  TEST_ASSERT_INT32_WITHIN(15, (int32_t)((t - 2.0f) / 0.4f * 60.0f), eta);
  TEST_ASSERT_FALSE(m.slow(t, 2.0f, 0.5f, 1.5f));
  // Il frigo rallenta a 0,1 °C/h: dopo sei ore a quel passo scatta l'allarme.
  t = crashRun(m, t, 0.1f, 6.0f);
  TEST_ASSERT_TRUE(m.slow(t, 2.0f, 0.5f, 1.5f));
  // Arrivato in tolleranza: niente allarme e niente stima.
  TEST_ASSERT_FALSE(m.slow(2.3f, 2.0f, 0.5f, 1.5f));
  TEST_ASSERT_EQUAL_INT32(-1, m.etaMin(2.3f, 2.0f, 0.5f, 0.5f, &misurata));
  // Sonda assente oltre 30 minuti: la finestra riparte.
  for (int i = 0; i < 31; i++) m.tick(60.0f, false, 0.0f);
  TEST_ASSERT_FALSE(m.full());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, m.windowHours());
}

static void test_taratura_segue_la_sonda() {
  auto same = [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) == 0; };
  auto empty = [](const std::string& a) { return a.empty(); };
  const std::string vecchi[3] = {"28AA", "28BB", "28CC"};
  const float tar[3] = {0.5f, -0.25f, 1.0f};
  bool m[3];
  {  // Sonda del mosto sostituita: la nuova riparte da zero.
    std::string nuovi[3] = {"28DD", "28BB", "28CC"};
    float off[3] = {0.5f, -0.25f, 1.0f};
    TEST_ASSERT_EQUAL_UINT8(1, followProbeOffsets(vecchi, tar, nuovi, off, 3, same, empty, m));
    TEST_ASSERT_TRUE(m[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, off[0]);
    TEST_ASSERT_EQUAL_FLOAT(-0.25f, off[1]);
  }
  {  // Due sonde scambiate di posizione: ognuna si porta dietro la sua taratura.
    std::string nuovi[3] = {"28bb", "28AA", "28CC"};  // maiuscole diverse: stessa sonda
    float off[3] = {0.5f, -0.25f, 1.0f};
    TEST_ASSERT_EQUAL_UINT8(2, followProbeOffsets(vecchi, tar, nuovi, off, 3, same, empty, m));
    TEST_ASSERT_EQUAL_FLOAT(-0.25f, off[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, off[1]);
  }
  {  // Sonda cambiata e correzione scritta dall'utente nello stesso salvataggio: si rispetta.
    std::string nuovi[3] = {"28DD", "28BB", "28CC"};
    float off[3] = {0.3f, -0.25f, 1.0f};
    TEST_ASSERT_EQUAL_UINT8(0, followProbeOffsets(vecchi, tar, nuovi, off, 3, same, empty, m));
    TEST_ASSERT_EQUAL_FLOAT(0.3f, off[0]);
  }
  {  // Nessun cambio di sonda: niente si muove. Posizione svuotata: correzione a zero.
    std::string nuovi[3] = {"28AA", "28BB", ""};
    float off[3] = {0.5f, -0.25f, 1.0f};
    TEST_ASSERT_EQUAL_UINT8(1, followProbeOffsets(vecchi, tar, nuovi, off, 3, same, empty, m));
    TEST_ASSERT_FALSE(m[0]);
    TEST_ASSERT_TRUE(m[2]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, off[2]);
  }
}

static void test_penalita_tentativi() {
  RetryPenalty p;
  const uint32_t t0 = 0xFFFFF000u;  // a cavallo del wrap di millis()
  for (int i = 0; i < 4; i++) p.fail(t0 + i);
  TEST_ASSERT_FALSE(p.locked(t0 + 10));
  p.fail(t0 + 10);  // quinto errore: pausa di 2 s
  TEST_ASSERT_TRUE(p.locked(t0 + 11));
  TEST_ASSERT_EQUAL_UINT32(2, p.waitSec(t0 + 11));
  TEST_ASSERT_TRUE(p.locked(t0 + 2009));
  TEST_ASSERT_FALSE(p.locked(t0 + 2010));
  p.fail(t0 + 2010);  // sesto: 4 s
  TEST_ASSERT_TRUE(p.locked(t0 + 6009));
  TEST_ASSERT_FALSE(p.locked(t0 + 6010));
  for (int i = 0; i < 10; i++) p.fail(t0 + 7000 + i);  // tetto a 60 s
  TEST_ASSERT_EQUAL_UINT32(60, p.waitSec(t0 + 7009));
  // Un tentativo riuscito azzera.
  p.success();
  TEST_ASSERT_FALSE(p.locked(t0 + 7010));
  // Dopo 15 minuti senza errori il conteggio riparte da zero.
  for (int i = 0; i < 4; i++) p.fail(t0 + 8000 + i);
  p.fail(t0 + 8000 + 15UL * 60UL * 1000UL + 10);
  TEST_ASSERT_FALSE(p.locked(t0 + 8000 + 15UL * 60UL * 1000UL + 11));
}

static void test_penalita_per_indirizzo() {
  RetryPenaltyByKey<2> p;
  const uint32_t A = 0x0A00A8C0, B = 0x0B00A8C0, C = 0x0C00A8C0;
  for (int i = 0; i < 5; i++) p.fail(A, 1000 + i);  // uno script con la password vecchia
  TEST_ASSERT_TRUE(p.locked(A, 1010));
  TEST_ASSERT_FALSE(p.locked(B, 1010));  // il proprietario, da un altro indirizzo, entra
  // Posti pieni: un terzo indirizzo prende il posto inattivo o il piu' vecchio, non blocca A.
  p.fail(B, 2000);
  p.fail(C, 3000);
  TEST_ASSERT_FALSE(p.locked(C, 3001));
  p.success(A);
  TEST_ASSERT_FALSE(p.locked(A, 3002));
}

// Immagine sintetica nel formato di elf2bin.py: 0xE9 in testa e a 0x1000, lunghezza e CRC
// a 0x1010. Il CRC atteso (0x0C5751C3) e' calcolato con crc8266() di elf2bin.py.
static std::vector<uint8_t> fwSintetico() {
  std::vector<uint8_t> b(5000);
  for (size_t i = 0; i < b.size(); i++) b[i] = (uint8_t)((i * 7 + 3) & 255);
  b[0] = 0xE9;
  b[0x1000] = 0xE9;
  const uint32_t len = 5000, crc = 0x0C5751C3u;
  for (int k = 0; k < 4; k++) {
    b[0x1010 + k] = (uint8_t)(len >> (8 * k));
    b[0x1014 + k] = (uint8_t)(crc >> (8 * k));
  }
  return b;
}

static FwImageCheck::Result fwVerifica(const std::vector<uint8_t>& b, size_t n, size_t blocco) {
  FwImageCheck c;
  c.begin();
  for (size_t o = 0; o < n; o += blocco) c.feed(b.data() + o, (n - o) < blocco ? (n - o) : blocco);
  return c.result();
}

static void test_firmware_immagine_verificata() {
  std::vector<uint8_t> b = fwSintetico();
  TEST_ASSERT_EQUAL(FwImageCheck::Result::Ok, fwVerifica(b, b.size(), 1436));
  TEST_ASSERT_EQUAL(FwImageCheck::Result::Ok, fwVerifica(b, b.size(), 7));  // blocchi qualsiasi
  TEST_ASSERT_EQUAL(FwImageCheck::Result::Short, fwVerifica(b, 4999, 1436));  // troncato
  TEST_ASSERT_EQUAL(FwImageCheck::Result::NotImage, fwVerifica(b, 3000, 1436));
  std::vector<uint8_t> lungo = b;
  lungo.push_back(0);
  TEST_ASSERT_EQUAL(FwImageCheck::Result::Long, fwVerifica(lungo, lungo.size(), 1436));
  std::vector<uint8_t> rotto = b;
  rotto[2500] ^= 0x01;  // un bit cambiato
  TEST_ASSERT_EQUAL(FwImageCheck::Result::Crc, fwVerifica(rotto, rotto.size(), 1436));
  std::vector<uint8_t> altro = b;
  altro[0x1000] = 0x1F;  // per esempio un .bin.gz
  TEST_ASSERT_EQUAL(FwImageCheck::Result::NotImage, fwVerifica(altro, altro.size(), 1436));
}

static void test_firmware_vero_verificato() {
  // Il firmware.bin compilato per la scheda, se c'e': deve passare intero, non troncato.
  // Percorso dal sorgente di questo test (la cartella di lavoro dei test cambia).
  std::string dir = __FILE__;
  dir = dir.substr(0, dir.find_last_of('/') + 1);
  FILE* f = fopen((dir + "../../.pio/build/d1_mini/firmware.bin").c_str(), "rb");
  if (!f) f = fopen(".pio/build/d1_mini/firmware.bin", "rb");
  if (!f) TEST_IGNORE_MESSAGE("firmware.bin non compilato: prova saltata");
  std::vector<uint8_t> b;
  uint8_t buf[4096];
  size_t k;
  while ((k = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + k);
  fclose(f);
  TEST_ASSERT_EQUAL(FwImageCheck::Result::Ok, fwVerifica(b, b.size(), 1436));
  TEST_ASSERT_EQUAL(FwImageCheck::Result::Short, fwVerifica(b, 300000, 1436));
}

// Freddo non comandato (relè del frigo incollato): solo allarme.
// sp < -50 = nessun setpoint (modo fermo).
static bool cwRun(CoolWatch& w, uint32_t n, bool cool, float sp, float mosto, float aria,
                  float dAria, float amb) {
  bool a = false;
  for (uint32_t i = 0; i < n; i++) {
    a = w.tick(1.0f, true, cool, sp > -50.0f, sp, true, mosto, true, aria, true, amb);
    aria += dAria;
  }
  return a;
}

static void test_coolwatch_rele_incollato_dopo_lo_stop() {
  // Il frigo gira (aria a 4 °C per un mosto a 18), poi il freddo viene spento ma il relè
  // resta chiuso: dopo 10 minuti l'aria e' ancora a 4.
  CoolWatch w;
  w.configure(CoolWatch::Cfg{});
  TEST_ASSERT_FALSE(cwRun(w, 300, true, 18.0f, 18.0f, 4.0f, 0, 22.0f));
  TEST_ASSERT_FALSE(cwRun(w, 599, false, 18.0f, 18.0f, 4.0f, 0, 22.0f));
  TEST_ASSERT_TRUE(cwRun(w, 2, false, 18.0f, 18.0f, 4.0f, 0, 22.0f));
  // Resta finche' l'aria resta fredda; si spegne quando il freddo torna comandato.
  TEST_ASSERT_TRUE(cwRun(w, 3600, false, 18.0f, 18.0f, 4.0f, 0, 22.0f));
  TEST_ASSERT_FALSE(cwRun(w, 1, true, 18.0f, 18.0f, 4.0f, 0, 22.0f));
}

static void test_coolwatch_stop_vero_non_allarma() {
  // Stop vero: l'aria risale di 6 °C in 10 minuti.
  CoolWatch w;
  w.configure(CoolWatch::Cfg{});
  TEST_ASSERT_FALSE(cwRun(w, 300, true, 18.0f, 18.0f, 4.0f, 0, 22.0f));
  TEST_ASSERT_FALSE(cwRun(w, 1800, false, 18.0f, 18.0f, 4.0f, 6.0f / 600, 22.0f));
}

static void test_coolwatch_falso_allarme_si_smentisce() {
  // Freezer a pozzetto: dopo lo stop l'aria risale piano (solo +0,5 in 10 minuti), l'allarme
  // scatta, ma si spegne da solo quando l'aria risale vicino al mosto.
  CoolWatch w;
  w.configure(CoolWatch::Cfg{});
  TEST_ASSERT_FALSE(cwRun(w, 300, true, 18.0f, 18.0f, 4.0f, 0, 22.0f));
  TEST_ASSERT_TRUE(cwRun(w, 601, false, 18.0f, 18.0f, 4.0f, 0.5f / 600, 22.0f));
  // ref 18, soglia 16: si spegne con l'aria a 17.
  TEST_ASSERT_FALSE(cwRun(w, 1300, false, 18.0f, 18.0f, 4.5f, 12.6f / 1300, 22.0f));
}

static void test_coolwatch_garage_freddo_non_allarma() {
  // Inverno: il frigo non ha mai raffreddato, l'aria e' stabile a 10 °C con il mosto a 18.
  CoolWatch w;
  w.configure(CoolWatch::Cfg{});
  TEST_ASSERT_FALSE(cwRun(w, 3600, false, 18.0f, 18.0f, 10.0f, 0, 8.0f));
  // Un breve ciclo di freddo (correzione di un superamento), poi stop: l'aria resta a 8,5
  // perche' la stanza e' a 5. Non e' sotto la stanza: niente allarme.
  TEST_ASSERT_FALSE(cwRun(w, 300, true, 18.0f, 18.5f, 8.0f, 0, 5.0f));
  TEST_ASSERT_FALSE(cwRun(w, 1800, false, 18.0f, 18.5f, 8.5f, 0, 5.0f));
}

static void test_coolwatch_crollo_a_frigo_fermo() {
  // Frigo fermo da un pezzo, il relè si chiude da solo: l'aria crolla di 4 °C in 4 minuti
  // sotto l'ambiente e sotto il mosto. Il crollo parte a meta' di un passo della finestra.
  CoolWatch w;
  w.configure(CoolWatch::Cfg{});
  TEST_ASSERT_FALSE(cwRun(w, 1365, false, 18.0f, 18.0f, 17.0f, 0, 22.0f));
  TEST_ASSERT_TRUE(cwRun(w, 240, false, 18.0f, 18.0f, 17.0f, -4.0f / 240, 22.0f));
}

static void test_coolwatch_porta_aperta_non_allarma() {
  // Estate: camera a 12, stanza a 25: porta aperta (aria a 18) e richiusa (torna a 12,5).
  CoolWatch w;
  w.configure(CoolWatch::Cfg{});
  TEST_ASSERT_FALSE(cwRun(w, 1200, false, 12.0f, 12.0f, 12.5f, 0, 25.0f));
  TEST_ASSERT_FALSE(cwRun(w, 60, false, 12.0f, 12.0f, 12.5f, 5.5f / 60, 25.0f));
  TEST_ASSERT_FALSE(cwRun(w, 240, false, 12.0f, 12.0f, 18.0f, -5.5f / 240, 25.0f));
  // Riscaldamento: setpoint 20, mosto a 17, stanza a 23. Porta aperta (aria a 21) e richiusa
  // (torna a 17,8): sotto il setpoint di oltre 2 °C, ma non sotto il mosto.
  TEST_ASSERT_FALSE(cwRun(w, 1200, false, 20.0f, 17.0f, 17.5f, 0, 23.0f));
  TEST_ASSERT_FALSE(cwRun(w, 60, false, 20.0f, 17.0f, 17.5f, 3.5f / 60, 23.0f));
  TEST_ASSERT_FALSE(cwRun(w, 120, false, 20.0f, 17.0f, 21.0f, -3.2f / 120, 23.0f));
}

static void test_coolwatch_modo_fermo_rele_incollato() {
  // Fine cotta: il freddo girava, poi la camera passa a fermo (nessun setpoint) e il relè
  // resta chiuso. Riferimento = mosto (4 °C): l'aria a -2 non risale.
  CoolWatch w;
  w.configure(CoolWatch::Cfg{});
  TEST_ASSERT_FALSE(cwRun(w, 300, true, 4.0f, 4.0f, -2.0f, 0, 20.0f));
  TEST_ASSERT_TRUE(cwRun(w, 601, false, -100.0f, 4.0f, -2.0f, 0, 20.0f));
}

static void test_slope_stima_pendenza_e_arrivo() {
  SlopeEstimator s;
  float t = 10.0f;
  for (int k = 0; k < 21 * 60; k++) {  // 21 minuti a +1,5 °C/h
    s.tick(1.0f, true, t);
    t += 1.5f / 3600.0f;
  }
  TEST_ASSERT_TRUE(s.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.5f, s.cph());
  // Da circa 10,5 °C a 18 °C a 1,5 °C/h: circa 5 ore.
  const int32_t eta = s.etaMin(10.5f, 18.0f, 0.3f);
  TEST_ASSERT_INT_WITHIN(5, 300, eta);
  // Allontanandosi dal target: nessuna stima.
  TEST_ASSERT_EQUAL_INT32(-1, s.etaMin(10.5f, 5.0f, 0.3f));
}

static void test_slope_quantizzazione_e_buco_della_sonda() {
  SlopeEstimator s;
  // Temperatura ferma ma quantizzata a 0,0625: la pendenza resta vicina a zero.
  for (int k = 0; k < 21 * 60; k++) s.tick(1.0f, true, (k / 60) % 2 ? 18.0625f : 18.0f);
  TEST_ASSERT_TRUE(s.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.25f, 0.0f, s.cph());
  // Sonda invalida oltre 90 secondi: si riparte da zero.
  for (int k = 0; k < 91; k++) s.tick(1.0f, false, 0.0f);
  TEST_ASSERT_FALSE(s.valid());
}

static void test_aria_limite_freddo_e_caldo() {
  AirLimit a;
  a.configure(AirLimit::Cfg{});  // min -5, distanza sotto spenta, 10 sopra, isteresi 3
  // Ale a 18 °C: senza la distanza sotto, il pull-down non e' limitato (aria a 2 °C).
  a.tick(true, 18.0f, true, 2.0f);
  TEST_ASSERT_FALSE(a.coolBlocked());
  // Cold crash a -1: freddo sospeso sotto -5, ripreso sopra -2.
  a.tick(true, -1.0f, true, -5.2f);
  TEST_ASSERT_TRUE(a.coolBlocked());
  a.tick(true, -1.0f, true, -2.5f);
  TEST_ASSERT_TRUE(a.coolBlocked());
  a.tick(true, -1.0f, true, -1.9f);
  TEST_ASSERT_FALSE(a.coolBlocked());
  // Con la distanza sotto a 10: ale a 18, freddo sospeso sotto 8, ripreso sopra 11.
  AirLimit::Cfg c; c.belowSpC = 10.0f;
  a.configure(c);
  a.tick(true, 18.0f, true, 7.9f);
  TEST_ASSERT_TRUE(a.coolBlocked());
  a.tick(true, 18.0f, true, 10.5f);
  TEST_ASSERT_TRUE(a.coolBlocked());
  a.tick(true, 18.0f, true, 11.1f);
  TEST_ASSERT_FALSE(a.coolBlocked());
  // Minimo a -10: nessun minimo assoluto.
  c.belowSpC = 0.0f; c.minAirC = -10.0f;
  a.configure(c);
  a.tick(true, -1.0f, true, -15.0f);
  TEST_ASSERT_FALSE(a.coolBlocked());
  // Caldo sospeso con l'aria oltre 28 °C (setpoint 18 + 10).
  a.tick(true, 18.0f, true, 28.5f);
  TEST_ASSERT_TRUE(a.heatBlocked());
  // Sonda della camera non valida: nessun limite.
  a.tick(true, 18.0f, false, 0.0f);
  TEST_ASSERT_FALSE(a.heatBlocked());
  TEST_ASSERT_FALSE(a.coolBlocked());
}


// ---- 3.14.0: decimazione del grafico che conserva i picchi ----
static void test_minmax_conserva_picchi_e_ordine() {
  MinMaxGroup<2> g;
  g.reset();
  const int32_t M = MinMaxGroup<2>::kMissing;
  // serie 0: 500, 900, 100, 400 -> prima il massimo (900), poi il minimo (100)
  // serie 1: 10, M, 30, 20     -> prima il minimo (10), poi il massimo (30)
  int32_t a[2] = {500, 10}, b[2] = {900, M}, c[2] = {100, 30}, d[2] = {400, 20};
  g.add(1000, a, 0x00);
  g.add(1300, b, 0x02);  // freddo acceso per un solo campione
  g.add(1600, c, 0x00);
  g.add(1900, d, 0x00);
  TEST_ASSERT_EQUAL_UINT16(4, g.count());
  TEST_ASSERT_EQUAL_UINT32(1000, g.firstEpoch());
  TEST_ASSERT_EQUAL_UINT32(1900, g.lastEpoch());
  TEST_ASSERT_EQUAL_INT32(900, g.value(0, false));
  TEST_ASSERT_EQUAL_INT32(100, g.value(0, true));
  TEST_ASSERT_EQUAL_INT32(10, g.value(1, false));
  TEST_ASSERT_EQUAL_INT32(30, g.value(1, true));
  TEST_ASSERT_EQUAL_UINT8(0x02, g.flags());  // l'accensione breve non si perde
}

static void test_minmax_serie_assente_e_reset() {
  MinMaxGroup<2> g;
  g.reset();
  const int32_t M = MinMaxGroup<2>::kMissing;
  int32_t a[2] = {M, 7};
  g.add(10, a, 0x01);
  TEST_ASSERT_EQUAL_INT32(M, g.value(0, false));
  TEST_ASSERT_EQUAL_INT32(M, g.value(0, true));
  TEST_ASSERT_EQUAL_INT32(7, g.value(1, false));
  TEST_ASSERT_EQUAL_INT32(7, g.value(1, true));
  g.reset();
  TEST_ASSERT_EQUAL_UINT16(0, g.count());
  TEST_ASSERT_EQUAL_UINT8(0, g.flags());
  TEST_ASSERT_EQUAL_INT32(M, g.value(1, false));
}


// ---- 3.15.0: registro eventi ad anello ----
struct FakeEeprom {
  uint8_t m[4096];
  uint32_t writes = 0;
  bool fail = false;
  bool read(uint16_t mem, uint8_t* buf, uint16_t len) {
    if (fail || mem + len > sizeof(m)) return false;
    memcpy(buf, m + mem, len);
    return true;
  }
  bool writePage(uint16_t mem, const uint8_t* buf, uint8_t len) {
    if (fail || mem + len > sizeof(m) || (mem / 32) != ((mem + len - 1) / 32)) return false;
    memcpy(m + mem, buf, len);
    writes++;
    return true;
  }
};

static void test_eventi_vergine_e_ripresa() {
  static FakeEeprom ee;
  memset(ee.m, 0xFF, sizeof(ee.m));  // EEPROM nuova
  EventRing<FakeEeprom> r;
  TEST_ASSERT_TRUE(r.begin(&ee));
  TEST_ASSERT_EQUAL_UINT32(0, r.lastSeq());
  TEST_ASSERT_TRUE(r.add(1000, 1, 2, 3, -4));
  TEST_ASSERT_TRUE(r.add(1001, 5, 6, 7, 8));
  EventRec e;
  TEST_ASSERT_TRUE(r.readSeq(2, e));
  TEST_ASSERT_EQUAL_UINT8(5, e.code);
  TEST_ASSERT_EQUAL_INT16(8, e.v);
  // "riavvio": un anello nuovo sulla stessa memoria ritrova il punto
  EventRing<FakeEeprom> r2;
  TEST_ASSERT_TRUE(r2.begin(&ee));
  TEST_ASSERT_EQUAL_UINT32(2, r2.lastSeq());
  TEST_ASSERT_TRUE(r2.add(1002, 9, 0, 0, 0));
  TEST_ASSERT_TRUE(r2.readSeq(1, e));
  TEST_ASSERT_EQUAL_UINT32(1000, e.epoch);
  TEST_ASSERT_TRUE(r2.readSeq(3, e));
  TEST_ASSERT_EQUAL_UINT8(9, e.code);
  TEST_ASSERT_FALSE(r2.readSeq(4, e));
  TEST_ASSERT_FALSE(r2.readSeq(0, e));
}

static void test_eventi_giro_completo_e_contenuto_estraneo() {
  static FakeEeprom ee;
  for (uint16_t i = 0; i < sizeof(ee.m); i++) ee.m[i] = (uint8_t)(i * 37 + 11);  // spazzatura
  EventRing<FakeEeprom> r;
  TEST_ASSERT_TRUE(r.begin(&ee));
  TEST_ASSERT_EQUAL_UINT32(0, r.lastSeq());  // nessun record valido nella spazzatura
  for (uint32_t i = 1; i <= 300; i++) TEST_ASSERT_TRUE(r.add(i, (uint8_t)(i & 0xFF), 0, 0, 0));
  EventRec e;
  TEST_ASSERT_TRUE(r.readSeq(300, e));
  TEST_ASSERT_EQUAL_UINT32(300, e.epoch);
  TEST_ASSERT_TRUE(r.readSeq(45, e));   // l'ultimo dei 256 conservati
  TEST_ASSERT_FALSE(r.readSeq(44, e));  // sovrascritto
  EventRing<FakeEeprom> r2;
  TEST_ASSERT_TRUE(r2.begin(&ee));
  TEST_ASSERT_EQUAL_UINT32(300, r2.lastSeq());
  TEST_ASSERT_TRUE(r2.add(301, 1, 0, 0, 0));
  TEST_ASSERT_TRUE(r2.readSeq(46, e));
  TEST_ASSERT_FALSE(r2.readSeq(45, e));  // il 301 ha preso il suo slot
  // memoria che non risponde: registro spento, nessuna scrittura
  ee.fail = true;
  EventRing<FakeEeprom> r3;
  TEST_ASSERT_FALSE(r3.begin(&ee));
  TEST_ASSERT_FALSE(r3.add(1, 1, 0, 0, 0));
}

static void test_allarmi_antirimbalzo() {
  AlarmDebounce d(30000);
  uint32_t on, off;
  TEST_ASSERT_FALSE(d.tick(0x1, 0, on, off));
  TEST_ASSERT_FALSE(d.tick(0x1, 29000, on, off));
  TEST_ASSERT_TRUE(d.tick(0x1, 30000, on, off));
  TEST_ASSERT_EQUAL_UINT32(0x1, on);
  TEST_ASSERT_EQUAL_UINT32(0, off);
  // oscillazione rapida: niente
  TEST_ASSERT_FALSE(d.tick(0x0, 31000, on, off));
  TEST_ASSERT_FALSE(d.tick(0x1, 32000, on, off));
  TEST_ASSERT_FALSE(d.tick(0x0, 33000, on, off));
  TEST_ASSERT_FALSE(d.tick(0x1, 70000, on, off));  // tornato uguale: azzera
  // spento stabile, piu' un altro acceso
  TEST_ASSERT_FALSE(d.tick(0x4, 80000, on, off));
  TEST_ASSERT_TRUE(d.tick(0x4, 110000, on, off));
  TEST_ASSERT_EQUAL_UINT32(0x4, on);
  TEST_ASSERT_EQUAL_UINT32(0x1, off);
}


// ---- 3.16.0: andamento della densita' ----
static float rumore(uint32_t i) {  // ±0,001 SG, deterministico
  return ((int)((uint32_t)(i * 2654435761u) >> 28) - 8) / 8000.0f;
}
static uint32_t trendFill(GravityTrend& t, uint32_t t0, float sg0, float sgDay, float ore,
                          uint32_t passoSec = 1800) {
  uint32_t e = t0, i = 0;
  for (; e < t0 + (uint32_t)(ore * 3600); e += passoSec, i++)
    t.add(e, sg0 + sgDay * (e - t0) / 86400.0f + rumore(i));
  return e - passoSec;
}

static void test_trend_attiva_stabile_rallenta() {
  const uint32_t T0 = 1800000000UL;
  GravityTrend t;
  t.reset();
  uint32_t fine = trendFill(t, T0, 1.050f, -0.010f, 50);
  float s;
  TEST_ASSERT_TRUE(t.slopePerDay(fine, 24, s));
  TEST_ASSERT_FLOAT_WITHIN(0.0015f, -0.010f, s);
  TEST_ASSERT_EQUAL(GravityTrend::State::Active, t.state(fine));

  t.reset();
  fine = trendFill(t, T0, 1.012f, 0.0f, 50);  // ferma, con il rumore dell'iSpindel
  TEST_ASSERT_EQUAL(GravityTrend::State::Stable, t.state(fine));

  t.reset();
  fine = trendFill(t, T0, 1.014f, -0.002f, 50);  // cala piano: non e' ferma
  TEST_ASSERT_EQUAL(GravityTrend::State::Slowing, t.state(fine));
}

static void test_trend_dati_insufficienti_o_vecchi() {
  const uint32_t T0 = 1800000000UL;
  GravityTrend t;
  t.reset();
  uint32_t fine = trendFill(t, T0, 1.012f, 0.0f, 20);  // solo 20 ore: niente «stabile»
  TEST_ASSERT_NOT_EQUAL(GravityTrend::State::Stable, t.state(fine));
  t.reset();
  fine = trendFill(t, T0, 1.012f, 0.0f, 50);
  TEST_ASSERT_EQUAL(GravityTrend::State::Unknown, t.state(fine + 4 * 3600));  // densimetro muto
  // lettura nel passato e valori assurdi ignorati
  const uint8_t prima = t.bins();
  t.add(fine - 10 * 3600, 1.000f);
  t.add(fine + 3600, 2.5f);
  TEST_ASSERT_EQUAL_UINT8(prima, t.bins());
}

static void test_conversione_plato_sg() {
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.0484f, platoToSg(12.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 12.0f, sgToPlato(platoToSg(12.0f)));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, sgToPlato(1.000f));
}


// ---- 3.16.0: step «attendi conferma» e stabilita' robusta al rumore ----
static void test_step_attende_la_conferma() {
  StepDef s[2];
  s[0].type = StepType::Fixed; s[0].enabled = true; s[0].targetC = 19.0f;
  s[0].durationSec = 3600; s[0].confirm = true;
  s[1].type = StepType::Fixed; s[1].enabled = true; s[1].targetC = 21.0f; s[1].durationSec = 3600;
  ProfileEngine e;
  e.configure(0.5f);
  e.setSteps(s, 2);
  e.startProfile(T0, 19.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 19.0f; in.tempValid = true;
  in.nowEpoch = T0 + 3601;
  ProfileEngine::TickOutput o = e.tick(in);
  TEST_ASSERT_TRUE(o.waitingConfirm);
  TEST_ASSERT_TRUE(o.stepChanged);  // l'attesa va salvata
  TEST_ASSERT_EQUAL_UINT8(0, e.stepIdx());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 19.0f, o.setpointC);
  in.nowEpoch = T0 + 3 * 86400;  // giorni dopo: aspetta ancora, non avanza da solo
  o = e.tick(in);
  TEST_ASSERT_TRUE(o.waitingConfirm);
  TEST_ASSERT_EQUAL_UINT8(0, e.stepIdx());
  // riavvio: l'attesa sopravvive
  EngineSnapshot snap = e.snapshot();
  ProfileEngine e2;
  e2.configure(0.5f);
  e2.setSteps(s, 2);
  e2.restore(snap);
  TEST_ASSERT_TRUE(e2.waitingConfirm());
  o = e2.tick(in);
  TEST_ASSERT_EQUAL_UINT8(0, e2.stepIdx());
  // conferma: si passa allo step 2, che parte da adesso
  TEST_ASSERT_TRUE(e2.confirm(in.nowEpoch, 19.0f, true));
  TEST_ASSERT_FALSE(e2.waitingConfirm());
  TEST_ASSERT_EQUAL_UINT8(1, e2.stepIdx());
  TEST_ASSERT_EQUAL_UINT32(in.nowEpoch, e2.stepStartEpoch());
  TEST_ASSERT_FALSE(e2.confirm(in.nowEpoch, 19.0f, true));  // niente da confermare
}

static void test_conferma_sull_ultimo_step_e_salta() {
  StepDef s;
  s.type = StepType::Fixed; s.enabled = true; s.targetC = 19.0f;
  s.durationSec = 600; s.confirm = true;
  ProfileEngine e;
  e.configure(0.5f);
  e.setSteps(&s, 1);
  e.startProfile(T0, 19.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 19.0f; in.tempValid = true; in.nowEpoch = T0 + 601;
  e.tick(in);
  TEST_ASSERT_TRUE(e.waitingConfirm());
  TEST_ASSERT_FALSE(e.completed());
  TEST_ASSERT_TRUE(e.skip(in.nowEpoch, 19.0f, true));  // «Salta step» vale come conferma
  TEST_ASSERT_TRUE(e.completed());
  TEST_ASSERT_FALSE(e.waitingConfirm());
}

static void test_stabile_ignora_un_valore_isolato() {
  ProfileEngine e;
  e.configure(0.5f);
  StepDef s = gravStep(19.0f, ax::GravityMode::Stable, 0.0f);
  s.stableBand = 0.002f;
  s.stableHours = 2.0f;
  e.setSteps(&s, 1);
  e.startProfile(T0, 19.0f, true);
  ProfileEngine::TickInput in{};
  in.tempC = 19.0f; in.tempValid = true;
  in.gravityValid = true; in.gravityFresh = true;
  in.gravity = 1.012f; in.gravityEpoch = T0 + 1; in.nowEpoch = T0 + 1; e.tick(in);
  // un valore anomalo isolato (letto per piu' tick), poi di nuovo in banda
  in.gravity = 1.016f; in.gravityEpoch = T0 + 1800;
  for (uint32_t t = 1800; t < 2400; t += 60) { in.nowEpoch = T0 + t; e.tick(in); }
  in.gravity = 1.012f; in.gravityEpoch = T0 + 2700; in.nowEpoch = T0 + 2700; e.tick(in);
  in.nowEpoch = T0 + 7202; e.tick(in);
  TEST_ASSERT_TRUE(e.completed());  // la finestra non e' ripartita

  // due letture fuori banda di fila: la finestra riparte
  ProfileEngine f;
  f.configure(0.5f);
  f.setSteps(&s, 1);
  f.startProfile(T0, 19.0f, true);
  in.gravity = 1.012f; in.gravityEpoch = T0 + 1; in.nowEpoch = T0 + 1; f.tick(in);
  in.gravity = 1.008f; in.gravityEpoch = T0 + 1800; in.nowEpoch = T0 + 1800; f.tick(in);
  in.gravity = 1.008f; in.gravityEpoch = T0 + 2700; in.nowEpoch = T0 + 2700; f.tick(in);
  in.nowEpoch = T0 + 7202; f.tick(in);
  TEST_ASSERT_FALSE(f.completed());  // ripartita a T0+2700: servono altre 2 ore
  in.nowEpoch = T0 + 2700 + 7201; f.tick(in);
  TEST_ASSERT_TRUE(f.completed());
}


// ---- 3.17.0: ventola ----
static void test_ventola_segue_con_post_funzionamento() {
  FanControl f;
  FanControl::Cfg c;
  c.postRunSec = 120;
  f.configure(c);
  TEST_ASSERT_FALSE(f.tick(0, true, false, false));
  TEST_ASSERT_TRUE(f.tick(10, true, false, true));     // freddo acceso
  TEST_ASSERT_TRUE(f.tick(100, true, false, true));
  TEST_ASSERT_TRUE(f.tick(219, true, false, false));   // post-funzionamento
  TEST_ASSERT_FALSE(f.tick(220, true, false, false));
  TEST_ASSERT_TRUE(f.tick(300, true, true, false));    // caldo
  TEST_ASSERT_FALSE(f.tick(301, false, true, false));  // regolazione ferma: spenta
  TEST_ASSERT_FALSE(f.tick(302, true, false, false));  // ripresa: niente post-run vecchio
}

static void test_ventola_ricircolo_e_sempre() {
  FanControl f;
  FanControl::Cfg c;
  c.postRunSec = 0;
  c.cycleOnSec = 300;     // 5 minuti
  c.cycleEverySec = 3600; // ogni ora
  f.configure(c);
  TEST_ASSERT_FALSE(f.tick(0, true, false, false));
  TEST_ASSERT_FALSE(f.tick(3299, true, false, false));
  TEST_ASSERT_TRUE(f.tick(3300, true, false, false));  // 55 min spenta: parte il ricircolo
  TEST_ASSERT_TRUE(f.tick(3599, true, false, false));
  TEST_ASSERT_FALSE(f.tick(3600, true, false, false)); // 5 minuti fatti
  TEST_ASSERT_FALSE(f.tick(6898, true, false, false));
  TEST_ASSERT_TRUE(f.tick(6899, true, false, false));  // un'ora dopo l'ultima accensione
  // un'accensione per il freddo conta come ricircolo
  TEST_ASSERT_TRUE(f.tick(7500, true, false, true));
  TEST_ASSERT_FALSE(f.tick(7501, true, false, false));
  TEST_ASSERT_FALSE(f.tick(10799, true, false, false));
  TEST_ASSERT_TRUE(f.tick(10800, true, false, false));
  c.mode = FanControl::kSempre;
  f.configure(c);
  TEST_ASSERT_TRUE(f.tick(20000, true, false, false));
  TEST_ASSERT_FALSE(f.tick(20001, false, false, false));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hysteresis_thresholds);
  RUN_TEST(test_hysteresis_min_state_time);
  RUN_TEST(test_interlock_never_both);
  RUN_TEST(test_interlock_switch_dead_time);
  RUN_TEST(test_compressor_rest_across_reboot);
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
  RUN_TEST(test_profile_fixed_advances);
  RUN_TEST(test_profile_ramp_time_midpoint);
  RUN_TEST(test_profile_resume_after_blackout);
  RUN_TEST(test_profile_blackout_past_multiple_steps);
  RUN_TEST(test_profile_interruzione_breve_tiene_il_passo);
  RUN_TEST(test_profile_attendi_il_target);
  RUN_TEST(test_profile_attendi_il_target_scade);
  RUN_TEST(test_profile_attendi_il_target_blackout);
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
  RUN_TEST(test_pipeline_stop_forzato_non_insegna);
  RUN_TEST(test_pipeline_opposite_side_discards_observation);
  RUN_TEST(test_gravity_attenuation_trigger);
  RUN_TEST(test_gravity_attenuation_without_og_waits_then_timeout);
  RUN_TEST(test_gravity_timeout_beats_stale_sensor);
  RUN_TEST(test_gravity_sorgente_presente_ma_muta_scade_ancora);
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
  RUN_TEST(test_heater_timeout_scatta_al_limite);
  RUN_TEST(test_heater_timeout_si_azzera_quando_il_caldo_si_spegne);
  RUN_TEST(test_heater_sonda_ferma_a_caldo_pieno);
  RUN_TEST(test_heater_sonda_che_sale_non_blocca);
  RUN_TEST(test_heater_sonda_che_si_ferma_dopo_aver_salito);
  RUN_TEST(test_heater_salita_a_caldo_spento);
  RUN_TEST(test_heater_lenta_deriva_a_caldo_spento_non_blocca);
  RUN_TEST(test_heater_freddo_inefficace);
  RUN_TEST(test_heater_freddo_che_raffredda_non_blocca);
  RUN_TEST(test_heater_freddo_vicino_al_setpoint_non_blocca);
  RUN_TEST(test_heater_salto_della_sonda_non_blocca);
  RUN_TEST(test_heater_disarmato_azzera_e_sonda_invalida_congela);
  RUN_TEST(test_sonda_classificazione_guasti);
  RUN_TEST(test_cold_crash_stima_e_allarme);
  RUN_TEST(test_taratura_segue_la_sonda);
  RUN_TEST(test_penalita_tentativi);
  RUN_TEST(test_penalita_per_indirizzo);
  RUN_TEST(test_firmware_immagine_verificata);
  RUN_TEST(test_firmware_vero_verificato);
  RUN_TEST(test_coolwatch_rele_incollato_dopo_lo_stop);
  RUN_TEST(test_coolwatch_stop_vero_non_allarma);
  RUN_TEST(test_coolwatch_falso_allarme_si_smentisce);
  RUN_TEST(test_coolwatch_garage_freddo_non_allarma);
  RUN_TEST(test_coolwatch_crollo_a_frigo_fermo);
  RUN_TEST(test_coolwatch_porta_aperta_non_allarma);
  RUN_TEST(test_coolwatch_modo_fermo_rele_incollato);
  RUN_TEST(test_slope_stima_pendenza_e_arrivo);
  RUN_TEST(test_slope_quantizzazione_e_buco_della_sonda);
  RUN_TEST(test_aria_limite_freddo_e_caldo);
  RUN_TEST(test_minmax_conserva_picchi_e_ordine);
  RUN_TEST(test_minmax_serie_assente_e_reset);
  RUN_TEST(test_eventi_vergine_e_ripresa);
  RUN_TEST(test_eventi_giro_completo_e_contenuto_estraneo);
  RUN_TEST(test_allarmi_antirimbalzo);
  RUN_TEST(test_trend_attiva_stabile_rallenta);
  RUN_TEST(test_trend_dati_insufficienti_o_vecchi);
  RUN_TEST(test_conversione_plato_sg);
  RUN_TEST(test_step_attende_la_conferma);
  RUN_TEST(test_conferma_sull_ultimo_step_e_salta);
  RUN_TEST(test_stabile_ignora_un_valore_isolato);
  RUN_TEST(test_ventola_segue_con_post_funzionamento);
  RUN_TEST(test_ventola_ricircolo_e_sempre);
  return UNITY_END();
}
