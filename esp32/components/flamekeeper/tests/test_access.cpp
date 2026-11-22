// Wi-Fi key tables, the web password and its lockout (PROTOCOL.md).
#include <cstdint>
#include <cstring>
#include <vector>

#include "../access.h"
#include "fake_heater.h"

static uint8_t g_byte = 0;
static void counter_rng(uint8_t *out, size_t len) {
  for (size_t i = 0; i < len; i++)
    out[i] = ++g_byte;
}

static void test_app_keys() {
  fk::Access a(counter_rng);
  const uint8_t phone1[6] = {1, 1, 1, 1, 1, 1}, phone2[6] = {2, 2, 2, 2, 2, 2};
  const fk::WifiKey *k1 = a.issue_app_key(phone1);
  CHECK(k1 != nullptr && k1->kind == fk::KeyKind::APP);
  uint8_t id1[16];
  std::memcpy(id1, k1->id, 16);
  CHECK(a.dirty());
  // The same phone again: its key is replaced, not added.
  const fk::WifiKey *k1b = a.issue_app_key(phone1);
  CHECK(std::memcmp(k1b->id, id1, 16) != 0);
  CHECK(a.find_key(id1) == nullptr);
  CHECK_EQ(a.count(fk::KeyKind::APP), 1);
  a.issue_app_key(phone2);
  CHECK_EQ(a.count(fk::KeyKind::APP), 2);
  // Full table: refused (app keys are never dropped).
  for (uint8_t i = 3; i < 10; i++) {
    const uint8_t phone[6] = {i, i, i, i, i, i};
    const fk::WifiKey *k = a.issue_app_key(phone);
    CHECK((i <= 8) == (k != nullptr));
  }
  CHECK_EQ(a.count(fk::KeyKind::APP), 8);
  CHECK_EQ(a.revoke_owner(phone2), 1);
  CHECK_EQ(a.count(fk::KeyKind::APP), 7);
}

static void test_browser_keys_lru() {
  fk::Access a(counter_rng);
  std::vector<std::vector<uint8_t>> ids;
  for (int i = 0; i < 8; i++) {
    const fk::WifiKey *k = a.issue_browser_key(true);
    ids.emplace_back(k->id, k->id + 16);
  }
  CHECK_EQ(a.count(fk::KeyKind::BROWSER), 8);
  // Browser 0 signs in again; browser 1 is now the least recently used and goes first.
  a.clean();
  CHECK(a.find_key(ids[0].data()) != nullptr);
  CHECK(!a.dirty());  // looking a key up writes nothing
  a.mark_used(ids[0].data());
  CHECK(a.dirty());
  a.issue_browser_key(true);
  CHECK_EQ(a.count(fk::KeyKind::BROWSER), 8);
  CHECK(a.find_key(ids[0].data()) != nullptr);
  CHECK(a.find_key(ids[1].data()) == nullptr);
  CHECK(a.find_key(ids[2].data()) != nullptr);
  // A new web password signs every browser out; app keys stay.
  const uint8_t phone[6] = {9, 9, 9, 9, 9, 9};
  a.issue_app_key(phone);
  const uint8_t pw[] = "correct horse";
  CHECK(a.change_web_password(nullptr, 0, pw, 13).accepted());
  CHECK_EQ(a.count(fk::KeyKind::BROWSER), 0);
  CHECK_EQ(a.count(fk::KeyKind::APP), 1);
}

static void test_web_password() {
  fk::Access a(counter_rng);
  CHECK(!a.web_password_set());
  const uint8_t pw[] = "correct horse", other[] = "battery staple";
  CHECK(a.change_web_password(nullptr, 0, pw, 13).accepted());
  CHECK(a.web_password_set() && a.web_password_dirty());
  // Changing it needs the current one.
  CHECK(a.change_web_password(other, 14, other, 14).result == fk::Result::WRONG_PASSWORD);
  CHECK(a.change_web_password(pw, 12, other, 14).result == fk::Result::WRONG_PASSWORD);
  CHECK(a.change_web_password(pw, 13, other, 7).result == fk::Result::OUT_OF_RANGE);
  CHECK(a.change_web_password(pw, 13, other, 14).accepted());
  CHECK(std::memcmp(a.web_password(), other, 14) == 0);
  // Empty turns web sign-in off.
  CHECK(a.change_web_password(other, 14, nullptr, 0).accepted());
  CHECK(!a.web_password_set());
}

static void test_lockout() {
  fk::PasswordLockout l;
  uint32_t t = 1000;
  for (int i = 0; i < 4; i++) {
    l.failed(t);
    CHECK_EQ(l.seconds_left(t), 0);
  }
  l.failed(t);  // fifth: a minute
  CHECK_EQ(l.seconds_left(t), 60);
  CHECK_EQ(l.seconds_left(t + 59001), 1);
  CHECK_EQ(l.seconds_left(t + 60000), 0);
  t += 60000;
  l.failed(t);  // sixth: two minutes
  CHECK_EQ(l.seconds_left(t), 120);
  for (int i = 0; i < 10; i++)
    l.failed(t);
  CHECK_EQ(l.seconds_left(t), 3600);  // capped at an hour
  l.succeeded();
  CHECK_EQ(l.seconds_left(t), 0);
  l.failed(t);
  CHECK_EQ(l.seconds_left(t), 0);
  // millis() wrapping during a wait
  fk::PasswordLockout w;
  uint32_t near = UINT32_MAX - 10000;
  for (int i = 0; i < 5; i++)
    w.failed(near);
  CHECK_EQ(w.seconds_left(near + 30000), 30);
}

static void test_persistence() {
  fk::Access a(counter_rng);
  const uint8_t phone[6] = {7, 7, 7, 7, 7, 7};
  const fk::WifiKey *app = a.issue_app_key(phone);
  uint8_t app_id[16], app_secret[32];
  std::memcpy(app_id, app->id, 16);
  std::memcpy(app_secret, app->secret, 32);
  const fk::WifiKey *kept = a.issue_browser_key(true);
  uint8_t kept_id[16];
  std::memcpy(kept_id, kept->id, 16);
  const fk::WifiKey *tab = a.issue_browser_key(false);  // this tab only: not saved
  uint8_t tab_id[16];
  std::memcpy(tab_id, tab->id, 16);

  uint8_t buf[fk::Access::SERIALIZED_MAX];
  size_t n = a.serialize(buf, sizeof(buf));
  CHECK_EQ(n, 2 + 2 * fk::Access::ENTRY_SIZE);
  fk::Access b(counter_rng);
  CHECK(b.deserialize(buf, n));
  const fk::WifiKey *back = b.find_key(app_id);
  CHECK(back != nullptr && back->kind == fk::KeyKind::APP);
  CHECK(back != nullptr && std::memcmp(back->secret, app_secret, 32) == 0);
  CHECK(back != nullptr && std::memcmp(back->owner, phone, 6) == 0);
  CHECK(b.find_key(kept_id) != nullptr);
  CHECK(b.find_key(tab_id) == nullptr);
  // Damaged data is refused.
  CHECK(!b.deserialize(buf, n - 1));
  buf[0] = 3;
  CHECK(!b.deserialize(buf, n));
  CHECK(!b.deserialize(buf, 1));
  // Factory reset
  b.clear();
  CHECK_EQ(b.count(fk::KeyKind::APP) + b.count(fk::KeyKind::BROWSER), 0);
}

// Phone keys keep their slots over a restart: the slot is the phone's place in the relay roster and
// what its push preferences are bound to.
static void test_slots_kept() {
  fk::Access a(counter_rng);
  const uint8_t p0[6] = {1, 1, 1, 1, 1, 1}, p1[6] = {2, 2, 2, 2, 2, 2}, p2[6] = {3, 3, 3, 3, 3, 3};
  const fk::WifiKey *k0 = a.issue_app_key(p0);
  a.issue_app_key(p1);
  a.issue_app_key(p2);
  const fk::WifiKey *kb = a.issue_browser_key(true);
  uint8_t b_id[16];
  std::memcpy(b_id, kb->id, 16);
  uint8_t id1[16], id2[16];
  std::memcpy(id1, a.app_key(1)->id, 16);
  std::memcpy(id2, a.app_key(2)->id, 16);
  CHECK(a.revoke(k0->id));  // the first phone goes: slot 0 is free
  uint8_t buf[fk::Access::SERIALIZED_MAX];
  size_t n = a.serialize(buf, sizeof(buf));
  fk::Access b(counter_rng);
  CHECK(b.deserialize(buf, n));
  CHECK(b.app_key(0) == nullptr);
  CHECK(b.app_key(1) != nullptr && std::memcmp(b.app_key(1)->id, id1, 16) == 0);
  CHECK(b.app_key(2) != nullptr && std::memcmp(b.app_key(2)->id, id2, 16) == 0);
  CHECK(b.find_key(b_id) != nullptr && b.find_key(b_id)->kind == fk::KeyKind::BROWSER);
  // A new phone takes the free slot 0.
  const uint8_t p3[6] = {4, 4, 4, 4, 4, 4};
  CHECK(b.issue_app_key(p3) == b.app_key(0));
  // Format 1 (older builds, no slot) still loads, packed from the first slot.
  buf[0] = 1;
  for (size_t i = 0; i < buf[1]; i++)
    buf[2 + i * fk::Access::ENTRY_SIZE] &= 0x0F;
  fk::Access c(counter_rng);
  CHECK(c.deserialize(buf, n));
  CHECK(c.app_key(0) != nullptr && std::memcmp(c.app_key(0)->id, id1, 16) == 0);
  CHECK(c.app_key(1) != nullptr && std::memcmp(c.app_key(1)->id, id2, 16) == 0);
  CHECK(c.find_key(b_id) != nullptr);
}

static void test_equal_ct() {
  const uint8_t a[] = {1, 2, 3}, b[] = {1, 2, 4};
  CHECK(fk::equal_ct(a, 3, a, 3));
  CHECK(!fk::equal_ct(a, 3, b, 3));
  CHECK(!fk::equal_ct(a, 3, a, 2));
  CHECK(fk::equal_ct(a, 0, b, 0));
}

int main() {
  test_app_keys();
  test_browser_keys_lru();
  test_web_password();
  test_lockout();
  test_persistence();
  test_slots_kept();
  test_equal_ct();
  return report("access");
}
