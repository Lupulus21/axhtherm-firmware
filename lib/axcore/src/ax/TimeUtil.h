#pragma once
#include <stdint.h>

namespace ax {

// Conversioni civile<->epoch UTC indipendenti da TZ/libc (algoritmo days_from_civil).
inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

inline uint32_t epochFromYmdHms(int y, unsigned mo, unsigned d, unsigned h, unsigned mi,
                                unsigned s) {
  return (uint32_t)(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
}

inline void ymdHmsFromEpoch(uint32_t epoch, int* y, unsigned* mo, unsigned* d, unsigned* h,
                            unsigned* mi, unsigned* s) {
  int64_t z = epoch / 86400;
  uint32_t rem = epoch % 86400;
  *h = rem / 3600;
  *mi = (rem % 3600) / 60;
  *s = rem % 60;
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int yy = (int)yoe + (int)era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *mo = mp + (mp < 10 ? 3 : -9);
  *y = yy + (*mo <= 2);
}

}  // namespace ax
