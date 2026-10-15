#pragma once
#include <Arduino.h>

// ESP-IDF task logs are queued, then passed through the normal firmware logger
// on the main loop. The SDK must never write into a framed USB export directly.
namespace sdk_log {
void begin();
void drain(Print &output);
}
