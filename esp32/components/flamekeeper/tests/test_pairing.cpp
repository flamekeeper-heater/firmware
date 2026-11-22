// Pairing window and passkey.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../pairing.h"
#include "fake_heater.h"

static void test_window() {
  fk::PairingWindow w;
  CHECK(!w.is_open(0));
  CHECK(!w.accept(0, 0, 8));
  CHECK_EQ(w.seconds_left(0), 0);
  uint32_t t = 1000;
  w.open(t);
  CHECK(w.is_open(t));
  CHECK_EQ(w.seconds_left(t), 120);
  CHECK_EQ(w.seconds_left(t + 500), 120);
  CHECK_EQ(w.seconds_left(t + 1000), 119);
  CHECK(w.accept(t + 119999, 7, 8));
  CHECK(!w.accept(t + 1000, 8, 8));  // table full
  CHECK(!w.is_open(t + 120000));
  CHECK(!w.accept(t + 120000, 0, 8));
  // One phone per window.
  w.open(t);
  w.paired();
  CHECK(!w.accept(t + 1, 0, 8));
  // Re-opening restarts the two minutes; millis() wrapping is fine.
  t = UINT32_MAX - 1000;
  w.open(t);
  CHECK(w.is_open(t + 60000));
  CHECK_EQ(w.seconds_left(t + 60000), 60);
  CHECK(!w.is_open(t + 121000));
  w.open(t, 0);
  CHECK(!w.is_open(t));
  w.open(t);
  w.close();
  CHECK(!w.is_open(t));
}

static void test_passkey() {
  char s[7];
  fk::format_passkey(42, s);
  CHECK(std::strcmp(s, "000042") == 0);
  fk::format_passkey(999999, s);
  CHECK(std::strcmp(s, "999999") == 0);
  CHECK(fk::passkey_valid(0));
  CHECK(fk::passkey_valid(999999));
  CHECK(!fk::passkey_valid(1000000));

  // The top of the 32-bit range is rejected (it would favour low passkeys).
  std::vector<uint32_t> seq = {UINT32_MAX, 4294000000u, 4293999999u};
  size_t i = 0;
  uint32_t k = fk::passkey_from_random([&] { return seq[i++]; });
  CHECK_EQ(i, 3);
  CHECK_EQ(k, 999999);
  // Spread: a simple LCG gives every leading digit.
  uint32_t x = 12345;
  int digit[10] = {};
  for (int n = 0; n < 10000; n++) {
    uint32_t key = fk::passkey_from_random([&] { return x = x * 1664525u + 1013904223u; });
    CHECK(fk::passkey_valid(key));
    digit[key / 100000]++;
  }
  for (int d = 0; d < 10; d++)
    CHECK(digit[d] > 800 && digit[d] < 1200);
}

static void test_failed_pairings() {
  fk::PairingWindow w;
  w.open(0);
  w.failed();
  w.failed();
  CHECK(w.is_open(1));
  w.failed();  // third wrong PIN
  CHECK(!w.is_open(1));
  w.open(10);  // a new window starts the count again
  w.failed();
  w.failed();
  CHECK(w.is_open(11));
}

// Failed pairings add up across windows; after five, a power-up opens none.
static void test_failures_across_windows() {
  fk::PairingWindow w;
  CHECK(w.power_up_allowed());
  for (int window = 0; window < 2; window++) {
    w.open(1000);
    w.failed();
    w.failed();
    w.failed();
    CHECK(!w.is_open(1001));
  }
  CHECK_EQ(w.failures_since_paired(), 6);
  CHECK(!w.power_up_allowed());
  CHECK(w.failures_dirty());
  w.failures_saved();
  // Kept over a restart.
  fk::PairingWindow after;
  after.load_failures(6);
  CHECK(!after.power_up_allowed());
  // A deliberate window still opens; a good pairing clears the count.
  after.open(1000);
  CHECK(after.is_open(1000));
  after.paired();
  CHECK_EQ(after.failures_since_paired(), 0);
  CHECK(after.power_up_allowed());
  CHECK(after.failures_dirty());
}

static void test_label_pin() {
  // Rejection: the first counter's value is in the top of the range, the second is not.
  std::vector<uint8_t> counters;
  fk::PinHmac hmac = [&](const uint8_t *msg, size_t len, uint8_t out[32]) {
    CHECK_EQ(len, 8);
    CHECK(std::memcmp(msg, "fk1 pin", 7) == 0);
    counters.push_back(msg[7]);
    std::memset(out, 0, 32);
    if (msg[7] == 0) {
      out[0] = out[1] = out[2] = out[3] = 0xFF;  // 4294967295: rejected
    } else {
      uint32_t v = 4293999999u;  // the last accepted value: 999999
      out[0] = uint8_t(v >> 24), out[1] = uint8_t(v >> 16), out[2] = uint8_t(v >> 8), out[3] = uint8_t(v);
    }
    return true;
  };
  uint32_t pin = 0;
  CHECK(fk::label_pin(hmac, &pin));
  CHECK_EQ(pin, 999999);
  CHECK(counters.size() == 2 && counters[0] == 0 && counters[1] == 1);
  // A failing HMAC peripheral gives no PIN.
  CHECK(!fk::label_pin([](const uint8_t *, size_t, uint8_t *) { return false; }, &pin));
  // Deterministic: the same key gives the same PIN.
  uint32_t a = 0, b = 0;
  fk::PinHmac h2 = [](const uint8_t *msg, size_t, uint8_t out[32]) {
    for (int i = 0; i < 32; i++)
      out[i] = uint8_t(msg[7] * 31 + i * 7 + 5);
    return true;
  };
  CHECK(fk::label_pin(h2, &a) && fk::label_pin(h2, &b) && a == b && fk::passkey_valid(a));
}

int main() {
  test_window();
  test_failed_pairings();
  test_failures_across_windows();
  test_label_pin();
  test_passkey();
  return report("pairing");
}
