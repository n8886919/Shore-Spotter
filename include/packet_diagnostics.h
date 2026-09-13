#pragma once
#include <stddef.h>
#include <stdint.h>

namespace packet_diagnostics {
enum class Kind : uint8_t { Data, Telemetry, Diagnostic, Length, Format, Binding, Sequence,
                            RadioError, AckError, AckSkipped };
inline const char *name(Kind kind) {
  switch (kind) {
    case Kind::Data: return "data";
    case Kind::Telemetry: return "telemetry";
    case Kind::Diagnostic: return "diagnostic";
    case Kind::Length: return "length";
    case Kind::Format: return "format";
    case Kind::Binding: return "binding";
    case Kind::Sequence: return "sequence";
    case Kind::RadioError: return "radio_error";
    case Kind::AckError: return "ack_error";
    case Kind::AckSkipped: return "ack_skipped";
  }
  return "unknown";
}
struct Event {
  uint32_t id = 0, ms = 0;
  Kind kind = Kind::Data;
  uint16_t clientId = 0, seq = 0, length = 0;
  uint16_t sourceAgeMs = UINT16_MAX;
  int16_t rssiDbm10 = 0, snrQuarterDb = 0, code = 0;
  uint8_t flags = 0;
  // Copy bounded raw radio data to make decoder defects reviewable offline.
  uint8_t raw[17]{};
  uint8_t rawLength = 0;
};

constexpr size_t kMaxEventsPerResponse = 8;
struct Selection {
  size_t start = 0, count = 0;  // indices in the current oldest-first Ring view
  uint32_t nextId = 0;         // last returned ID, not the newest unsent ID
  bool more = false, dropped = false, reset = false;
};

// Select a bounded page without copying events or allocating memory. The caller
// compares boot IDs; absent/new-boot cursors start with retained history, while
// an invalid same-boot cursor reports a gap. `available` is the Ring's size,
// not its lifetime total. Unsigned subtraction preserves cursors through the
// uint32 ID wrap (including a valid nextId of zero).
inline Selection selectWindow(uint32_t total, size_t available, bool haveCursor,
                              bool sameBoot, uint32_t since,
                              size_t limit = kMaxEventsPerResponse) {
  if (limit == 0 || limit > kMaxEventsPerResponse) limit = kMaxEventsPerResponse;
  Selection selected;
  const bool newCursor = !haveCursor || !sameBoot;
  uint32_t remaining = total - since;
  selected.reset = newCursor || remaining > available;
  if (selected.reset) {
    selected.dropped = !newCursor;
    remaining = static_cast<uint32_t>(available);
    since = total - remaining;
  } else {
    selected.start = available - remaining;
  }
  selected.count = remaining < limit ? remaining : limit;
  selected.nextId = since + static_cast<uint32_t>(selected.count);
  selected.more = selected.count < remaining;
  return selected;
}

template <size_t Capacity>
class Ring {
 public:
  void push(Event event) {
    event.id = ++total_;
    events_[head_] = event;
    head_ = (head_ + 1) % Capacity;
    if (size_ < Capacity) ++size_; else ++overwritten_;
  }
  size_t size() const { return size_; }
  uint32_t total() const { return total_; }
  uint32_t overwritten() const { return overwritten_; }
  const Event &at(size_t index) const { return events_[(head_ + Capacity - size_ + index) % Capacity]; }
  Selection select(bool haveCursor, bool sameBoot, uint32_t since,
                   size_t limit = kMaxEventsPerResponse) const {
    return selectWindow(total_, size_, haveCursor, sameBoot, since, limit);
  }
 private:
  Event events_[Capacity]{};
  size_t head_ = 0, size_ = 0;
  uint32_t total_ = 0, overwritten_ = 0;
};
}  // namespace packet_diagnostics
