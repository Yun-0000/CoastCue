#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "boat_emergency_scheduler.h"
#include "incident_protocol.h"

namespace coastcue {

// Explicit byte encoding keeps the NVS checkpoint independent of compiler
// padding and makes one Preferences blob the atomic Incident commit unit.
constexpr size_t kBoatIncidentSnapshotBytes = 26;

struct BoatIncidentSnapshot {
  char incidentId[17] = {};
  IncidentState state = IncidentState::kNone;
  uint32_t stateSeq = 0;
  bool hasStateSeq = false;
  BoatEmergencyKind kind = BoatEmergencyKind::kNone;
};

inline bool isValidBoatEmergencyKind(BoatEmergencyKind kind) {
  return kind == BoatEmergencyKind::kHelp ||
         kind == BoatEmergencyKind::kSos;
}

inline bool encodeBoatIncidentSnapshot(
    const BoatIncidentSnapshot& snapshot, uint8_t* encoded, size_t length) {
  BoatIncidentState validator;
  if (encoded == nullptr || length != kBoatIncidentSnapshotBytes ||
      snapshot.incidentId[16] != '\0' ||
      !isValidBoatEmergencyKind(snapshot.kind) ||
      !validator.restore(snapshot.incidentId, snapshot.state,
                         snapshot.stateSeq, snapshot.hasStateSeq)) {
    return false;
  }
  encoded[0] = 'C';
  encoded[1] = 'C';
  encoded[2] = 'I';
  encoded[3] = static_cast<uint8_t>(snapshot.state);
  encoded[4] = static_cast<uint8_t>(snapshot.kind);
  encoded[5] = snapshot.hasStateSeq ? 1 : 0;
  encoded[6] = snapshot.stateSeq & 0xFF;
  encoded[7] = (snapshot.stateSeq >> 8) & 0xFF;
  encoded[8] = (snapshot.stateSeq >> 16) & 0xFF;
  encoded[9] = (snapshot.stateSeq >> 24) & 0xFF;
  std::memcpy(encoded + 10, snapshot.incidentId, 16);
  return true;
}

inline bool decodeBoatIncidentSnapshot(
    const uint8_t* encoded, size_t length, BoatIncidentSnapshot& snapshot) {
  if (encoded == nullptr || length != kBoatIncidentSnapshotBytes ||
      encoded[0] != 'C' || encoded[1] != 'C' || encoded[2] != 'I' ||
      (encoded[5] & ~1U) != 0) {
    return false;
  }
  BoatIncidentSnapshot decoded;
  decoded.state = static_cast<IncidentState>(encoded[3]);
  decoded.kind = static_cast<BoatEmergencyKind>(encoded[4]);
  decoded.hasStateSeq = encoded[5] != 0;
  decoded.stateSeq = static_cast<uint32_t>(encoded[6]) |
                     (static_cast<uint32_t>(encoded[7]) << 8) |
                     (static_cast<uint32_t>(encoded[8]) << 16) |
                     (static_cast<uint32_t>(encoded[9]) << 24);
  std::memcpy(decoded.incidentId, encoded + 10, 16);
  decoded.incidentId[16] = '\0';
  BoatIncidentState validator;
  if (!isValidBoatEmergencyKind(decoded.kind) ||
      !validator.restore(decoded.incidentId, decoded.state, decoded.stateSeq,
                         decoded.hasStateSeq)) {
    return false;
  }
  snapshot = decoded;
  return true;
}

}  // namespace coastcue
