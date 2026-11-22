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
  out->param_count = 0;
  out->reset_reason = s->reset_reason;
  out->checks = s->checks;
  strncpy(out->text, "Flamekeeper safety 0.1.0", LINK_TEXT_MAX);
  out->text[LINK_TEXT_MAX] = 0;
}

static int starts_refused(const safety *s) { return s->no_starts || s->esp_updating || s->checks != 0; }

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
    else
      s->start_result = (uint8_t) burn_start(b, want->mode, want->power, s->now, &s->in);
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

static int on_fault_reset(void *ctx) {
  safety *s = (safety *) ctx;
  /* The stages' own faults clear with a user reset too, once the burn agrees. */
  burn_inputs in = s->in;
  in.glow_fault = 0;
  in.fan_fault = 0;
  int r = burn_fault_reset(&s->burn, &in);
  if (r == LINK_OK) {
    if (s->glow.fault != GLOW_FAULT_NONE)
      glow_init(&s->glow, &s->glow.p, s->now);
    if (s->fan.fault != FAN_FAULT_NONE)
      fan_init(&s->fan, &s->fan.p, s->now);
    s->in.glow_fault = 0;
    s->in.fan_fault = 0;
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
                 uint32_t now, const burn_inputs *in, uint8_t reset_reason, uint8_t checks) {
  memset(s, 0, sizeof(*s));
  s->now = now;
  s->in = *in;
  s->reset_reason = reset_reason;
  s->checks = checks;
  burn_init(&s->burn, p, now, in);
  glow_init(&s->glow, gp, now);
  fan_init(&s->fan, fp, now);
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

uint16_t safety_fan_step(safety *s, uint32_t now, const fan_meas *m) {
  return fan_step(&s->fan, now, s->burn.out.fan_rpm, m);
}

glow_cmd safety_glow_period(safety *s, uint32_t now, glow_meas *m) {
  /* Airflow as the sequencer sees it too: both must agree (GLW-14). */
  if (s->in.fan_rpm < s->burn.p.glow_min_fan_rpm)
    m->airflow_ok = 0;
  return glow_period(&s->glow, now, s->burn.out.glow_w, m);
}
