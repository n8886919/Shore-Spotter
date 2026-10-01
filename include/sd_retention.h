#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace sd_retention {
inline bool name(const char *s, uint64_t &sequence, bool &legacy) {
  const size_t n = strlen(s);
  const size_t digits = n == 28 ? 16 : n == 20 ? 8 : 0;
  if (!digits || s[digits] != '-' || strcmp(s + digits + 5, ".ndjson")) return false;
  sequence = 0; legacy = digits == 8;
  for (size_t i = 0; i < digits; ++i) {
    const int v = s[i] >= '0' && s[i] <= '9' ? s[i] - '0' :
                  s[i] >= 'a' && s[i] <= 'f' ? s[i] - 'a' + 10 : -1;
    if (v < 0) return false;
    sequence = (sequence << 4) | unsigned(v);
  }
  for (size_t i = digits + 1; i < digits + 5; ++i)
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}
inline uint64_t reserve(uint64_t total) {
  constexpr uint64_t floor = 64ULL * 1024 * 1024;
  return total / 20 > floor ? total / 20 : floor;
}
}
