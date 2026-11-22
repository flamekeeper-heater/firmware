// Little-endian byte packing, and the fixed-point encoding with PROTOCOL.md's "unknown" markers
// ("Encoding"). Shared by every codec in this component.
#pragma once

#include <cmath>
#include <cstdint>

namespace esphome {
namespace flamekeeper {

inline void put_u16(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
inline void put_u32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = uint8_t(v >> (8 * i));
}
inline uint16_t get_u16(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
inline uint32_t get_u32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline int32_t get_i32(const uint8_t *p) { return int32_t(get_u32(p)); }

// value x scale, rounded and clamped; NAN is unknown (0x8000).
inline void put_fixed_i16(uint8_t *p, float v, float scale) {
  if (std::isnan(v)) {
    put_u16(p, 0x8000);
    return;
  }
  float x = std::round(v * scale);
  x = x < -32767.0f ? -32767.0f : (x > 32767.0f ? 32767.0f : x);
  put_u16(p, uint16_t(int16_t(x)));
}
// value x scale, rounded and clamped; NAN is unknown (0xFFFF).
inline void put_fixed_u16(uint8_t *p, float v, float scale) {
  if (std::isnan(v)) {
    put_u16(p, 0xFFFF);
    return;
  }
  float x = std::round(v * scale);
  x = x < 0.0f ? 0.0f : (x > 65534.0f ? 65534.0f : x);
  put_u16(p, uint16_t(x));
}
// Rounded and clamped; NAN is unknown (0xFF).
inline uint8_t fixed_u8(float v) {
  if (std::isnan(v))
    return 0xFF;
  float x = std::round(v);
  return uint8_t(x < 0.0f ? 0.0f : (x > 254.0f ? 254.0f : x));
}

}  // namespace flamekeeper
}  // namespace esphome
