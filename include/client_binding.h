#pragma once
#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

namespace client_binding {
constexpr bool validId(uint16_t id) {
  return id != 0 && id != SERVER_ID && id != ID_BROADCAST;
}

inline bool parseId(const char *text, size_t length, uint16_t &id) {
  if (length == 0 || length > 4) return false;
  uint16_t value = 0;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    uint8_t digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return false;
    value = static_cast<uint16_t>((value << 4) | digit);
  }
  if (!validId(value)) return false;
  id = value;
  return true;
}

// An existing empty list means intentionally unbound. For legacy lists with
// multiple entries, deterministically retain only the first valid client.
inline uint16_t fromLegacyList(const char *text, size_t length) {
  size_t start = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i != length && text[i] != ',') continue;
    uint16_t id = 0;
    if (parseId(text + start, i - start, id)) return id;
    start = i + 1;
  }
  return 0;
}
}  // namespace client_binding
