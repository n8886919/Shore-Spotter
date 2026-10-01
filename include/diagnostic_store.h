#pragma once
#include "diagnostic_store_codec.h"
#if defined(ARDUINO)
#include <Arduino.h>

namespace diagnostic_store {
constexpr size_t kQueueRecords = 32;
constexpr uint32_t kFlushMs = 1000;
// Only FIELD_DIAGNOSTIC builds touch the existing unused SPIFFS partition.
// Producers copy into a zero-wait queue. SD, NVS, OTA and coredump are untouched.
void begin(uint32_t bootId);
bool submit(uint16_t kind, const void *payload, size_t length, uint32_t ms);
String statusJson();
// Dispatch complete lines here from the shared USB command parser. Never reads
// Serial itself. True means accepted/queued (including bad syntax). False means
// not accepted: the router retains a recognized DIAG line and retries later.
bool command(const char *line);
bool transferActive();
bool healthy();
const char *stateName();
uint32_t dropped();
uint32_t writeMaxUs();
} // namespace diagnostic_store
#endif
