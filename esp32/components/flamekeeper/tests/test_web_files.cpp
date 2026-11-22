// The web page's file table: which file a request gets, and when the browser's copy is still good.
#include "../web_files.h"

#include <cstdio>

using namespace esphome::flamekeeper::web;

static int checks = 0, failed = 0;
#define CHECK(c)                                              \
  do {                                                        \
    checks++;                                                 \
    if (!(c)) {                                               \
      failed++;                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    }                                                         \
  } while (0)

static const uint8_t A[] = {1};
static const File FILES[] = {
    {"/index.html", "text/html; charset=utf-8", "\"aa\"", A, 1, false},
    {"/assets/index-CkksxKgm.js", "text/javascript", "\"bb\"", A, 1, true},
    {"/assets/index-CkksxKgm.css", "text/css", "\"cc\"", A, 1, true},
};
static const size_t N = sizeof(FILES) / sizeof(FILES[0]);

static void test_find() {
  CHECK(find(FILES, N, "/") == &FILES[0]);
  CHECK(find(FILES, N, "/index.html") == &FILES[0]);
  CHECK(find(FILES, N, "/?board=1") == &FILES[0]);
  CHECK(find(FILES, N, "/#settings") == &FILES[0]);
  CHECK(find(FILES, N, "/assets/index-CkksxKgm.js") == &FILES[1]);
  CHECK(find(FILES, N, "/assets/index-CkksxKgm.js?v=2") == &FILES[1]);
  CHECK(find(FILES, N, "/assets/index-CkksxKgm.css") == &FILES[2]);
  // Nothing else: no prefixes, no directories, no traversal, no case folding.
  CHECK(find(FILES, N, "/assets/index-CkksxKgm") == nullptr);
  CHECK(find(FILES, N, "/assets/") == nullptr);
  CHECK(find(FILES, N, "/assets/../index.html") == nullptr);
  CHECK(find(FILES, N, "/INDEX.HTML") == nullptr);
  CHECK(find(FILES, N, "") == nullptr);
  CHECK(find(FILES, N, "/api/v1") == nullptr);
  CHECK(find(FILES, 0, "/") == nullptr);
}

static void test_etag() {
  CHECK(etag_matches("\"aa\"", "\"aa\""));
  CHECK(etag_matches("W/\"aa\"", "\"aa\""));
  CHECK(etag_matches("\"zz\", \"aa\"", "\"aa\""));
  CHECK(etag_matches("\"zz\",\"aa\" ", "\"aa\""));
  CHECK(etag_matches("*", "\"aa\""));
  CHECK(!etag_matches("\"zz\"", "\"aa\""));
  CHECK(!etag_matches("\"aa", "\"aa\""));
  CHECK(!etag_matches("\"aaa\"", "\"aa\""));
  CHECK(!etag_matches("", "\"aa\""));
  CHECK(!etag_matches(" , ", "\"aa\""));
  CHECK(!etag_matches(nullptr, "\"aa\""));
}

int main() {
  test_find();
  test_etag();
  std::printf("web_files: %d checks, %d failed\n", checks, failed);
  return failed == 0 ? 0 : 1;
}
