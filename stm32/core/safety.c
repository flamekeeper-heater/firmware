/* The safety MCU's side of the link: see safety.h. */
#include "safety.h"

#include <string.h>

/* One "not known" temperature everywhere: the CO module and the link use their own names for it. */
typedef char unknown_temps_agree[CO_UNKNOWN_TEMP == UNKNOWN_TEMP && LINK_UNKNOWN_I16 == UNKNOWN_TEMP ? 1 : -1];

static void on_info(void *ctx, link_info *out) {
  safety *s = (safety *) ctx;
  out->kind = LINK_KIND_STM32;
  out->version[0] = SAFETY_VERSION_MAJOR;
  out->version[1] = SAFETY_VERSION_MINOR;
  out->version[2] = SAFETY_VERSION_PATCH;
  out->capabilities = SAFETY_CAPABILITIES | (s->co.cfg.fitted ? LINK_CAP_CO : 0u);
  out->param_count = (uint8_t) PARAM_COUNT;
  out->reset_reason = s->reset_reason;
  out->checks = s->checks;
#ifdef SAFETY_BENCH
  strncpy(out->text, "Flamekeeper bring-up " SAFETY_VERSION_TEXT, LINK_TEXT_MAX); /* not for a heater in use */
#else
  strncpy(out->text, "Flamekeeper safety " SAFETY_VERSION_TEXT, LINK_TEXT_MAX);
#endif
  out->text[LINK_TEXT_MAX] = 0;
}

static int starts_refused(const safety *s) {
  return s->no_starts || s->esp_updating || s->checks != 0 || !s->chk.power_up_done;
}

static void fill_status(safety *s, link_status *o) {
  const burn *b = &s->burn;
  o->state = burn_state(b);
  o->detail = b->step;
  o->fault = b->fault;
  o->fault_detail = b->why;
  o->flags = 0;
  if (b->fault_latched)
    o->flags |= LINK_STATUS_FAULT_LATCHED;
  if (s->in.supply_cv >= SAFETY_SUPPLY_PRESENT_CV)
    o->flags |= LINK_STATUS_SUPPLY_PRESENT;
  if (s->in.overheat_latch)
    o->flags |= LINK_STATUS_OVERHEAT_TRIPPED;
  if (b->why == BURN_WHY_DEADMAN)
    o->flags |= LINK_STATUS_DEADMAN_STOP;
  if (starts_refused(s))
    o->flags |= LINK_STATUS_NO_STARTS;
  if (s->params.staged_mask)
    o->flags |= LINK_STATUS_PARAMS_STAGED;
  if (s->plug_warning)
    o->flags |= LINK_STATUS_PLUG_WARNING;
  o->start_ack = s->gate.last;
  o->start_result = s->start_result;
  o->power = b->running_power;
  o->heat_exchanger = s->in.hx_c10;
  o->supply = s->in.supply_cv;
  o->fan_rpm = s->in.fan_rpm;
  o->pump = b->out.pump_cHz;
  o->glow_w = b->out.glow_w < s->glow_limit_w ? b->out.glow_w : s->glow_limit_w; /* PWR-2 */
  o->prime_left_s = burn_prime_left_s(b, s->now);
  o->run_time_s = s->run_time_s;
}

static void on_poll(void *ctx, const link_desired *want, link_status *out) {
  safety *s = (safety *) ctx;
  burn *b = &s->burn;
  s->esp_updating = (want->flags & LINK_WANT_UPDATING) != 0;

  if (link_start_gate_check(&s->gate, want->start, want->mode)) {
    /* A new start number: taken as acted on whatever happens, so a refusal is not retried. */
    uint32_t free_a2s = s->glow.p.bucket_cap_a2s - glow_bucket_a2s(&s->glow);
    if (starts_refused(s))
      s->start_result = LINK_BUSY;
    else if (want->mode == LINK_MODE_HEAT &&
             free_a2s * 100u < s->glow.p.bucket_cap_a2s * SAFETY_GLOW_START_FREE_PERCENT)
      s->start_result = LINK_BUSY; /* the glow plug's stage is still resting */
    else {
      s->start_result = (uint8_t) burn_start(b, want->mode, want->power, s->now, &s->in);
      if (s->start_result == LINK_OK) {
        pump_new_run(&s->pump); /* a new run: the high side's one re-arm again */
        /* The plug's cold state is taken now, before any glow of this start. */
        plug_begin(&s->plug, s->now, s->in.hx_c10, s->in.board_c10);
        s->plug_judged = 0;
        s->plug_refused = 0;
        s->plug_warning = 0;
      }
    }
  }
  if (want->mode == LINK_MODE_OFF) {
    if (burn_active(b))
      burn_stop(b, BURN_WHY_ASKED, s->now);
  } else {
    burn_set_power(b, want->power);
  }
  fill_status(s, out);
}

static void on_detail(void *ctx, link_detail *out) {
  safety *s = (safety *) ctx;
  const co_alarm *c = &s->co;
  *out = s->detail;
  out->co_state = c->state;
  out->co_ppm = LINK_UNKNOWN_U16;
  if (c->state == CO_LEARNING || c->state == CO_OK || c->state == CO_ALARM || c->state == CO_TOO_COLD) {
    if (c->ppm != CO_UNKNOWN_PPM)
      out->co_ppm = c->ppm;
    else if (c->ratio != 0xFFFFu)
      out->co_ppm = 0; /* cleaner than the curve's end (about 25 ppm): no CO it can see */
  }
}

/* ---------------------------------------------------------------- the CO alarm */

/* The table's parameters take effect: the sequencer runs on them, and the CO-fitted parameter alone
 * says whether the sensor is there (CO-1; a change, heater off only, starts the alarm afresh). */
static void take_params(safety *s) {
  burn_set_params(&s->burn, &s->params.live);
  co_set_fitted(&s->co, s->burn.p.co_fitted);
}

void safety_co_step(safety *s, uint32_t now, uint16_t adc, int16_t board_c10) {
  co_step(&s->co, now, adc, board_c10);
}

void safety_load_co_r0(safety *s, uint32_t r0_ohm) {
  s->co_r0_saved = r0_ohm;
  co_load_r0(&s->co, r0_ohm);
}

uint32_t safety_co_r0_to_save(const safety *s) {
  const co_alarm *c = &s->co;
  uint32_t r0 = c->r0, old = s->co_r0_saved;
  if (c->state != CO_OK || r0 == 0) /* learnt, warm enough, no alarm or fault */
    return 0;
  if (old == 0)
    return r0;
  uint32_t diff = r0 > old ? r0 - old : old - r0;
  if (diff < old / 16u || (uint32_t) (s->now - s->co_r0_saved_ms) < SAFETY_CO_R0_SAVE_MIN_MS)
    return 0;
  return r0;
}

void safety_co_r0_saved(safety *s, uint32_t r0_ohm) {
  s->co_r0_saved = r0_ohm;
  s->co_r0_saved_ms = s->now;
}

/* ---------------------------------------------------------------- parameters */

static int on_param_describe(void *ctx, uint8_t index, link_param_desc *out) {
  (void) ctx;
  if (index >= PARAM_COUNT)
    return LINK_OUT_OF_RANGE;
  const param_def *d = &PARAM_TABLE[index];
  out->index = index;
  out->count = (uint8_t) PARAM_COUNT;
  out->id = d->id;
  out->flags = d->flags;
  out->unit = d->unit;
  out->decimals = d->decimals;
  out->min = d->min, out->max = d->max, out->step = d->step;
  out->group = d->group;
  out->level = d->level;
  strncpy(out->name, d->name, LINK_PARAM_NAME_MAX);
  out->name[LINK_PARAM_NAME_MAX] = 0;
  return LINK_OK;
}

static int on_param_get(void *ctx, uint16_t id, link_param_value *out) {
  safety *s = (safety *) ctx;
  const param_def *d = params_find(id);
  if (d == NULL)
    return LINK_OUT_OF_RANGE;
  size_t i = (size_t) (d - PARAM_TABLE);
  out->id = id;
  out->value = params_value(&s->params, i);
  out->flags = LINK_PARAM_KNOWN;
  out->staged = out->value;
  if (s->params.staged_mask & (1u << i)) {
    out->flags |= LINK_PARAM_STAGED;
    out->staged = params_staged(&s->params, i);
  }
  return LINK_OK;
}

static int on_param_set(void *ctx, uint16_t id, int32_t value) {
  return params_set(&((safety *) ctx)->params, id, value);
}

/* The sequencer takes new parameters only while the heater is off. */
static int heater_off(const safety *s) { return burn_is_off(&s->burn); }

/* The staged changes would mark a fitted CO sensor as not fitted. */
static int co_unfit_staged(const safety *s) {
  for (size_t i = 0; i < PARAM_COUNT; i++)
    if (PARAM_TABLE[i].offset == offsetof(burn_params, co_fitted))
      return (s->params.staged_mask & (1u << i)) && params_staged(&s->params, i) == 0 && s->burn.p.co_fitted;
  return 0;
}

static int on_param_apply(void *ctx, uint8_t save) {
  safety *s = (safety *) ctx;
  /* "Not fitted" starts the CO alarm afresh: never while it alarms, or a latched alarm would clear
   * without its minute of clean air (it stays staged). A sensor fault may go this way: a board
   * without the sensor shows one until it is set (CO-1). */
  if (s->co.alarm && co_unfit_staged(s))
    return LINK_REFUSED;
  int r = params_apply(&s->params, heater_off(s), save);
  if (r == LINK_OK)
    take_params(s);
  return r;
}

static int on_param_discard(void *ctx) {
  params_discard(&((safety *) ctx)->params);
  return LINK_OK;
}

static int on_param_defaults(void *ctx) {
  safety *s = (safety *) ctx;
  if (!heater_off(s))
    return LINK_NOT_OFF;
  params_defaults(&s->params);
  take_params(s);
  return LINK_OK;
}

#ifdef SAFETY_BENCH
int safety_bench_tune(safety *s, const param_def *d, int32_t value) {
  if (!heater_off(s))
    return LINK_NOT_OFF;
  params_write_field(&s->params.live, d, value);
  take_params(s);
  return LINK_OK;
}
#endif

static int on_glow_new_plug(void *ctx) {
  safety *s = (safety *) ctx;
  plug_new_plug(&s->plug);
  s->plug_warning = 0;
  return LINK_OK;
}

static int on_param_help(void *ctx, uint16_t id, const char **text) {
  (void) ctx;
  const param_def *d = params_find(id);
  if (d == NULL)
    return LINK_OUT_OF_RANGE;
  *text = d->help;
  return LINK_OK;
}

void safety_load_plug(safety *s, const uint8_t *image, size_t len) { plug_unpack(&s->plug, image, len); }

void safety_load_params(safety *s, const uint8_t *image, size_t len) {
  /* At boot, before any start: the sequencer may already be cooling down after a reset with a hot
   * heat exchanger, and does that with the saved values too. */
  if (params_unpack(&s->params, image, len))
    take_params(s);
}

/* ---------------------------------------------------------------- faults */

/* The monitor's fault as the sequencer's why (0: none). */
static uint8_t mcu_why(const monitor *m) {
  switch (m->fault) {
    case MONITOR_VDDA_HIGH: return BURN_WHY_VDDA_HIGH;
    case MONITOR_VDDA_LOW: return BURN_WHY_VDDA_LOW;
    case MONITOR_TICK: return BURN_WHY_TICK;
    default: return 0;
  }
}

void safety_vdda(safety *s, uint32_t now, uint16_t vdda_mv) { monitor_vdda(&s->mon, now, vdda_mv); }

void safety_clock_check(safety *s, uint32_t now, uint32_t count) { monitor_clock(&s->mon, now, count); }

static int on_fault_reset(void *ctx) {
  safety *s = (safety *) ctx;
  /* The CO alarm's latches first: an alarm clears once the air has been clean a minute, a sensor
   * fault once the reading is plausible (and its heater check runs again). */
  if (s->co.alarm || s->co.fault)
    (void) co_reset(&s->co, s->now);
  /* The MCU's own supply or tick: once its cause has gone. */
  if (s->mon.fault != MONITOR_OK)
    (void) monitor_clear(&s->mon);
  /* The stages' own faults clear with a user reset too, once the burn agrees. */
  burn_inputs in = s->in;
  in.mcu_fault = mcu_why(&s->mon);
  in.co_alarm = s->co.alarm;
  in.co_fault = s->co.fault;
  in.glow_fault = 0;
  in.fan_fault = 0;
  in.pump_fault = 0;
  int r = burn_fault_reset(&s->burn, &in);
  if (r == LINK_OK) {
    if (s->glow.fault != GLOW_FAULT_NONE)
      glow_clear_fault(&s->glow); /* the heat budget stays as used (GLW-21) */
    if (s->fan.fault != FAN_FAULT_NONE)
      fan_clear_fault(&s->fan, s->now);
    if (s->pump.fault != PUMP_FAULT_NONE)
      pump_clear_fault(&s->pump);
    s->in.glow_fault = 0;
    s->in.fan_fault = 0;
    s->in.pump_fault = 0;
    s->in.co_alarm = 0;
    s->in.co_fault = 0;
    s->in.mcu_fault = 0;
  }
  return r;
}

static int on_update_prepare(void *ctx, uint8_t *method) {
  safety *s = (safety *) ctx;
  const burn *b = &s->burn;
  /* Only with the heater off and cold (DESIGN.md): the MCU then refuses starts until it resets. */
  if (!burn_is_off(b))
    return LINK_NOT_OFF;
  if (s->in.hx_c10 == BURN_UNKNOWN_TEMP || s->in.hx_c10 >= b->p.hot_start_c10)
    return LINK_NOT_OFF;
  s->no_starts = 1;
  *method = 1; /* the STM32 ROM bootloader on this link */
  return LINK_OK;
}

void safety_init(safety *s, uint32_t now, const burn_inputs *in, uint8_t reset_reason, uint8_t checks) {
  memset(s, 0, sizeof(*s));
  s->now = now;
  s->in = *in;
  s->reset_reason = reset_reason;
  s->checks = checks;
  /* Every stage's own figures are its defaults; the sequencer's are the parameter table's. */
  union {
    plug_params plug;
    glow_params glow;
    fan_params fan;
    checks_params chk;
    pump_params pump;
    co_config co;
    monitor_params mon;
    budget_params budget;
  } d;
  plug_params_default(&d.plug);
  plug_init(&s->plug, &d.plug, now);
  glow_params_default(&d.glow);
  glow_init(&s->glow, &d.glow, now);
  fan_params_default(&d.fan);
  fan_init(&s->fan, &d.fan, now);
  checks_params_default(&d.chk);
  checks_init(&s->chk, &d.chk, now);
  pump_params_default(&d.pump);
  pump_init(&s->pump, &d.pump);
  monitor_params_default(&d.mon, SAFETY_CLOCK_HZ, SAFETY_CLOCK_WRAP);
  monitor_init(&s->mon, &d.mon, now);
  budget_params_default(&d.budget);
  budget_init(&s->budget, &d.budget, now);
  s->glow_limit_w = 0xFFFFu;
  params_init(&s->params);
  burn_init(&s->burn, &s->params.live, now, in);
  co_default_config(&d.co);
  d.co.fitted = s->params.live.co_fitted ? 1u : 0u;
  co_init(&s->co, &d.co);
  s->co_r0_saved_ms = now;
  link_parser_init(&s->parser, LINK_REQUEST);
  link_start_gate_init(&s->gate);
  link_deadman_init(&s->deadman);
  link_detail_unknown(&s->detail);
  s->mcu.ctx = s;
  s->mcu.info = on_info;
  s->mcu.poll = on_poll;
  s->mcu.detail = on_detail;
  s->mcu.fault_reset = on_fault_reset;
  s->mcu.update_prepare = on_update_prepare;
  s->mcu.param_describe = on_param_describe;
  s->mcu.param_get = on_param_get;
  s->mcu.param_set = on_param_set;
  s->mcu.param_apply = on_param_apply;
  s->mcu.param_discard = on_param_discard;
  s->mcu.param_defaults = on_param_defaults;
  s->mcu.param_help = on_param_help;
  s->mcu.glow_new_plug = on_glow_new_plug;
  s->mcu.client_major = &s->client_major;
}

size_t safety_receive(safety *s, uint8_t byte, uint32_t now, uint8_t *reply) {
  if (!link_parser_feed(&s->parser, byte))
    return 0;
  /* Any valid frame is a sign of life (the dead-man), whatever it asks. */
  link_deadman_feed(&s->deadman, now);
  s->now = now;
  return link_server_handle(&s->mcu, s->parser.buf, reply);
}

void safety_step(safety *s, uint32_t now, const burn_inputs *in) {
  uint32_t dt = (uint32_t) (now - s->now);
  s->now = now;
  s->in = *in;
  s->in.glow_fault = (uint8_t) (in->glow_fault || s->glow.fault != GLOW_FAULT_NONE);
  s->in.fan_fault = in->fan_fault ? in->fan_fault : s->fan.fault;
  s->in.precheck = s->precheck_result;
  s->in.pump_fault = in->pump_fault ? in->pump_fault : s->pump.fault;
  s->in.plug_refused = s->plug_refused;
  s->in.co_alarm = (uint8_t) (in->co_alarm || s->co.alarm);
  /* OVH-1: the latch reads tripped after every power-up until the self-test re-arms it; only a fall
   * after that is a hardware overheat trip. */
  s->in.overheat_latch = (uint8_t) (in->overheat_latch || (s->latch_was_armed && !s->latch_armed));
  s->in.co_fault = (uint8_t) (in->co_fault || s->co.fault);
  s->in.mcu_fault = in->mcu_fault ? in->mcu_fault : mcu_why(&s->mon);
  if (burn_burning(&s->burn)) {
    s->run_ms += dt;
    s->run_time_s += s->run_ms / 1000u;
    s->run_ms %= 1000u;
  }
  s->burn.fan_budget_ma = budget_fan_allowed_ma(&s->budget); /* PWR-2: then the fan */
  if (link_deadman_expired(&s->deadman, now, burn_active(&s->burn)))
    burn_stop(&s->burn, BURN_WHY_DEADMAN, now); /* a full burn-off and cooldown, on its own */
  burn_step(&s->burn, now, &s->in);
}

const burn_outputs *safety_outputs(const safety *s) { return &s->burn.out; }

safety_io safety_ms(safety *s, uint32_t now, const checks_obs *o, pump_meas *pm) {
  checks *c = &s->chk;
  s->latch_armed = o->latch_armed;
  budget_pump_ms(&s->budget, pm->i_ma); /* PWR-1: the pump's share */
  if (pm->overcurrent)
    pump_cut(&s->pump); /* the watchdog has cut already; whatever runs, the fault latches now */
  /* The power-up test waits for VDDA to be up (PWR-5: a rail still rising after the reset). */
  if (!s->power_up_started && !monitor_holding(&s->mon)) {
    s->power_up_started = 1;
    checks_begin_power_up(c, now, o);
  }
  /* A new start waiting for its pre-start test. */
  if (s->burn.step == BURN_STEP_PRE_CHECK && !s->precheck_started && !checks_busy(c)) {
    s->precheck_started = 1;
    s->precheck_result = BURN_PRECHECK_RUNNING;
    checks_begin_pre_start(c, now);
  }
  if (s->burn.step != BURN_STEP_PRE_CHECK)
    s->precheck_started = 0, s->precheck_result = BURN_PRECHECK_RUNNING;

  int was_power_up = c->kind == CHECKS_POWER_UP, was_pre_start = c->kind == CHECKS_PRE_START;
  checks_out out = checks_step(c, now, o);
  if (was_power_up && !checks_busy(c)) {
    s->power_up_failed = c->failed;
    if (c->failed)
      s->checks |= LINK_CHECK_SELF_TEST; /* no starts until the MCU resets */
  }
  if (c->power_up_done && s->power_up_failed == 0 && o->latch_armed)
    s->latch_was_armed = 1;
  if (was_pre_start && !checks_busy(c))
    s->precheck_result = c->failed == 0                      ? BURN_PRECHECK_PASSED
                         : (c->failed & CHECK_FAILS_PUMP) != 0 ? BURN_PRECHECK_FAILED_PUMP
                         : (c->failed & CHECK_FAILS_GLOW) != 0 ? BURN_PRECHECK_FAILED_GLOW
                                                               : BURN_PRECHECK_FAILED;
  safety_io io = {out.heartbeat, out.glow_en, out.latch_reset, out.pump_pulse_us, 0, 0};
  if (!checks_busy(c) && !was_power_up && !was_pre_start) {
    /* The pump strokes, and the heartbeat the burn needs unless the pump holds it off. */
    pm->hb_running_ms = s->hb_running_ms;
    uint16_t rate = s->burn.out.pump_cHz;
    int hb = burn_heartbeat_wanted(&s->burn); /* it enables both high sides */
#ifdef SAFETY_BENCH
    if (s->burn.step == BURN_STEP_OFF) {
      rate = s->bench.pump_cHz;
      hb = s->bench.heartbeat;
    }
#endif
    pump_out po = pump_ms(&s->pump, now, rate, s->burn.step == BURN_STEP_PRIMING, pm);
    io.pump_duty = po.duty;
    io.pump_oc_armed = po.oc_armed;
    io.heartbeat = hb && !po.hb_hold_off ? CHECKS_HB_RUNNING : CHECKS_HB_STOPPED;
  }
  if (monitor_holding(&s->mon)) {
    /* PWR-5: VDDA out of its window (or still rising): every enable off, the heartbeat stopped. */
    memset(&io, 0, sizeof(io));
    io.heartbeat = CHECKS_HB_STOPPED;
  }
  if (io.heartbeat == CHECKS_HB_RUNNING)
    s->hb_running_ms = (uint16_t) (s->hb_running_ms < 60000u ? s->hb_running_ms + 1u : 60000u);
  else
    s->hb_running_ms = 0;
  return io;
}

uint16_t safety_fan_step(safety *s, uint32_t now, const fan_meas *m) {
  budget_pass(&s->budget, now, m->isense_ma, s->in.supply_cv); /* PWR-1: the fan and logic shares */
#ifdef SAFETY_BENCH
  if (s->burn.step == BURN_STEP_OFF && s->bench.fan_duty > 0)
    return fan_step_open(&s->fan, now, s->bench.fan_duty, m);
  if (s->burn.step == BURN_STEP_OFF)
    return fan_step(&s->fan, now, s->bench.fan_rpm, m);
#endif
  return fan_step(&s->fan, now, s->burn.out.fan_rpm, m);
}

glow_cmd safety_glow_period(safety *s, uint32_t now, glow_meas *m) {
  /* Airflow as the sequencer sees it too: both must agree (GLW-14). */
  if (s->in.fan_rpm < s->burn.p.glow_min_fan_rpm)
    m->airflow_ok = 0;
  uint16_t power_w = s->burn.out.glow_w;
#ifdef SAFETY_BENCH
  int bench_probe = s->burn.step == BURN_STEP_OFF && s->bench.glow_probe && !checks_busy(&s->chk) &&
                    s->hb_running_ms >= SAFETY_BENCH_GLOW_HB_MS;
  if (bench_probe)
    power_w = s->burn.p.glow_w; /* the planner's first pulse is always a probe (GLW-5) */
#endif
  if (monitor_holding(&s->mon))
    power_w = 0; /* PWR-5: GLOW_EN low, no pulse */
  /* PWR-2: the glow first, held to what the input budget leaves after the fan, pump and logic. */
  s->glow_limit_w = budget_glow_limit_w(&s->budget, m->vin_cv);
  if (power_w > s->glow_limit_w)
    power_w = s->glow_limit_w;
  uint16_t last_width = s->glow.last_width_us; /* the pulse that went out last period */
  glow_cmd c = glow_period(&s->glow, now, power_w, m);
  if (last_width > 0)
    budget_glow_pulse(&s->budget, m->pulse_ca, last_width); /* PWR-1: its share, as read in it */
#ifdef SAFETY_BENCH
  if (bench_probe && c.width_us > 0)
    s->bench.glow_probe = 0; /* one pulse, then the plug is off again */
#endif
  /* The plug-type check (GLW-10 to GLW-13), once per start, from the first probe's reading. */
  if (s->burn.step == BURN_STEP_PREHEAT && !s->plug_judged && s->glow.have_reading) {
    s->plug_judged = 1;
    uint8_t v = plug_judge(&s->plug, s->burn.p.plug_v, s->glow.vin_then_cv, s->glow.i_ca);
    if (v >= PLUG_REFUSE_RANGE)
      s->plug_refused = v;
    else if (v == PLUG_WARN)
      s->plug_warning = 1;
  }
  if (c.width_us > 0)
    plug_glow_seen(&s->plug, now);
  return c;
}
