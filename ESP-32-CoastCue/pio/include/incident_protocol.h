#pragma once

#include <cstdint>
#include <cstring>

namespace coastcue {

// This is deliberately a normalized protocol boundary: JSON parsing, HMAC
// verification, and radio scheduling remain in runtime adapters.
enum class IncidentState : uint8_t {
  kNone = 0,
  kOpen,
  kReceipt,
  kAcknowledged,
  kResolved,
};

enum class IncidentApplyResult : uint8_t {
  kApplied,
  kDuplicate,
  kStale,
  kConflict,
  kTerminalIgnored,
  kInvalid,
};

// A state receipt is a durable acknowledgement from the boat. Invalid or
// conflicting frames are never accepted, and an otherwise valid revision is
// not confirmed until its local Incident checkpoint has been stored.
inline bool shouldSendStateReceipt(IncidentApplyResult result,
                                   bool statePersisted) {
  return statePersisted &&
         (result == IncidentApplyResult::kApplied ||
          result == IncidentApplyResult::kDuplicate);
}

// A new Incident identity supersedes any unsent confirmation for the previous
// Incident.  It must not delay a Help->SOS escalation, while an already
// in-flight receipt is allowed to finish because the radio cannot cancel it.
inline bool shouldRetirePendingStateReceipt(
    bool pending, bool inFlight, const char* receiptIncidentId,
    const char* activeIncidentId) {
  return pending && !inFlight && receiptIncidentId != nullptr &&
         activeIncidentId != nullptr &&
         std::strcmp(receiptIncidentId, activeIncidentId) != 0;
}

// A boat receipt confirms only the exact revision the shore still has queued.
// A stale lower sequence must not mutate the durable confirmation checkpoint.
inline bool isMatchingPendingStateReceipt(const char* receiptIncidentId,
                                          uint32_t receiptStateSeq,
                                          const char* pendingIncidentId,
                                          uint32_t pendingStateSeq) {
  return receiptStateSeq > 0 && receiptStateSeq == pendingStateSeq &&
         receiptIncidentId != nullptr && pendingIncidentId != nullptr &&
         std::strcmp(receiptIncidentId, pendingIncidentId) == 0;
}

inline const char* incidentStateWireName(IncidentState state) {
  switch (state) {
    case IncidentState::kOpen: return "open";
    case IncidentState::kReceipt: return "receipt";
    case IncidentState::kAcknowledged: return "acknowledged";
    case IncidentState::kResolved: return "resolved";
    case IncidentState::kNone: return "none";
  }
  return "none";
}

inline bool parseIncidentStateWireName(const char* wireName,
                                       IncidentState& state) {
  if (wireName == nullptr) return false;
  if (std::strcmp(wireName, "open") == 0) state = IncidentState::kOpen;
  else if (std::strcmp(wireName, "receipt") == 0) state = IncidentState::kReceipt;
  else if (std::strcmp(wireName, "acknowledged") == 0) state = IncidentState::kAcknowledged;
  else if (std::strcmp(wireName, "resolved") == 0) state = IncidentState::kResolved;
  else return false;
  return true;
}

inline uint8_t incidentStateProgress(IncidentState state) {
  switch (state) {
    case IncidentState::kOpen: return 1;
    case IncidentState::kReceipt: return 2;
    case IncidentState::kAcknowledged: return 3;
    case IncidentState::kResolved: return 4;
    case IncidentState::kNone: return 0;
  }
  return 0;
}

inline bool isValidIncidentId(const char* incidentId) {
  if (incidentId == nullptr || std::strlen(incidentId) != 16) return false;
  for (size_t index = 0; index < 16; ++index) {
    const char value = incidentId[index];
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f'))) return false;
  }
  return true;
}

struct IncidentUplink {
  IncidentUplink(const char* id = nullptr,
                 IncidentState incidentState = IncidentState::kNone,
                 bool emergency = false)
      : incidentId(id), state(incidentState), isEmergency(emergency) {}

  const char* incidentId;
  IncidentState state;
  bool isEmergency;  // true only for help or sos, never normal GPS.
};

struct IncidentDownlink {
  IncidentDownlink(const char* id = nullptr,
                   IncidentState incidentState = IncidentState::kNone,
                   uint32_t sequence = 0)
      : incidentId(id), state(incidentState), stateSeq(sequence) {}

  const char* incidentId;
  IncidentState state;
  uint32_t stateSeq;  // Monotonic shore revision, not a state rank.
};

inline bool isValidIncidentUplink(const IncidentUplink& frame) {
  return isValidIncidentId(frame.incidentId) && frame.isEmergency &&
         frame.state == IncidentState::kOpen;
}

inline bool isValidIncidentDownlink(const IncidentDownlink& frame) {
  return isValidIncidentId(frame.incidentId) && frame.stateSeq > 0 &&
         (frame.state == IncidentState::kReceipt ||
          frame.state == IncidentState::kAcknowledged ||
          frame.state == IncidentState::kResolved);
}

class BoatIncidentState {
 public:
  bool begin(const char* incidentId) {
    if (!isValidIncidentId(incidentId)) return false;
    copyIncidentId(incidentId);
    state_ = IncidentState::kOpen;
    lastStateSeq_ = 0;
    hasStateSeq_ = false;
    return true;
  }

  // Sequence values are shore revisions; they are never inferred from state.
  bool restore(const char* incidentId, IncidentState state, uint32_t stateSeq,
               bool hasStateSeq = true) {
    const bool openState = state == IncidentState::kOpen;
    const bool deliveredState =
        state == IncidentState::kReceipt ||
        state == IncidentState::kAcknowledged ||
        state == IncidentState::kResolved;
    if (!isValidIncidentId(incidentId) || (!openState && !deliveredState) ||
        (openState && (hasStateSeq || stateSeq != 0)) ||
        (deliveredState && (!hasStateSeq || stateSeq == 0))) return false;
    copyIncidentId(incidentId);
    state_ = state;
    lastStateSeq_ = stateSeq;
    hasStateSeq_ = hasStateSeq;
    return true;
  }

  IncidentApplyResult applyDownlink(const IncidentDownlink& frame) {
    if (!isValidIncidentDownlink(frame) || !hasIncident() ||
        std::strcmp(frame.incidentId, incidentId_) != 0) {
      return IncidentApplyResult::kInvalid;
    }
    if (state_ == IncidentState::kResolved) {
      return hasStateSeq_ && frame.stateSeq == lastStateSeq_ &&
                     frame.state == IncidentState::kResolved
                 ? IncidentApplyResult::kDuplicate
                 : IncidentApplyResult::kTerminalIgnored;
    }
    if (hasStateSeq_) {
      if (frame.stateSeq < lastStateSeq_) return IncidentApplyResult::kStale;
      if (frame.stateSeq == lastStateSeq_) {
        return frame.state == state_ ? IncidentApplyResult::kDuplicate
                                     : IncidentApplyResult::kConflict;
      }
    }
    if (incidentStateProgress(frame.state) <
        incidentStateProgress(state_)) {
      return IncidentApplyResult::kStale;
    }
    state_ = frame.state;
    lastStateSeq_ = frame.stateSeq;
    hasStateSeq_ = true;
    return IncidentApplyResult::kApplied;
  }

  bool hasIncident() const { return incidentId_[0] != '\0'; }
  bool shouldRetryOpen() const { return state_ == IncidentState::kOpen; }
  bool isTerminal() const { return state_ == IncidentState::kResolved; }
  const char* incidentId() const { return incidentId_; }
  IncidentState state() const { return state_; }
  uint32_t lastStateSeq() const { return lastStateSeq_; }
  bool hasStateSeq() const { return hasStateSeq_; }

 private:
  void copyIncidentId(const char* incidentId) {
    std::memcpy(incidentId_, incidentId, 16);
    incidentId_[16] = '\0';
  }

  char incidentId_[17] = {};
  IncidentState state_ = IncidentState::kNone;
  uint32_t lastStateSeq_ = 0;
  bool hasStateSeq_ = false;
};

inline bool shouldOpenIncidentResponseWindow(
    const char* transmittedIncidentId, const BoatIncidentState& current) {
  return transmittedIncidentId != nullptr && current.hasIncident() &&
         std::strcmp(transmittedIncidentId, current.incidentId()) == 0;
}

// After accepting Resolved, the boat briefly reconciles the final state on
// later heartbeat response slots. Consecutive quiet slots mean the shore has
// most likely consumed the boat's state receipt, so listening can stop.
class BoatTerminalReconciliation {
 public:
  void begin() {
    active_ = true;
    quietWindows_ = 0;
  }

  void observeStateReplay() {
    if (active_) quietWindows_ = 0;
  }

  void observeQuietWindow() {
    if (!active_) return;
    if (++quietWindows_ >= 2) active_ = false;
  }

  bool shouldListen() const { return active_; }

 private:
  bool active_ = false;
  uint8_t quietWindows_ = 0;
};

}  // namespace coastcue
