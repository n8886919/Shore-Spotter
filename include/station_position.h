#pragma once
#include <cmath>
#include <cstdint>

// A time-window mean for a stationary shore station. Every fresh fix is retained
// in the window; scatter is reported, never used to silently discard a point.
namespace station_position {
constexpr uint32_t kWindowMs = 30000;
constexpr double kWarnRmsM = 3.0;
class Average {
 public:
  void observe(uint32_t now, bool fresh, uint32_t epoch, double lat, double lon) {
    bool changed = false;
    while (size_ && now - points_[first_].ms >= kWindowMs) {
      first_ = (first_ + 1) % kCapacity; --size_; changed = true;
    }
    if (fresh && std::isfinite(lat) && std::isfinite(lon) &&
        lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 &&
        (!haveEpoch_ || epoch != lastEpoch_)) {
      haveEpoch_ = true; lastEpoch_ = epoch;
      if (size_ == kCapacity) { first_ = (first_ + 1) % kCapacity; --size_; }
      points_[(first_ + size_) % kCapacity] = {now, lat, lon};
      ++size_; changed = true;
    }
    if (changed) recompute();
  }
  unsigned count() const { return size_; }
  double latitude() const { return lat_; }
  double longitude() const { return lon_; }
  double rmsM() const { return rms_; }
  bool warning() const { return size_ >= 2 && rms_ > kWarnRmsM; }
 private:
  static constexpr unsigned kCapacity = 64; // 30 s at 2 Hz, plus boundary headroom
  struct Point { uint32_t ms = 0; double lat = 0, lon = 0; } points_[kCapacity];
  unsigned first_ = 0, size_ = 0;
  bool haveEpoch_ = false;
  uint32_t lastEpoch_ = 0;
  double lat_ = 0, lon_ = 0, rms_ = 0;
  void recompute() {
    lat_ = lon_ = rms_ = 0;
    if (!size_) return;
    for (unsigned i = 0; i < size_; ++i) {
      const auto &p = points_[(first_ + i) % kCapacity]; lat_ += p.lat; lon_ += p.lon;
    }
    lat_ /= size_; lon_ /= size_;
    const double lonScale = 111320.0 * std::cos(lat_ * 0.017453292519943295);
    for (unsigned i = 0; i < size_; ++i) {
      const auto &p = points_[(first_ + i) % kCapacity];
      const double n = (p.lat - lat_) * 111320.0, e = (p.lon - lon_) * lonScale;
      rms_ += n*n + e*e;
    }
    rms_ = std::sqrt(rms_ / size_);
  }
};
}  // namespace station_position
