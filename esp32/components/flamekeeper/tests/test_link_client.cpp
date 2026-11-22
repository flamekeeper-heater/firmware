// The ESP32 side of the link against a simulated safety MCU built on the shared C codec: start-up,
// the start-number rule from both ends, released, lost replies, the dead-man, parameters.
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../link_client.h"
#include "fake_heater.h"

using fk::HeaterState;
using fk::Mode;
using fk::Result;

// A safety MCU: a small burn state machine behind the link callbacks.
struct SimMcu {
  link_start_gate gate;
  link_deadman deadman;
  uint32_t now = 0;
  bool silent = false;  // drops every request (cable unplugged, MCU hung)
  bool no_starts = false;
  bool run_switch = true;  // capability 14: heat <-> fan during a run
  bool switch_by_stop = false;  // switches as some emulators do: stop, cool down, start again
  uint8_t pending = 0;          // the mode to start after that stop
  uint8_t state = 1;    // the app protocol's heater states
  uint8_t ack = 0, start_result = 0;
  int phase = 0;        // polls spent in the current state
  link_desired last{};
  int polls = 0, deadman_stops = 0, resets = 0;
  uint8_t fault = 0;
  uint8_t client_major = 0;  // from the client's HELLO (the server refuses POLL without it)
  struct P {
    uint16_t id;
    int32_t value, staged;
    bool is_staged;
    const char *name;
  };
  std::vector<P> params{{7, 60, 0, false, "Preheat time"}, {0x80, 15, 0, false, "Pump min"}};
  // Id 7: a description in three pieces (120 bytes); id 0x80: none.
  std::string help7 = std::string(100, 'a') + std::string(20, 'b');
  int help_asks = 0;

  SimMcu() {
    link_start_gate_init(&gate);
    link_deadman_init(&deadman);
  }
  bool burning() const { return (state >= 2 && state <= 4) || state == 7; }

  void poll(const link_desired *w, link_status *o) {
    polls++;
    last = *w;
    link_deadman_feed(&deadman, now);
    bool off_wanted = w->mode == LINK_MODE_OFF;
    bool start = link_start_gate_check(&gate, w->start, w->mode);
    ack = gate.last;  // the number last acted on, the one recorded after a reset included
    if (start) {
      if (no_starts || state != 1) {
        start_result = LINK_BUSY;
      } else if (!off_wanted) {
        start_result = LINK_OK;
        state = w->mode == LINK_MODE_PRIME ? 7 : 2;
        phase = 0;
        fault = 0;
      }
    }
    phase++;
    if (state == 2 && phase > 3) {
      state = w->mode == LINK_MODE_FAN ? 4 : 3;
      phase = 0;
    }
    if ((state == 3 || state == 4) && !off_wanted) {
      uint8_t want = w->mode == LINK_MODE_FAN ? 4 : 3;  // heat <-> fan while running
      if (want != state && switch_by_stop) {
        pending = w->mode;
        state = 6;
        phase = 0;
      } else {
        state = want;
      }
    }
    if (state == 6 && !off_wanted && pending != 0 && (w->mode == LINK_MODE_HEAT || w->mode == LINK_MODE_FAN))
      pending = w->mode;  // changed again during the stop: the latest wins
    if (off_wanted && (state == 2 || state == 3 || state == 4 || state == 7)) {
      state = 6;
      phase = 0;
    }
    if (state == 6 && phase > 3) {
      state = pending != 0 && !off_wanted ? 2 : 1;
      phase = 0;
      pending = 0;
    }
    o->state = state;
    o->fault = fault;
    o->start_ack = ack;
    o->start_result = start_result;
    o->supply = 1240;
    o->fan_rpm = state == 1 ? 0 : 2800;
    o->power = w->power;
    o->flags = uint8_t((no_starts ? LINK_STATUS_NO_STARTS : 0) | (fault != 0 ? LINK_STATUS_FAULT_LATCHED : 0));
    for (const P &p : params)
      if (p.is_staged)
        o->flags |= LINK_STATUS_PARAMS_STAGED;
  }
  // Time passes on the MCU without polls.
  void tick(uint32_t t) {
    now = t;
    if (link_deadman_expired(&deadman, now, burning())) {
      state = 6;  // burn-off and cooldown on its own
      phase = 0;
      deadman_stops++;
      link_deadman_feed(&deadman, now);
    }
  }
  void flame_out() {
    state = 1;
    fault = 8;
  }
  // An MCU reset: everything forgotten, never resumes.
  void reset() {
    link_start_gate_init(&gate);
    link_deadman_init(&deadman);
    state = 1;
    ack = 0;
    client_major = 0;  // forgotten: no POLL until the next HELLO
    resets++;
  }
};

static void s_info(void *c, link_info *o) {
  SimMcu *m = static_cast<SimMcu *>(c);
  o->kind = LINK_KIND_STM32;
  o->capabilities = LINK_CAP_FAN_ONLY | LINK_CAP_PRIME | LINK_CAP_PRIME_STOP | LINK_CAP_PARAMETERS | LINK_CAP_GLOW_VI |
                    LINK_CAP_FAN_V | (m->run_switch ? LINK_CAP_RUN_SWITCH : 0);
  o->param_count = uint8_t(m->params.size());
  strcpy(o->text, "sim 1.0");
}
static void s_poll(void *c, const link_desired *w, link_status *o) { static_cast<SimMcu *>(c)->poll(w, o); }
static void s_detail(void *c, link_detail *o) {
  (void) c;
  o->fan_v = 390;
  o->glow_v = 820;
  o->glow_a = 830;
}
static int s_describe(void *c, uint8_t i, link_param_desc *o) {
  SimMcu *m = static_cast<SimMcu *>(c);
  if (i >= m->params.size())
    return LINK_OUT_OF_RANGE;
  o->index = i;
  o->count = uint8_t(m->params.size());
  o->id = m->params[i].id;
  o->unit = 1;
  o->min = 0;
  o->max = 200;
  o->step = 1;
  o->group = i == 0 ? 4 : 2;  // start-up, fuel pump
  o->level = i == 0 ? 1 : 7;  // advanced, then a level this client does not know
  strcpy(o->name, m->params[i].name);
  return LINK_OK;
}
static int s_help(void *c, uint16_t id, const char **text) {
  SimMcu *m = static_cast<SimMcu *>(c);
  m->help_asks++;
  if (id == 7)
    *text = m->help7.c_str();
  else if (id == 0x80)
    *text = "";
  else
    return LINK_OUT_OF_RANGE;
  return LINK_OK;
}
static int s_get(void *c, uint16_t id, link_param_value *o) {
  for (auto &p : static_cast<SimMcu *>(c)->params)
    if (p.id == id) {
      o->id = id;
      o->flags = uint8_t(LINK_PARAM_KNOWN | (p.is_staged ? LINK_PARAM_STAGED : 0));
      o->value = p.value;
      o->staged = p.staged;
      return LINK_OK;
    }
  return LINK_OUT_OF_RANGE;
}
static int s_set(void *c, uint16_t id, int32_t v) {
  for (auto &p : static_cast<SimMcu *>(c)->params)
    if (p.id == id) {
      p.staged = v;
      p.is_staged = true;
      return LINK_OK;
    }
  return LINK_OUT_OF_RANGE;
}
static int s_apply(void *c, uint8_t) {
  for (auto &p : static_cast<SimMcu *>(c)->params)
    if (p.is_staged) {
      p.value = p.staged;
      p.is_staged = false;
    }
  return LINK_OK;
}
static int s_defaults(void *c) {
  static_cast<SimMcu *>(c)->params[0].value = 60;
  return LINK_OK;
}
static int s_fault_reset(void *c) {
  static_cast<SimMcu *>(c)->fault = 0;
  return LINK_OK;
}

// The wire: the client's bytes go through a request parser to the server, its replies come back.
struct Wire : fk::LinkTransport {
  SimMcu *mcu;
  link_mcu ops{};
  link_parser rx;
  std::deque<uint8_t> to_client;
  int corrupt_next = 0;
  explicit Wire(SimMcu *m) : mcu(m) {
    link_parser_init(&rx, LINK_REQUEST);
    ops.ctx = m;
    ops.info = s_info;
    ops.poll = s_poll;
    ops.detail = s_detail;
    ops.param_describe = s_describe;
    ops.param_get = s_get;
    ops.param_help = s_help;
    ops.param_set = s_set;
    ops.param_apply = s_apply;
    ops.param_defaults = s_defaults;
    ops.fault_reset = s_fault_reset;
    ops.client_major = &m->client_major;
  }
  void send(const uint8_t *data, size_t len) override {
    if (mcu->silent)
      return;
    for (size_t i = 0; i < len; i++) {
      if (!link_parser_feed(&rx, data[i]))
        continue;
      uint8_t reply[LINK_FRAME_MAX];
      size_t n = link_server_handle(&ops, rx.buf, reply);
      if (corrupt_next > 0) {
        corrupt_next--;
        reply[n - 1] ^= 0xFF;
      }
      to_client.insert(to_client.end(), reply, reply + n);
    }
  }
  int read() override {
    if (to_client.empty())
      return -1;
    int c = to_client.front();
    to_client.pop_front();
    return c;
  }
};

struct Rig {
  SimMcu mcu;
  Wire wire{&mcu};
  fk::LinkClient client{&wire};
  uint32_t t = 1000;
  // Runs the client (and the MCU's clock) for ms milliseconds in 10 ms steps.
  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i += 10) {
      t += 10;
      mcu.tick(t);
      client.loop(t);
    }
  }
  fk::HeaterStatus st() {
    fk::HeaterStatus s;
    client.read_status(s);
    return s;
  }
};

static void test_startup() {
  Rig r;
  CHECK(!r.client.up());
  CHECK(r.st().control == fk::ControlStatus::NO_REPLY);  // nothing heard yet
  r.run(50);
  CHECK(r.client.up());
  CHECK(std::strcmp(r.client.firmware_text(), "sim 1.0") == 0);
  r.run(2000);
  CHECK(r.mcu.polls >= 15);  // about 10 a second
  CHECK_EQ(r.mcu.last.flags, 0);
  CHECK(r.st().control == fk::ControlStatus::IDLE);
  fk::HeaterStatus s = r.st();
  CHECK(s.answering);
  CHECK(s.state == HeaterState::OFF);
  CHECK(s.supply_v == 12.4f);
  CHECK(s.fan_v == 3.9f);
  CHECK(s.glow_a == 8.3f);
  CHECK(std::isnan(s.co_ppm));  // not reported by this MCU
  // The parameter table came over the link.
  CHECK(r.client.capabilities() & fk::cap::PARAMETERS);
  CHECK(r.client.capabilities() & fk::cap::FAN_ONLY);
  CHECK_EQ(r.client.param_count(), 2);
  fk::ParamInfo pi;
  CHECK(r.client.param_info(1, pi) && pi.id == 0x80 && std::strcmp(pi.name, "Pump min") == 0);
  CHECK(pi.group == fk::ParamGroup::FUEL_PUMP);
  CHECK(pi.level == fk::ParamLevel::EXPERT);  // an unknown level shows as expert
  CHECK(r.client.param_info(0, pi) && pi.group == fk::ParamGroup::START_UP && pi.level == fk::ParamLevel::ADVANCED);
  fk::ParamValue pv;
  CHECK(r.client.param_get(7, pv) && pv.known && pv.value == 60);
  // The descriptions came too, in pieces, once.
  CHECK(r.client.param_help(7) == r.mcu.help7);
  CHECK(std::string(r.client.param_help(0x80)).empty());
  CHECK(std::string(r.client.param_help(99)).empty());
  CHECK_EQ(r.mcu.help_asks, 3 + 1);
  r.run(5000);
  CHECK_EQ(r.mcu.help_asks, 4);
}

static void test_start_rule() {
  Rig r;
  r.run(500);
  CHECK(r.client.set_power_demand(40).accepted());
  CHECK(r.client.set_mode(Mode::HEAT).accepted());
  // Asked, not reported yet: the heater still says off, the controller is starting.
  CHECK(r.st().state == HeaterState::OFF);
  CHECK(r.st().control == fk::ControlStatus::STARTING);
  r.run(1000);
  CHECK(r.mcu.state == 3);
  CHECK(r.st().state == HeaterState::HEATING);
  CHECK(r.st().control == fk::ControlStatus::RUNNING);
  CHECK_EQ(r.mcu.last.power, 40);
  // Heat -> fan: no new start.
  uint8_t start = r.mcu.last.start;
  CHECK(r.client.set_mode(Mode::FAN).accepted());
  r.run(300);
  CHECK(r.mcu.state == 4);
  CHECK_EQ(r.mcu.last.start, start);
  CHECK(r.client.set_mode(Mode::PRIME).result == Result::NOT_OFF);

  // The heater stops by itself: it stays off, and the client follows.
  r.mcu.flame_out();
  r.run(500);
  CHECK(r.mcu.state == 1);
  CHECK(r.st().mode == Mode::OFF);
  CHECK(r.st().fault == fk::Fault::FLAME_OUT);
  // A new start by the user works.
  CHECK(r.client.set_mode(Mode::HEAT).accepted());
  r.run(600);
  CHECK(r.mcu.state == 3);

  // Off, then a start during the cooldown is refused.
  CHECK(r.client.set_mode(Mode::OFF).accepted());
  r.run(150);
  CHECK(r.mcu.state == 6);
  CHECK(r.client.set_mode(Mode::HEAT).result == Result::NOT_OFF);
  r.run(1000);
  CHECK(r.mcu.state == 1);
}

static void test_resets() {
  // The ESP32 restarts while the MCU keeps its last number: the next start is still a new one.
  Rig r;
  r.run(300);
  r.client.set_mode(Mode::HEAT);
  r.run(600);
  r.client.set_mode(Mode::OFF);
  r.run(1500);
  CHECK(r.mcu.state == 1);
  {
    Wire w2(&r.mcu);
    fk::LinkClient fresh(&w2);  // a restarted ESP32 on the same MCU
    uint32_t t = r.t;
    for (int i = 0; i < 50; i++) {
      t += 10;
      r.mcu.tick(t);
      fresh.loop(t);
    }
    CHECK(r.mcu.state == 1);  // nothing started by itself
    CHECK(fresh.set_mode(Mode::HEAT).accepted());
    for (int i = 0; i < 60; i++) {
      t += 10;
      r.mcu.tick(t);
      fresh.loop(t);
    }
    CHECK(r.mcu.state == 3);
    CHECK_EQ(r.mcu.ack, r.mcu.last.start);  // the restarted ESP32's own new number
    fresh.set_mode(Mode::OFF);
    for (int i = 0; i < 150; i++) {
      t += 10;
      r.mcu.tick(t);
      fresh.loop(t);
    }
    r.t = t;
  }

  // The MCU resets mid-run: it never resumes, and the client sees it off.
  Rig q;
  q.run(300);
  q.client.set_mode(Mode::HEAT);
  q.run(600);
  CHECK(q.mcu.state == 3);
  q.mcu.reset();
  q.run(1000);
  CHECK(q.mcu.state == 1);
  CHECK(q.st().mode == Mode::OFF);
}

static void test_lost_link() {
  Rig r;
  r.run(300);
  r.client.set_mode(Mode::HEAT);
  r.run(600);
  CHECK(r.mcu.state == 3);
  // A corrupted reply is dropped and the next poll repeats everything.
  r.wire.corrupt_next = 1;
  r.run(300);
  CHECK(r.client.up());
  CHECK(r.client.frames_bad() >= 1);
  // The MCU goes silent: after three missed replies the heater does not answer.
  r.mcu.silent = true;
  r.run(400);
  CHECK(!r.client.up());
  fk::HeaterStatus s = r.st();
  CHECK(!s.answering);
  CHECK(s.control == fk::ControlStatus::NO_REPLY);
  CHECK(s.state == HeaterState::UNKNOWN);
  CHECK(r.client.set_mode(Mode::FAN).result == Result::NOT_OFF || !r.client.up());
  // ... and the MCU's dead-man burns off on its own after 2 s.
  r.run(2000);
  CHECK_EQ(r.mcu.deadman_stops, 1);
  CHECK(r.mcu.state == 6 || r.mcu.state == 1);
  // Back: HELLO again, and the stopped heater is not started again.
  r.mcu.silent = false;
  r.run(1500);
  CHECK(r.client.up());
  CHECK(r.mcu.state == 1);
  CHECK(r.st().mode == Mode::OFF);
}

// The MCU restarts and refuses POLL until it hears HELLO again: the client says HELLO at once
//, well before its 30 s refresh.
static void test_hello_after_mcu_restart() {
  Rig r;
  r.run(300);
  CHECK(r.client.up());
  int polls = r.mcu.polls;
  r.mcu.reset();
  r.run(1000);
  CHECK(r.client.up());
  CHECK(r.mcu.polls > polls + 5);
  CHECK(r.st().answering);
}

static void test_refusals() {
  Rig r;
  r.run(300);
  r.mcu.no_starts = true;
  r.run(200);
  CHECK(r.client.set_mode(Mode::HEAT).result == Result::BUSY);
  r.mcu.no_starts = false;
  CHECK(r.client.set_heat_mode(fk::HeatMode::TEMPERATURE).result == Result::NOT_SUPPORTED);
  // An MCU that cannot switch heat <-> fan during a run: off first.
  Rig b;
  b.mcu.run_switch = false;
  b.run(300);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());
  b.run(600);
  CHECK(b.client.set_mode(Mode::FAN).result == Result::NOT_OFF);
  CHECK(b.client.set_mode(Mode::HEAT).accepted());  // unchanged mode: fine
  // Silent from the start: refused with no reply.
  Rig q;
  q.mcu.silent = true;
  q.run(300);
  CHECK(q.client.set_mode(Mode::HEAT).result == Result::NO_REPLY);
  CHECK_EQ(q.client.capabilities(), 0);
}

// An MCU that switches heat <-> fan by stopping and starting again: the run
// and its start number carry on through the stop, and off during the stop ends it.
static void test_switch_by_stop() {
  Rig r;
  r.mcu.switch_by_stop = true;
  r.run(300);
  CHECK(r.client.set_mode(Mode::HEAT).accepted());
  r.run(600);
  CHECK_EQ(r.mcu.state, 3);
  uint8_t start = r.mcu.ack;
  CHECK(r.client.set_mode(Mode::FAN).accepted());
  bool left_run = false, cooled = false;
  for (int i = 0; i < 30; i++) {
    r.run(100);
    left_run |= r.st().mode == Mode::OFF;
    cooled |= r.mcu.state == 6;
  }
  CHECK(!left_run);
  CHECK(cooled);
  CHECK_EQ(r.mcu.state, 4);
  CHECK_EQ(r.mcu.ack, start);
  CHECK(r.st().mode == Mode::FAN);
  // Back to heat, then off before the stop is over: no restart.
  CHECK(r.client.set_mode(Mode::HEAT).accepted());
  r.run(200);
  CHECK_EQ(r.mcu.state, 6);
  CHECK(r.client.set_mode(Mode::OFF).accepted());
  r.run(1500);
  CHECK_EQ(r.mcu.state, 1);
  CHECK(r.st().mode == Mode::OFF);
  CHECK_EQ(r.mcu.ack, start);
}

static void test_params() {
  Rig r;
  int changed = 0, applied = 0;
  struct L : fk::HeaterListener {
    int *changed, *applied;
    int fault_resets = 0, defaults = 0;
    void on_heater_event(fk::Event e, Result res, uint16_t, const char *) override {
      if (e == fk::Event::FAULT_RESET && res == Result::OK)
        fault_resets++;
      if (e == fk::Event::PARAMS_DEFAULTS && res == Result::OK)
        defaults++;
      if (e == fk::Event::PARAM_CHANGED && res == Result::OK)
        (*changed)++;
      if (e == fk::Event::PARAMS_APPLIED && res == Result::OK)
        (*applied)++;
    }
  } l;
  l.changed = &changed;
  l.applied = &applied;
  r.client.set_listener(&l);
  r.run(1000);
  CHECK(r.client.param_set(7, 999).result == Result::OUT_OF_RANGE);
  CHECK(r.client.param_set(99, 1).result == Result::OUT_OF_RANGE);
  CHECK(r.client.param_set(7, 75).accepted());
  fk::ParamValue v;
  CHECK(r.client.param_get(7, v) && v.staged && v.staged_value == 75 && v.value == 60);
  CHECK_EQ(r.st().params_pending, 1);
  r.run(300);
  CHECK(r.mcu.params[0].is_staged);
  CHECK(r.client.param_apply(false).accepted());
  r.run(1500);
  CHECK_EQ(r.mcu.params[0].value, 75);
  CHECK_EQ(applied, 1);
  CHECK(r.client.param_get(7, v) && !v.staged && v.value == 75);
  CHECK(changed >= 1);
  CHECK_EQ(r.st().params_pending, 0);
  // Factory reset of the MCU's parameters, and a fault reset.
  // Each answered later by an event with the MCU's own result.
  CHECK(r.client.reset_parameters().accepted());
  CHECK(r.client.reset_fault().result == Result::REFUSED);  // nothing latched
  r.mcu.fault = 4;
  r.run(200);
  CHECK(r.client.reset_fault().accepted());
  r.run(1000);
  CHECK_EQ(r.mcu.params[0].value, 60);
  CHECK_EQ(r.mcu.fault, 0);
  CHECK_EQ(l.fault_resets, 1);
  CHECK_EQ(l.defaults, 1);
  // Trims ride on every poll.
  r.client.set_trims(-40, 300);
  r.run(200);
  CHECK_EQ(r.mcu.last.fuel_trim, -30);  // held to the protocol's range
  CHECK_EQ(r.mcu.last.fan_trim, 300);
}

static void test_controller_on_top() {
  // The shared controller drives the link like any backend.
  Rig r;
  fk::Controller c(&r.client);
  r.run(300);
  c.tick(r.t, true);
  CHECK(c.set_power(55, fk::Source::PHONE).accepted());
  CHECK(c.set_mode(Mode::HEAT, fk::Source::PHONE).accepted());
  for (int i = 0; i < 80; i++) {
    r.run(10);
    c.tick(r.t, true);
  }
  CHECK(c.status().state == HeaterState::HEATING);
  CHECK_EQ(int(c.status().run_source), int(fk::Source::PHONE));
  CHECK_EQ(r.mcu.last.power, 55);
}

int main() {
  test_startup();
  test_start_rule();
  test_resets();
  test_lost_link();
  test_hello_after_mcu_restart();
  test_refusals();
  test_switch_by_stop();
  test_params();
  test_controller_on_top();
  return report("link_client");
}
