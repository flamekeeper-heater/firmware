// The board's web page: the app's browser build, gzipped into the firmware image at compile time
// (flamekeeper_net/__init__.py writes the table; PROTOCOL.md). One OTA updates
// the board and its page together.
//
// Plain C++, no ESPHome or ESP-IDF headers (host-tested through tests/test_web_files.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace flamekeeper {
namespace web {

struct File {
  const char *path;  // "/index.html", "/assets/index-CkksxKgm.js"
  const char *type;  // Content-Type
  const char *etag;  // quoted, from the gzipped bytes
  const uint8_t *gz;
  size_t gz_size;
  bool immutable;    // the name carries a content hash: the browser may keep it for good
};

// The file a GET for `uri` asks for, or nullptr. "/" is "/index.html"; a query string is ignored.
inline const File *find(const File *files, size_t n, const char *uri) {
  size_t len = std::strcspn(uri, "?#");
  static const char INDEX[] = "/index.html";
  if (len == 1 && uri[0] == '/') {
    uri = INDEX;
    len = sizeof(INDEX) - 1;
  }
  for (size_t i = 0; i < n; i++)
    if (std::strlen(files[i].path) == len && std::memcmp(files[i].path, uri, len) == 0)
      return &files[i];
  return nullptr;
}

// Whether an If-None-Match header names this ETag (a list, or "*").
inline bool etag_matches(const char *if_none_match, const char *etag) {
  if (if_none_match == nullptr || etag == nullptr)
    return false;
  const size_t n = std::strlen(etag);
  for (const char *p = if_none_match; *p != 0;) {
    while (*p == ' ' || *p == ',')
      p++;
    if (*p == 0)
      break;
    const size_t len = std::strcspn(p, ",");
    size_t end = len;
    while (end > 0 && p[end - 1] == ' ')
      end--;
    if (end == 1 && p[0] == '*')
      return true;
    const char *tag = p;
    size_t tag_len = end;
    if (tag_len > 2 && tag[0] == 'W' && tag[1] == '/') {  // a weak tag compares equal too
      tag += 2;
      tag_len -= 2;
    }
    if (tag_len == n && std::memcmp(tag, etag, n) == 0)
      return true;
    p += len;
  }
  return false;
}

}  // namespace web
}  // namespace flamekeeper
}  // namespace esphome
