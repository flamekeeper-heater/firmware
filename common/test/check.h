/* The host tests' checks (stm32/tests, common/link/tests, common/co/tests), C99 and C++: CHECK and
 * CHECK_EQ count and print failures, check_report() prints the totals and gives the exit status.
 *
 * A test may define CHECK_ON_FAIL() before including this, to say more after a failed check. */
#ifndef FLAMEKEEPER_CHECK_H
#define FLAMEKEEPER_CHECK_H

#include <stdio.h>

static int check_count = 0, check_failures = 0;

#ifndef CHECK_ON_FAIL
#define CHECK_ON_FAIL() ((void) 0)
#endif

#define CHECK(c)                                          \
  do {                                                    \
    check_count++;                                        \
    if (!(c)) {                                           \
      check_failures++;                                   \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      CHECK_ON_FAIL();                                    \
    }                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    long long va_ = (long long) (a), vb_ = (long long) (b);                                \
    check_count++;                                                                         \
    if (va_ != vb_) {                                                                      \
      check_failures++;                                                                    \
      printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
      CHECK_ON_FAIL();                                                                     \
    }                                                                                      \
  } while (0)

/* "name: N checks, F failures"; 0 if none failed, else 1. */
static int check_report(const char *name) {
  printf("%s: %d checks, %d failures\n", name, check_count, check_failures);
  return check_failures ? 1 : 0;
}

#endif /* FLAMEKEEPER_CHECK_H */
