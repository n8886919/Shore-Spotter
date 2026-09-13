#pragma once
#include <math.h>
#include "taiwan_wmm_grid.h"

namespace magnetic_declination {
// WMM2025 is valid until the end of 2029. Use the GPS UTC date, not uptime.
inline bool decimalYear(unsigned year, unsigned month, unsigned day, float &out) {
  if (year < 2025 || year >= 2030 || month < 1 || month > 12) return false;
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const unsigned limit = days[month - 1] + (month == 2 && leap ? 1 : 0);
  if (day < 1 || day > limit) return false;
  unsigned elapsed = day - 1;
  for (unsigned m = 1; m < month; ++m) elapsed += days[m - 1];
  if (leap && month > 2) ++elapsed;
  out = year + static_cast<float>(elapsed) / (leap ? 366 : 365);
  return true;
}

// Sea-level WMM declination, east-positive. Bilinear position interpolation and
// linear secular change across 2025..2030; no extrapolation outside this grid.
inline bool taiwanDegrees(double lat, double lon, float year, float &out) {
  using namespace taiwan_wmm_grid;
  if (!isfinite(lat) || !isfinite(lon) || !isfinite(year) ||
      lat < kLatMin || lat > kLatMax || lon < kLonMin || lon > kLonMax ||
      year < 2025.0f || year >= 2030.0f) return false;
  const float y = static_cast<float>(lat - kLatMin);
  const float x = static_cast<float>(lon - kLonMin);
  int row = static_cast<int>(y), col = static_cast<int>(x);
  if (row == kRows - 1) --row;
  if (col == kCols - 1) --col;
  const float fy = y - row, fx = x - col, time = (year - 2025.0f) / 5.0f;
  auto value = [&](int r, int c) {
    const auto &v = kValues[r * kCols + c];
    return (v[0] + time * v[1]) / 1000.0f;
  };
  const float a = value(row, col) * (1 - fx) + value(row, col + 1) * fx;
  const float b = value(row + 1, col) * (1 - fx) + value(row + 1, col + 1) * fx;
  out = a * (1 - fy) + b * fy;
  return true;
}

inline float trueBearing(float magneticBearing, float declination) {
  float result = fmodf(magneticBearing + declination, 360.0f);
  return result < 0 ? result + 360.0f : result;
}
}  // namespace magnetic_declination
