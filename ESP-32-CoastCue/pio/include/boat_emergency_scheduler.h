#pragma once

#include <cstdint>

namespace coastcue {

enum class BoatEmergencyKind : uint8_t {
  kNone = 0,
  kHelp = 1,
  kSos = 2,
};

enum class BoatEmergencyQueueResult : uint8_t {
  kQueued,
  kDuplicate,
  kUpgraded,
  kUpgradePending,
};

enum class BoatEmergencyCompletion : uint8_t {
  kNone,
  kRetryPending,
  kExhausted,
  kUpgraded,
};

enum class BoatIncidentIdentityAction : uint8_t {
  kKeepCurrent,
  kStartNew,
};

// A Help→SOS escalation intentionally starts a new Incident. This reopens the
// operator-attention lifecycle and lets the shore supersede the lower-severity
// Help instead of silently treating SOS as a same-ID retransmission.
inline BoatIncidentIdentityAction decideBoatIncidentIdentity(
    bool hasIncident, bool incidentTerminal, BoatEmergencyKind currentKind,
    BoatEmergencyKind requestedKind) {
  if (!hasIncident || incidentTerminal ||
      currentKind == BoatEmergencyKind::kNone) {
    return BoatIncidentIdentityAction::kStartNew;
  }
  if (currentKind == BoatEmergencyKind::kHelp &&
      requestedKind == BoatEmergencyKind::kSos) {
    return BoatIncidentIdentityAction::kStartNew;
  }
  return BoatIncidentIdentityAction::kKeepCurrent;
}

// A same-severity press after Receipt/Acknowledged is neither a new Incident
// nor an Open retry. Escalation and a terminal Incident remain exceptions.
inline bool shouldQueueBoatEmergencyAction(
    bool hasIncident, bool incidentTerminal, bool shouldRetryOpen,
    BoatEmergencyKind currentKind, BoatEmergencyKind requestedKind) {
  if (!hasIncident || incidentTerminal || shouldRetryOpen) return true;
  return currentKind == BoatEmergencyKind::kHelp &&
         requestedKind == BoatEmergencyKind::kSos;
}

// The Help identity remains current until its post-uplink response window has
// closed. This preserves a valid Help Receipt when SOS is pressed immediately
// after the radio finishes transmitting.
inline bool shouldDeferBoatSosIdentityHandoff(
    bool hasOpenHelp, bool helpTransmitInFlight, bool helpResponseWindowOpen) {
  return hasOpenHelp && (helpTransmitInFlight || helpResponseWindowOpen);
}

inline bool shouldMarkBoatSosIdentityPending(
    BoatEmergencyQueueResult result) {
  // During the old Help response window the burst may already be exhausted,
  // so queue(SOS) starts a fresh scheduler entry instead of reporting an
  // in-queue upgrade. Both outcomes still need a new SOS Incident identity
  // after that response window closes.
  return result == BoatEmergencyQueueResult::kQueued ||
         result == BoatEmergencyQueueResult::kUpgraded;
}

class BoatEmergencyScheduler {
 public:
  static constexpr uint8_t kMaxAttempts = 3;

  BoatEmergencyQueueResult queue(BoatEmergencyKind kind) {
    if (kind == BoatEmergencyKind::kNone) {
      return BoatEmergencyQueueResult::kDuplicate;
    }
    if (!hasPending()) {
      activeKind_ = kind;
      attemptCount_ = 0;
      upgradePending_ = false;
      return BoatEmergencyQueueResult::kQueued;
    }
    if (kind == BoatEmergencyKind::kSos &&
        activeKind_ == BoatEmergencyKind::kHelp) {
      if (attemptInFlight_) {
        upgradePending_ = true;
        return BoatEmergencyQueueResult::kUpgradePending;
      }
      activeKind_ = BoatEmergencyKind::kSos;
      attemptCount_ = 0;
      return BoatEmergencyQueueResult::kUpgraded;
    }
    return BoatEmergencyQueueResult::kDuplicate;
  }

  bool attemptDue(uint32_t now, uint32_t retryPeriodMs,
                  bool force = false) const {
    return hasPending() && !attemptInFlight_ &&
           (force || attemptCount_ == 0 ||
            now - lastAttemptAt_ >= retryPeriodMs);
  }

  void attemptStarted(uint32_t now) {
    if (!hasPending() || attemptInFlight_) return;
    attemptInFlight_ = true;
    ++attemptCount_;
    lastAttemptAt_ = now;
  }

  BoatEmergencyCompletion attemptStartFailed(uint32_t now) {
    if (!hasPending() || attemptInFlight_) {
      return BoatEmergencyCompletion::kNone;
    }
    ++attemptCount_;
    lastAttemptAt_ = now;
    return settleAttempt();
  }

  BoatEmergencyCompletion attemptFinished(uint32_t now) {
    if (!attemptInFlight_) return BoatEmergencyCompletion::kNone;
    attemptInFlight_ = false;
    lastAttemptAt_ = now;
    return settleAttempt();
  }

  // A receipt can only cancel a queued retry. If a local transmit is already
  // in flight, its completion must be observed first so callers retain the
  // radio completion callback's ownership.
  bool cancelPending() {
    if (!hasPending() || attemptInFlight_) return false;
    activeKind_ = BoatEmergencyKind::kNone;
    attemptCount_ = 0;
    upgradePending_ = false;
    return true;
  }

  bool hasPending() const {
    return activeKind_ != BoatEmergencyKind::kNone;
  }
  bool attemptInFlight() const { return attemptInFlight_; }
  BoatEmergencyKind activeKind() const { return activeKind_; }
  uint8_t attemptCount() const { return attemptCount_; }

 private:
  BoatEmergencyCompletion settleAttempt() {
    if (upgradePending_) {
      activeKind_ = BoatEmergencyKind::kSos;
      attemptCount_ = 0;
      upgradePending_ = false;
      return BoatEmergencyCompletion::kUpgraded;
    }
    if (attemptCount_ >= kMaxAttempts) {
      activeKind_ = BoatEmergencyKind::kNone;
      attemptCount_ = 0;
      return BoatEmergencyCompletion::kExhausted;
    }
    return BoatEmergencyCompletion::kRetryPending;
  }

  BoatEmergencyKind activeKind_ = BoatEmergencyKind::kNone;
  uint8_t attemptCount_ = 0;
  uint32_t lastAttemptAt_ = 0;
  bool attemptInFlight_ = false;
  bool upgradePending_ = false;
};

}  // namespace coastcue
