#pragma once

#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

// Collect the fields of one GNSS UTC epoch, rather than combining TinyGPS
// fields whose last successful updates may belong to different epochs.
namespace gnss_snapshot {
constexpr uint32_t kDayMs = 86400000;
constexpr uint32_t kBacklogAllowanceMs = 200;
constexpr uint8_t kUnknownSatellites = 255;
constexpr const char *kAgeOrigin = "nmea_epoch_aligned_arrival";

struct Snapshot {
  double lat = 0.0, lon = 0.0;
  double speedMps = 0.0, courseDeg = 0.0;
  bool fix = false, velocityValid = false;
  uint8_t satellites = kUnknownSatellites;
  double hdop = NAN;
  uint32_t sourceAgeMs = UINT32_MAX, epochMsOfDay = 0;
  uint32_t ageUncertaintyMs = kBacklogAllowanceMs;
  bool haveEpoch = false;
  bool haveRmc = false, haveGga = false;
};

struct Counters {
  uint32_t acceptedSentences = 0, rejectedSentences = 0, overflows = 0;
  uint32_t duplicateEpochs = 0, backwardEpochs = 0, invalidations = 0;
  uint32_t rmcSentences = 0, ggaSentences = 0, checksumErrors = 0;
  uint32_t snapshots = 0, lastEpochIntervalMs = 0;
  uint32_t ignoredSentences = 0;
};

class Collector {
 public:
  // The allowance bounds the caller's local UART servicing delay. The caller
  // MUST discard the pending UART bytes and invalidate() if that bound fails.
  // Neither this bound nor the serial correction measures the receiver's
  // internal solution latency. This is NOT GPS-to-millis clock synchronization.
  explicit Collector(uint32_t baud = 9600,
                     uint32_t backlogAllowanceMs = kBacklogAllowanceMs)
      : baud_(baud ? baud : 9600), allowanceMs_(backlogAllowanceMs) {}

  const Counters &counters() const { return counters_; }
  const Counters &stats() const { return counters_; }
  uint32_t backlogAllowanceMs() const { return allowanceMs_; }

  void feed(char ch, uint32_t now) {
    if (ch == '$') {
      collecting_ = true;
      length_ = 0;
      lineStartMs_ = now;
      line_[length_++] = ch;
      return;
    }
    if (!collecting_) return;
    if (ch == '\n') {
      collecting_ = false;
      line_[length_] = '\0';
      if (!consume(now)) ++counters_.rejectedSentences;
      return;
    }
    if (ch == '\r') return;
    if (static_cast<unsigned char>(ch) < 32 ||
        static_cast<unsigned char>(ch) > 126) {
      collecting_ = false;
      ++counters_.rejectedSentences;
      return;
    }
    if (length_ + 1 >= sizeof(line_)) {
      collecting_ = false;
      ++counters_.overflows;
      ++counters_.rejectedSentences;
      return;
    }
    line_[length_++] = ch;
  }

  bool sample(uint32_t now, Snapshot &out) const {
    out = Snapshot{};
    if (!current_.present) return false;
    // RMC and GGA travel sequentially over UART. During the short interval
    // before the newest GGA arrives, retain an older coherent sample with its
    // ORIGINAL age instead of fabricating missing quality every other RF tick.
    // Explicit invalidity always wins. A newest GGA without RMC is useful as
    // position/quality only and must not borrow an older velocity vector.
    const bool holdPrevious = current_.rmc && current_.rmcFix &&
        !current_.invalidRmc && !current_.invalidGga && !current_.gga &&
        previous_.present && previous_.gga && previous_.ggaFix &&
        !previous_.invalidRmc && !previous_.invalidGga;
    const Epoch &e = holdPrevious ? previous_ : current_;
    out.haveEpoch = true;
    out.ageUncertaintyMs = allowanceMs_;
    out.epochMsOfDay = e.utc;
    out.sourceAgeMs = now - e.origin;
    out.haveRmc = e.rmc;
    out.haveGga = e.gga;
    const bool veto = e.invalidRmc || e.invalidGga;
    out.fix = !veto && ((e.gga && e.ggaFix) || (e.rmc && e.rmcFix));
    if (e.gga && e.ggaFix) {
      out.lat = e.ggaLat;
      out.lon = e.ggaLon;
    } else if (e.rmc && e.rmcFix) {
      out.lat = e.rmcLat;
      out.lon = e.rmcLon;
    }
    if (e.gga) {
      out.satellites = e.satellites;
      out.hdop = e.hdop;
    }
    out.velocityValid = out.fix && e.rmc && e.rmcVelocity;
    if (out.velocityValid) {
      out.speedMps = e.speed;
      out.courseDeg = e.course;
    }
    // Freshness is intentionally left to the caller's policy. An old sample
    // remains observable as old, rather than masquerading as a new no-fix epoch.
    return true;
  }

  // A servicing fault must not allow a repeated epoch to acquire a new origin.
  // Preserve the UTC baseline, reject that same epoch until a newer one arrives.
  void invalidate(uint32_t /*now*/) {
    collecting_ = false;
    length_ = 0;
    current_ = Epoch{};
    previous_ = Epoch{};
    invalidated_ = true;
    ++counters_.invalidations;
  }

  // Explicit GNSS reset/reconfiguration may establish a new UTC baseline.
  void reset(uint32_t now = 0) {
    invalidate(now);
    haveAnchor_ = false;
    invalidated_ = false;
  }

 private:
  struct Epoch {
    bool present = false;
    uint32_t utc = 0, origin = 0;
    bool rmc = false, gga = false, rmcFix = false, ggaFix = false;
    bool invalidRmc = false, invalidGga = false, rmcVelocity = false;
    double rmcLat = 0, rmcLon = 0, ggaLat = 0, ggaLon = 0;
    double speed = 0, course = 0, hdop = NAN;
    uint8_t satellites = kUnknownSatellites;
  };

  static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }

  static bool decimal(const char *s, double &v) {
    if (!s || !*s) return false;
    bool dot = false, digit = false;
    unsigned count = 0;
    double scale = 1;
    v = 0;
    for (; *s; ++s) {
      if (++count > 20) return false;
      if (*s == '.' && !dot) { dot = true; continue; }
      if (*s < '0' || *s > '9') return false;
      digit = true;
      if (dot) { scale *= 0.1; v += (*s - '0') * scale; }
      else v = v * 10 + (*s - '0');
    }
    return digit && isfinite(v);
  }

  static bool integer(const char *s, unsigned limit, unsigned &v) {
    if (!s || !*s) return false;
    v = 0;
    for (; *s; ++s) {
      if (*s < '0' || *s > '9') return false;
      v = v * 10 + static_cast<unsigned>(*s - '0');
      if (v > limit) return false;
    }
    return true;
  }

  static bool utcTime(const char *s, uint32_t &utc) {
    if (strlen(s) < 6) return false;
    for (unsigned i = 0; i < 6; ++i)
      if (s[i] < '0' || s[i] > '9') return false;
    unsigned h = (s[0] - '0') * 10 + s[1] - '0';
    unsigned m = (s[2] - '0') * 10 + s[3] - '0';
    unsigned sec = (s[4] - '0') * 10 + s[5] - '0';
    if (h > 23 || m > 59 || sec > 59) return false;
    unsigned fraction = 0;
    if (s[6]) {
      if (s[6] != '.' || !s[7]) return false;
      unsigned places = 0;
      for (const char *p = s + 7; *p; ++p) {
        if (*p < '0' || *p > '9' || places == 3) return false;
        fraction = fraction * 10 + *p - '0';
        ++places;
      }
      while (places++ < 3) fraction *= 10;
    }
    utc = (h * 3600UL + m * 60UL + sec) * 1000 + fraction;
    return true;
  }

  static bool coordinate(const char *s, const char *hemisphere,
                         bool latitude, double &degrees) {
    double encoded;
    if (!decimal(s, encoded) || !hemisphere[0] || hemisphere[1]) return false;
    const double whole = floor(encoded / 100.0);
    const double minutes = encoded - whole * 100.0;
    const double limit = latitude ? 90.0 : 180.0;
    if (minutes >= 60.0 || whole > limit ||
        (whole == limit && minutes > 0.0)) return false;
    const char positive = latitude ? 'N' : 'E';
    const char negative = latitude ? 'S' : 'W';
    if (hemisphere[0] != positive && hemisphere[0] != negative) return false;
    degrees = (whole + minutes / 60.0) * (hemisphere[0] == negative ? -1 : 1);
    return true;
  }

  Epoch *selectEpoch(uint32_t utc, uint32_t now, uint32_t observedOrigin) {
    if (haveAnchor_) {
      const uint32_t delta = (utc + kDayMs - anchorUtc_) % kDayMs;
      if (delta == 0) {
        if (invalidated_ || !current_.present) {
          ++counters_.duplicateEpochs;
          return nullptr;
        }
        // A later sentence/duplicate cannot move this epoch's age forward.
        if (now - observedOrigin > now - current_.origin) {
          current_.origin = observedOrigin;
          anchorOrigin_ = observedOrigin;
        }
        return &current_;
      }
      if (delta >= kDayMs / 2) {
        if (!invalidated_ && previous_.present && previous_.utc == utc)
          return &previous_;  // interleaved older counterpart, never latest
        ++counters_.backwardEpochs;
        return nullptr;
      }
      counters_.lastEpochIntervalMs = delta;
      const uint32_t projectedOrigin = anchorOrigin_ + delta;
      // UTC progression detects late/backlogged epochs. Never stamp them as
      // fresh simply because the bytes were parsed in this service pass.
      if (static_cast<int32_t>(now - projectedOrigin) >= 0 &&
          now - projectedOrigin > now - observedOrigin)
        observedOrigin = projectedOrigin;
    }
    previous_ = current_;
    current_ = Epoch{};
    current_.present = true;
    current_.utc = utc;
    current_.origin = observedOrigin;
    anchorUtc_ = utc;
    anchorOrigin_ = observedOrigin;
    haveAnchor_ = true;
    invalidated_ = false;
    ++counters_.snapshots;
    return &current_;
  }

  bool consume(uint32_t now) {
    char *star = strchr(line_, '*');
    if (!star || star - line_ < 6 || strlen(star + 1) != 2) return false;
    const int hi = hex(star[1]), lo = hex(star[2]);
    if (hi < 0 || lo < 0) { ++counters_.checksumErrors; return false; }
    uint8_t checksum = 0;
    for (char *p = line_ + 1; p < star; ++p) checksum ^= static_cast<uint8_t>(*p);
    if (checksum != ((hi << 4) | lo)) {
      ++counters_.checksumErrors;
      return false;
    }
    // GSV/GSA/VTG/TXT etc. are normal GNSS output, not decoder errors.
    // Ignore them before tokenization (some contain more fields than RMC/GGA).
    if (strncmp(line_ + 1, "GNRMC,", 6) != 0 &&
        strncmp(line_ + 1, "GPRMC,", 6) != 0 &&
        strncmp(line_ + 1, "GNGGA,", 6) != 0 &&
        strncmp(line_ + 1, "GPGGA,", 6) != 0) {
      ++counters_.ignoredSentences;
      return true;
    }
    *star = '\0';
    char *fields[20];
    size_t count = 0;
    fields[count++] = line_ + 1;
    for (char *p = line_ + 1; *p; ++p) {
      if (*p != ',') continue;
      if (count == sizeof(fields) / sizeof(fields[0])) return false;
      *p = '\0';
      fields[count++] = p + 1;
    }
    if (strlen(fields[0]) != 5 || fields[0][0] != 'G' ||
        (fields[0][1] != 'N' && fields[0][1] != 'P')) return false;
    const bool rmc = strcmp(fields[0] + 2, "RMC") == 0;
    const bool gga = strcmp(fields[0] + 2, "GGA") == 0;
    if ((!rmc && !gga) || (rmc && count < 10) || (gga && count < 10)) return false;
    uint32_t utc;
    if (!utcTime(fields[1], utc)) return false;

    double lat = 0, lon = 0, speed = 0, course = 0, hdop = NAN;
    bool fix = false, velocity = false;
    uint8_t satellites = kUnknownSatellites;
    if (rmc) {
      if ((strcmp(fields[2], "A") != 0) && (strcmp(fields[2], "V") != 0)) return false;
      fix = fields[2][0] == 'A';
      if (fix && (!coordinate(fields[3], fields[4], true, lat) ||
                  !coordinate(fields[5], fields[6], false, lon))) return false;
      // Explicit estimated/manual/simulated/no-fix mode is not a measured fix.
      if (count > 12 && fields[12][0] &&
          strchr("EMNS", fields[12][0])) fix = false;
      velocity = fix && decimal(fields[7], speed) && decimal(fields[8], course) &&
                 course >= 0 && course < 360;
      speed *= 1852.0 / 3600.0;
    } else {
      unsigned quality, sats;
      if (!integer(fields[6], 8, quality)) return false;
      fix = quality >= 1 && quality <= 5;
      if (fix && (!coordinate(fields[2], fields[3], true, lat) ||
                  !coordinate(fields[4], fields[5], false, lon))) return false;
      if (integer(fields[7], 254, sats)) satellites = static_cast<uint8_t>(sats);
      if (!decimal(fields[8], hdop)) hdop = NAN;
    }
    const uint32_t serializedMs = static_cast<uint32_t>(
        (static_cast<uint64_t>(length_ + 2) * 10000 + baud_ - 1) / baud_);
    const uint32_t observedDuration = now - lineStartMs_;
    const uint32_t duration = observedDuration > serializedMs ? observedDuration : serializedMs;
    // Uncertainty is used by the freshness gate, NEVER as invented elapsed
    // movement in the position predictor. Keep it separate from sourceAgeMs.
    Epoch *e = selectEpoch(utc, now, now - duration);
    if (!e) return false;
    // The expected RMC+GGA pair is one epoch, not a duplicate transmission.
    if ((rmc && e->rmc) || (gga && e->gga)) ++counters_.duplicateEpochs;
    if (rmc) {
      ++counters_.rmcSentences;
      e->rmc = true;
      e->rmcFix = fix;
      e->invalidRmc = e->invalidRmc || !fix;
      e->rmcLat = lat; e->rmcLon = lon;
      e->rmcVelocity = velocity;
      e->speed = speed; e->course = course;
    } else {
      ++counters_.ggaSentences;
      e->gga = true;
      e->ggaFix = fix;
      e->invalidGga = e->invalidGga || !fix;
      e->ggaLat = lat; e->ggaLon = lon;
      e->satellites = satellites; e->hdop = hdop;
    }
    ++counters_.acceptedSentences;
    return true;
  }

  uint32_t baud_, allowanceMs_;
  char line_[128]{};
  size_t length_ = 0;
  uint32_t lineStartMs_ = 0, anchorUtc_ = 0, anchorOrigin_ = 0;
  bool collecting_ = false, haveAnchor_ = false, invalidated_ = false;
  Epoch current_, previous_;
  Counters counters_;
};
}  // namespace gnss_snapshot
