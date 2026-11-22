/* The parser after a resync onto a frame it had already buffered: found by the link fuzzer
 * (stm32/tests/fuzz_link.cpp). A header with a long length in the noise makes the parser buffer the
 * real frame and what follows it; when that bogus frame fails, the real one comes out, once, and
 * the frames after it still come out. */
#include <stdio.h>
#include <string.h>

#include "../link.h"

static int checks = 0, failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    checks++;                                                     \
    if (!(c)) {                                                   \
      failures++;                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
    }                                                             \
  } while (0)

static void test_frame_after_bogus_length(void) {
  uint8_t a[LINK_FRAME_MAX], b[LINK_FRAME_MAX], stream[4 * LINK_FRAME_MAX];
  const uint8_t pl[] = {1, 2, 3};
  size_t na = link_build_request(8, LINK_OP_FAULT_RESET, NULL, 0, a);
  size_t nb = link_build_request(9, LINK_OP_PARAM_SET, pl, 3, b);
  size_t k = 0;
  /* The fuzzer's stream: noise with a header claiming 0x35 bytes, then the frame. */
  const uint8_t noise[] = {0x7D, 0x00, 0x08, 0x7A, 0x35, 0xEF, 0x06, 0x8F, 0x21, 0x40, 0x00, 0x9F};
  memcpy(stream, noise, sizeof(noise));
  k += sizeof(noise);
  memcpy(stream + k, a, na);
  k += na;
  while (k < 3 + 0x35 + 2) /* filler until the bogus frame is complete (and fails) */
    stream[k++] = 0x55;
  memcpy(stream + k, b, nb);
  k += nb;

  link_parser p;
  link_parser_init(&p, LINK_REQUEST);
  int got_a = 0, got_b = 0, other = 0;
  for (size_t i = 0; i < k; i++) {
    if (!link_parser_feed(&p, stream[i]))
      continue;
    size_t n = link_frame_len(p.buf);
    if (n == na && memcmp(p.buf, a, na) == 0)
      got_a++;
    else if (n == nb && memcmp(p.buf, b, nb) == 0)
      got_b++;
    else
      other++;
  }
  CHECK(got_a == 1); /* handed out once, not once per byte that followed it */
  CHECK(got_b == 1); /* and the next frame still comes out */
  CHECK(other == 0);
}

int main(void) {
  test_frame_after_bogus_length();
  printf("parser resync: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
