#pragma once

#include <cstdint>

namespace coastcue {

struct BoatGpsPolicy {
  bool includeCoordinates;
  bool fresh;
  uint32_t ageSeconds;
};

inline BoatGpsPolicy selectBoatGpsPolicy(bool locationValid, uint32_t ageMs,
                                         bool emergency,
                                         uint32_t freshnessMs) {
  const bool hasLocation = locationValid && ageMs != UINT32_MAX;
  const bool fresh = hasLocation && ageMs <= freshnessMs;
  const uint32_t ageSeconds = ageMs / 1000UL;
  return {hasLocation && (fresh || emergency), fresh, ageSeconds};
}

}  // namespace coastcue
