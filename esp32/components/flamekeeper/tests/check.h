// The host tests' checks: CHECK and CHECK_EQ count and print failures, report() sums them up.
#pragma once

#include <cstdio>

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    g_checks++;                                                          \
    if (!(cond)) {                                                       \
      g_failures++;                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
    }                                                                    \
  } while (0)
#define CHECK_EQ(a, b)                                                                                    \
  do {                                                                                                    \
    g_checks++;                                                                                           \
    long long va_ = (long long) (a), vb_ = (long long) (b);                                               \
    if (va_ != vb_) {                                                                                     \
      g_failures++;                                                                                       \
      std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_);         \
    }                                                                                                     \
  } while (0)
static int report(const char *name) {
  std::printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
