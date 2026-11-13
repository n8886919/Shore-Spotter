#pragma once

#include <stdint.h>

namespace t096_maintenance {
// Last failure in this boot, deliberately retained after recovery. Sources are
// static string literals. An absent code denotes boolean/validation failure;
// an explicitly recorded zero remains a raw zero (e.g. missing TX IRQ flags).
struct Fault {
  const char *source = nullptr;
  int32_t code = 0;
  bool hasCode = false;
  void record(const char *where) { source = where; code = 0; hasCode = false; }
  void record(const char *where, int32_t value) { source = where; code = value; hasCode = true; }
};
}
