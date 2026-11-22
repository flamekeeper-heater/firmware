/* The safety MCU's side of the link: see safety.h. */
#include "safety.h"

#include <string.h>

static void on_info(void *ctx, link_info *out) {
  safety *s = (safety *) ctx;
  out->kind = LINK_KIND_STM32;
  out->version[0] = SAFETY_VERSION_MAJOR;
  out->version[1] = SAFETY_VERSION_MINOR;
  out->version[2] = SAFETY_VERSION_PATCH;
  out->capabilities = SAFETY_CAPABILITIES;
  out->param_count = (uint8_t) PARAM_COUNT;
  out->reset_reason = s->reset_reason;
  out->checks = s->checks;
  strncpy(out->text, "Flamekeeper safety 0.1.0", LINK_TEXT_MAX);
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
  o->start_ack = s->gate.last;
  o->start_result = s->start_result;
  o->power = b->running_power;
  o->heat_exchanger = s->in.hx_c10;
  o->supply = s->in.supply_cv;
  o->fan_rpm = s->in.fan_rpm;
  o->pump = b->out.pump_cHz;
  o->glow_w = b->out.glow_w;
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
      if (s->start_result == LINK_OK)
        pump_new_run(&s->pump); /* a new run: the high side's one re-arm again */
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
  *out = s->detail;
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
    out->staged = s->params.staged[i];
  }
  return LINK_OK;
}

static int on_param_set(void *ctx, uint16_t id, int32_t value) {
  return params_set(&((safety *) ctx)->params, id, value);
}

/* The sequencer takes new parameters only while the heater is off. */
static int heater_off(const safety *s) { return s->burn.step == BURN_STEP_OFF; }

static int on_param_apply(void *ctx, uint8_t save) {
  safety *s = (safety *) ctx;
  int r = params_apply(&s->params, heater_off(s), save);
  if (r == LINK_OK)
    s->burn.p = s->params.live;
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
  s->burn.p = s->params.live;
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

void safety_load_params(safety *s, const uint8_t *image, size_t len) {
  if (params_unpack(&s->params, image, len) && heater_off(s))
    s->burn.p = s->params.live;
}

/* ---------------------------------------------------------------- faults */

static int on_fault_reset(void *ctx) {
  safety *s = (safety *) ctx;
  /* The stages' own faults clear with a user reset too, once the burn agrees. */
  burn_inputs in = s->in;
  in.glow_fault = 0;
  in.fan_fault = 0;
  in.pump_fault = 0;
  int r = burn_fault_reset(&s->burn, &in);
  if (r == LINK_OK) {
    if (s->glow.fault != GLOW_FAULT_NONE)
      glow_init(&s->glow, &s->glow.p, s->now);
    if (s->fan.fault != FAN_FAULT_NONE)
      fan_init(&s->fan, &s->fan.p, s->now);
    if (s->pump.fault != PUMP_FAULT_NONE)
      pump_init(&s->pump, &s->pump.p);
    s->in.glow_fault = 0;
    s->in.fan_fault = 0;
    s->in.pump_fault = 0;
  }
  return r;
}

static int on_update_prepare(void *ctx, uint8_t *method) {
  safety *s = (safety *) ctx;
  const burn *b = &s->burn;
  /* Only with the heater off and cold (DESIGN.md): the MCU then refuses starts until it resets. */
  if (b->step != BURN_STEP_OFF)
    return LINK_NOT_OFF;
  if (s->in.hx_c10 == BURN_UNKNOWN_TEMP || s->in.hx_c10 >= b->p.hot_start_c10)
    return LINK_NOT_OFF;
  s->no_starts = 1;
  *method = 1; /* the STM32 ROM bootloader on this link */
  return LINK_OK;
}

void safety_init(safety *s, const burn_params *p, const glow_params *gp, const fan_params *fp,
                 const checks_params *cp, const pump_params *pp, uint32_t now, const burn_inputs *in,
                 uint8_t reset_reason, uint8_t checks) {
  memset(s, 0, sizeof(*s));
  s->now = now;
  s->in = *in;
  s->reset_reason = reset_reason;
  s->checks = checks;
  params_init(&s->params);
  s->params.live = *p;
  burn_init(&s->burn, p, now, in);
  glow_init(&s->glow, gp, now);
  fan_init(&s->fan, fp, now);
  checks_init(&s->chk, cp, now);
  pump_init(&s->pump, pp);
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
  if (burn_fuel_steps(&s->burn) && s->burn.step != BURN_STEP_PRIMING) {
    s->run_ms += dt;
    s->run_time_s += s->run_ms / 1000u;
    s->run_ms %= 1000u;
  }
  if (link_deadman_expired(&s->deadman, now, burn_active(&s->burn)))
    burn_stop(&s->burn, BURN_WHY_DEADMAN, now); /* a full burn-off and cooldown, on its own */
  burn_step(&s->burn, now, &s->in);
}

const burn_outputs *safety_outputs(const safety *s) { return &s->burn.out; }

/* The heartbeat enables both high sides: it runs from the fan check before preheat (so the glow
 * enable comes at least 20 ms after it, GLW-7) through burn-off, and while priming. */
static int heartbeat_wanted(const burn *b) {
  switch (b->step) {
    case BURN_STEP_SELF_TEST:
    case BURN_STEP_PREHEAT:
    case BURN_STEP_IGNITION:
    case BURN_STEP_PURGE:
    case BURN_STEP_WARM_UP:
    case BURN_STEP_RUNNING:
    case BURN_STEP_BURN_OFF:
    case BURN_STEP_PRIMING:
      return 1;
    default:
      return 0;
  }
}

safety_io safety_ms(safety *s, uint32_t now, const checks_obs *o, pump_meas *pm) {
  checks *c = &s->chk;
  if (!s->power_up_started) {
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
  if (was_pre_start && !checks_busy(c))
    s->precheck_result = c->failed == 0                      ? BURN_PRECHECK_PASSED
                         : (c->failed & CHECK_FAILS_PUMP) != 0 ? BURN_PRECHECK_FAILED_PUMP
                         : (c->failed & CHECK_FAILS_GLOW) != 0 ? BURN_PRECHECK_FAILED_GLOW
                                                               : BURN_PRECHECK_FAILED;
  safety_io io = {out.heartbeat, out.glow_en, out.latch_reset, out.pump_pulse_us, 0};
  if (!checks_busy(c) && !was_power_up && !was_pre_start) {
    /* The pump strokes, and the heartbeat the burn needs unless the pump holds it off. */
    pm->hb_running_ms = s->hb_running_ms;
    pump_out po = pump_ms(&s->pump, now, s->burn.out.pump_cHz, s->burn.step == BURN_STEP_PRIMING, pm);
    io.pump_duty = po.duty;
    io.heartbeat = heartbeat_wanted(&s->burn) && !po.hb_hold_off ? CHECKS_HB_RUNNING : CHECKS_HB_STOPPED;
  }
  if (io.heartbeat == CHECKS_HB_RUNNING)
    s->hb_running_ms = (uint16_t) (s->hb_running_ms < 60000u ? s->hb_running_ms + 1u : 60000u);
  else
    s->hb_running_ms = 0;
  return io;
}

uint16_t safety_fan_step(safety *s, uint32_t now, const fan_meas *m) {
  return fan_step(&s->fan, now, s->burn.out.fan_rpm, m);
}

glow_cmd safety_glow_period(safety *s, uint32_t now, glow_meas *m) {
  /* Airflow as the sequencer sees it too: both must agree (GLW-14). */
  if (s->in.fan_rpm < s->burn.p.glow_min_fan_rpm)
    m->airflow_ok = 0;
  return glow_period(&s->glow, now, s->burn.out.glow_w, m);
}
