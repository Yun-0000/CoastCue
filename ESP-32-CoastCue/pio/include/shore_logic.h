#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace coastcue {

constexpr size_t kBleStatusPayloadMaxBytes = 182;
constexpr size_t kLoraPacketMaxBytes = 255;

enum class JournalIdentityDecision : uint8_t {
  kUseMatching,
  kAdoptFilesystem,
  kSeedEmptyFilesystem,
  kCreateNew,
  kReject,
};

// A journal epoch is a 96-bit lowercase hexadecimal identity. The NVS mirror
// lets BLE advertise the same gateway identity while LittleFS is temporarily
// unavailable. Existing journal bytes are accepted only with their own valid,
// matching epoch marker.
bool isValidJournalEpoch(const char* epoch);
JournalIdentityDecision decideJournalIdentity(const char* mirroredEpoch,
                                               const char* filesystemEpoch,
                                               bool journalHasHistory,
                                               bool mirrorIsProvisional = false);

// Fixed-size, allocation-free replay filter. Every observed hash sets seven
// bits, so retained history cannot exhaust a row-count capacity. Membership
// can have false positives but never false negatives; callers can exact-confirm
// positives against retained storage without scanning the ordinary fast path.
// Keeping nonce and Incident filters separate prevents Incident churn from
// degrading otherwise valid routine telemetry admission.
template <size_t BitCount>
class ReplayFilter {
 public:
  static_assert(BitCount >= 64 && (BitCount & (BitCount - 1)) == 0,
                "replay filter bit count must be a power of two");

  bool contains(uint64_t hash) const {
    for (uint8_t probe = 0; probe < kProbeCount; ++probe) {
      const size_t bit = bitIndex(hash, probe);
      if ((bits_[bit / 64] & (uint64_t{1} << (bit % 64))) == 0) {
        return false;
      }
    }
    return true;
  }

  bool add(uint64_t hash) {
    if (!complete_) return false;
    const bool alreadyPresent = contains(hash);
    for (uint8_t probe = 0; probe < kProbeCount; ++probe) {
      const size_t bit = bitIndex(hash, probe);
      bits_[bit / 64] |= uint64_t{1} << (bit % 64);
    }
    if (!alreadyPresent) ++count_;
    return true;
  }

  void reset() {
    for (uint64_t &word : bits_) word = 0;
    count_ = 0;
    complete_ = true;
  }

  void failClosed() { complete_ = false; }
  bool complete() const { return complete_; }
  size_t count() const { return count_; }

 private:
  static constexpr uint8_t kProbeCount = 7;

  static uint64_t mix(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

  static size_t bitIndex(uint64_t hash, uint8_t probe) {
    return static_cast<size_t>(
        mix(hash + 0x9e3779b97f4a7c15ULL * (probe + 1)) & (BitCount - 1));
  }

  uint64_t bits_[BitCount / 64] = {};
  size_t count_ = 0;
  bool complete_ = true;
};

enum class ReplayAdmissionDecision : uint8_t {
  kAccept,
  kConfirmExact,
  kReject,
};

// A filter negative is definitive. A positive needs an exact retained-journal
// lookup to distinguish a duplicate from a false positive. An incomplete
// filter remains fail-closed without consulting partial state.
constexpr ReplayAdmissionDecision decideReplayAdmission(
    bool filterComplete, bool filterMayContain) {
  return !filterComplete
             ? ReplayAdmissionDecision::kReject
             : filterMayContain
                   ? ReplayAdmissionDecision::kConfirmExact
                   : ReplayAdmissionDecision::kAccept;
}

constexpr bool shouldRejectExactReplayLookup(bool exactMatch,
                                             bool scanComplete) {
  return exactMatch || !scanComplete;
}

// Exact upper bound for current boat-originated telemetry and Incident frames:
// fixed node UID, full-width coordinates/GPS age, battery, and final HMAC.
constexpr size_t maxAuthenticatedBoatUplinkBytes(bool incident) {
  return (sizeof("{\"nonce\":\"") - 1) + 16 +
         (sizeof("\",\"node_uid\":\"") - 1) + 18 +
         (incident ? (sizeof("\",\"i\":\"") - 1) + 16 : 0) +
         (sizeof("\",\"status\":\"") - 1) + (incident ? 4 : 6) +
         (sizeof("\",\"gps\":{\"valid\":true,\"latitude\":") - 1) + 10 +
         (sizeof(",\"longitude\":") - 1) + 11 +
         (sizeof(",\"age_s\":") - 1) + 7 +
         (sizeof("},\"battery_percent\":") - 1) + 3 +
         (sizeof(",\"mac\":\"") - 1) + 32 + (sizeof("\"}") - 1);
}

constexpr size_t kMaxTelemetryUplinkBytes =
    maxAuthenticatedBoatUplinkBytes(false);
constexpr size_t kMaxIncidentUplinkBytes =
    maxAuthenticatedBoatUplinkBytes(true);
static_assert(kMaxTelemetryUplinkBytes == 224,
              "telemetry size model must match the wire builder");
static_assert(kMaxIncidentUplinkBytes == 245,
              "Incident size model must match the wire builder");
static_assert(kMaxTelemetryUplinkBytes <= kLoraPacketMaxBytes &&
                  kMaxIncidentUplinkBytes <= kLoraPacketMaxBytes,
              "current boat uplinks must fit one SX127x packet");

// A packet is fresh only when it was received during this boot. Persisted
// journal history must never make a restarted gateway look radio-connected.
bool isRadioFresh(bool hasReceivedPacket, uint32_t nowMs, uint32_t lastPacketAtMs,
                  uint32_t maxAgeMs);

// Emergency events may use a bounded local checkpoint when LittleFS append
// fails. Normal telemetry is intentionally not checkpointed this way.
bool shouldCheckpointVolatileEmergency(const char* status, bool persisted);

// An operator Incident state can change only after its journal transition is
// durable. An unchanged/idempotent command remains valid without another row.
bool mayApplyOperatorTransition(bool changed, bool transitionPersisted);

// Fault-only escape hatch for the physical shore unit. It changes neither the
// Incident lifecycle nor the radio state; normal Ack/Resolve still require a
// durable transition.
constexpr bool shouldMuteBuzzerLocally(bool storageReady, bool hasActiveAlert,
                                       bool incidentOpen, uint32_t heldMs) {
  return !storageReady && hasActiveAlert && incidentOpen && heldMs >= 1000 &&
         heldMs < 3000;
}

// Attention sounding matches ShoreRuntime::serviceBuzzer: silent when locally
// muted, resolved, or severity is zero. Acknowledged still sounds only if the
// mute flag was never set; acknowledge() and journal replay both set that flag.
bool shouldSoundAttentionBuzzer(const char* alertState, uint8_t severity,
                                bool locallySilenced);

// Incident identity is needed for in-boot de-duplication even when the event
// cannot be persisted. Durability controls Receipt, not local identity.
bool shouldBindIncidentIdentity(bool emergency, bool hasIncidentId,
                                bool hasNodeUid,
                                bool recordIsActive);

enum class EmergencyCheckpointRecovery : uint8_t {
  kRestoreLiveOnly,
  kAppendDurable,
  kPromoteLiveOnly,
};

// A checkpoint matching the exact in-memory live event is a durability
// promotion, not another emergency ingest. A reboot has no live snapshot and
// therefore replays the checkpoint normally.
EmergencyCheckpointRecovery decideEmergencyCheckpointRecovery(
    bool storageReady, bool hasLiveOnlySnapshot, bool incidentMatches,
    bool nonceMatches);

// Operator-state BLE commands carry a non-empty correlation ID. Sync uses its
// own sync_id, while time updates need no response correlation.
bool commandRequiresCommandId(const char* operation);
bool isValidCommandId(const char* commandId);

// BLE JSON record IDs share the firmware's full uint32_t range. In particular,
// live-only records intentionally use the high-bit namespace, so parsing them
// through a signed int default would turn valid IDs into zero.
uint32_t normalizeBleCommandRecordId(uint64_t raw, bool isNonNegativeInteger);

// Ack/Resolve are operator safety actions. Sync and time must never starve them
// out of the shore command queue.
bool isOperatorSafetyCommand(const char* operation);
bool isOperatorSafetyCommandJson(const char* json);

enum class CommandAdmission : uint8_t {
  kAdmit,
  kRefuseBusy,
};

// Non-operator work may fill only capacity - reservedForOperator slots. Operator
// commands may use the full capacity, including the reserved tail.
CommandAdmission decideCommandAdmission(bool isOperatorCommand,
                                        size_t currentDepth, size_t capacity,
                                        size_t reservedForOperator);

enum class CommandEnqueueTarget : uint8_t {
  kGeneral,
  kOperator,
  kBusy,
};

// Ack/resolve must share the proven general command queue with sync whenever
// that queue has space. The dedicated operator queue is overflow only, so a
// side queue that is never drained cannot swallow a phone Acknowledge.
CommandEnqueueTarget decideCommandEnqueueTarget(bool isOperator,
                                                bool generalHasSpace,
                                                bool operatorHasSpace,
                                                bool oversized);

// Best-effort extraction of a JSON string field, used so rejection paths can
// still echo command_id even when the full object fails to parse.
bool copyJsonStringField(const char* json, const char* key, char* out,
                         size_t outSize);

// SH1106 size-1 text fits ~21 characters on a 128-px row. These prompts stay
// within that budget and must not imply the button is mandatory while a phone
// is connected and able to act.
constexpr size_t kShoreDisplayPromptMaxChars = 21;
const char* operatorIncidentPrompt(bool acknowledged, bool phoneConnected);

enum class OutgoingKind : uint8_t {
  kOperatorResult,
  kSyncPayload,
  kOther,
};

OutgoingKind classifyOutgoingJson(const char* json);

enum class OutgoingAdmission : uint8_t {
  kEnqueueFront,
  kEnqueueTail,
  kRefuse,
};

// Operator command_result rows take the front of the notify queue. Sync rows
// enqueue at the tail and may be dropped under pressure so an operator result
// can still be admitted.
OutgoingAdmission decideOutgoingAdmission(OutgoingKind kind, size_t depth,
                                          size_t capacity);

bool mayDropOutgoingForOperator(OutgoingKind kind);

// A bonded reconnect can already be encrypted before NimBLE emits a fresh
// authentication-complete callback. Encrypted GATT activity may promote the
// runtime link only when it still resolves to the gateway's sole stored bond.
bool shouldPromoteBleAuthenticatedLink(bool encrypted, bool connectionBonded,
                                       bool addressBonded,
                                       bool identityAddressBonded,
                                       size_t bondCount);

struct StatusSnapshot {
  bool storageReady;
  uint32_t journalCount;
  const char* latestNodeUid;
  bool hasRadioMetrics;
  float rssi;
  float snr;
  uint32_t lastPacketAgeMs;
  const char* radioHealth;
  const char* alertState;
  uint32_t activeRecordId;
  const char* severity;
  bool pairingOpen;
};

// Status is a single GATT notification. This encoder always returns a valid
// payload within MTU 185's 182-byte attribute-value budget, dropping optional
// diagnostics if needed.
std::string encodeStatusPayload(const StatusSnapshot& status);

// Lexically scans raw JSON and rejects duplicate known radio keys at the top
// level. It intentionally permits unknown extension keys and nested keys.
bool hasUniqueRadioTopLevelKeys(const std::string& json);

struct BusyCommandResult {
  std::string commandId;
  std::string operation;
  int64_t recordId;
  bool hasRecordId;
};

// Encodes the best-effort response emitted by the main loop when the callback
// cannot admit a command because the normal command queue is full.
std::string encodeBusyCommandResult(const BusyCommandResult& result);

// Sync page failures that the phone can correlate via sync_id. Distinct from
// alert command_result rows, which key on command_id.
struct SyncFailureResult {
  std::string syncId;
  std::string error;
};

std::string encodeSyncFailureResult(const SyncFailureResult& result);

// Preferred ATT MTU is 185 (182-byte attribute value). Fragmentation stays at
// most 160 JSON bytes per SM chunk; lower negotiated MTUs shrink the chunk.
constexpr size_t kBleEventChunkMaxBytes = 160;
constexpr size_t kBleEventFrameHeaderBytes = 16;
constexpr uint32_t kBleNotifyGapMs = 18;
constexpr uint8_t kBleNotifyMaxFailures = 8;
constexpr size_t kSyncScanLinesPerService = 48;

size_t bleAttPayloadBytes(uint16_t mtu);
size_t bleEventUnframedLimit(size_t attPayloadBytes);
size_t bleEventChunkBytes(size_t attPayloadBytes);

enum class NotifyDeliveryAction : uint8_t {
  kAdvance,
  kRetryBackoff,
  kAbandon,
};

struct NotifyDeliveryDecision {
  NotifyDeliveryAction action;
  uint8_t nextFailCount;
  uint32_t delayMs;
};

// Pure notify-pump policy. A failed notify must either back off with a rising
// fail count or abandon so the queue cannot wedge forever. Operator results set
// protectFromAbandon so a committed transition is never silently discarded.
NotifyDeliveryDecision decideNotifyDelivery(bool notified, uint8_t failCount,
                                            bool protectFromAbandon = false);

// Correlated failure when an operator result cannot be queued at all. Distinct
// from a silent drop: the phone can match command_id and surface the error.
struct OperatorNotifyFailureResult {
  std::string commandId;
  std::string operation;
  int64_t recordId;
  bool hasRecordId;
  const char* source;
};

std::string encodeOperatorNotifyFailureResult(
    const OperatorNotifyFailureResult& result);

// Byte-offset resume for NDJSON sync. A hint is usable only when the request
// cursor is at or past the record the hint was taken after.
size_t syncJournalStartOffset(uint32_t requestAfter, uint32_t hintAfter,
                              size_t hintOffset);

enum class SyncRecordDisposition : uint8_t {
  kSkip,
  kAccept,
  kPageFull,
};

// Tracks one sync page while the runtime feeds it record IDs one line at a
// time. Yielding after kSyncScanLinesPerService lines is the caller's duty.
class SyncPageScanner {
 public:
  void begin(uint32_t after, uint8_t limit);
  SyncRecordDisposition observe(uint32_t recordId);
  bool shouldYield(size_t linesThisService) const;
  void noteMissingRange(uint32_t fromInclusive, uint32_t toInclusive);

  uint32_t after() const { return after_; }
  uint8_t limit() const { return limit_; }
  uint8_t sent() const { return sent_; }
  uint32_t last() const { return last_; }
  uint32_t expected() const { return expected_; }
  uint32_t firstReturned() const { return firstReturned_; }
  bool hasMore() const { return hasMore_; }
  bool hasGap() const { return hasGap_; }
  const uint32_t* missingRangeBounds() const { return missingRangeBounds_; }
  size_t missingRangeBoundCount() const { return missingRangeBoundCount_; }

 private:
  static constexpr size_t kMaxMissingBounds = 16;
  uint32_t after_ = 0;
  uint8_t limit_ = 0;
  uint8_t sent_ = 0;
  uint32_t last_ = 0;
  uint32_t expected_ = 0;
  uint32_t firstReturned_ = 0;
  bool hasMore_ = false;
  bool hasGap_ = false;
  uint32_t missingRangeBounds_[kMaxMissingBounds] = {};
  size_t missingRangeBoundCount_ = 0;
};

// Parser-independent representation of a signed boat uplink. The radio
// receiver maps missing or wrong-typed JSON values to unset optionals, then
// calls this pure protocol boundary before recording a nonce or journal event.
template <typename T>
class OptionalField {
 public:
  OptionalField() = default;

  OptionalField& operator=(const T& value) {
    value_ = value;
    present_ = true;
    return *this;
  }

  explicit operator bool() const { return present_; }
  const T& operator*() const { return value_; }
  void reset() { present_ = false; }

 private:
  bool present_ = false;
  T value_{};
};

struct RadioGpsFields {
  OptionalField<bool> valid;
  OptionalField<double> latitude;
  OptionalField<double> longitude;
  OptionalField<int64_t> ageSeconds;
};

struct BoatRadioUplink {
  OptionalField<std::string> nonce;
  OptionalField<std::string> nodeUid;
  bool incidentIdPresent = false;
  OptionalField<std::string> incidentId;
  OptionalField<std::string> status;
  RadioGpsFields gps;
  bool batteryPresent = false;
  OptionalField<int> batteryPercent;
};

// Validates the current packet semantics covered by the HMAC; callers retain
// responsibility for authenticating the exact wire bytes first and rejecting
// prohibited envelope keys.
bool validateTelemetryUplink(const BoatRadioUplink& packet);
bool validateIncidentUplink(const BoatRadioUplink& packet);

// Pure state reducer shared by journal replay and live LoRa ingest. The
// resulting action tells the durable runtime which transition records to emit.
class AlertReducer {
 public:
  enum Severity : uint8_t { kNone = 0, kHelp = 1, kSos = 2 };
  enum ActionKind : uint8_t { kNoChange, kOpened, kSuperseded, kAbsorbed };
  struct Action {
    ActionKind kind;
    uint32_t previousRecordId;
    Action(ActionKind actionKind = kNoChange, uint32_t previous = 0)
        : kind(actionKind), previousRecordId(previous) {}
  };

  Action ingest(uint32_t recordId, Severity severity);
  // Rebinds the current live-only alert to its durable journal record without
  // replaying the emergency as a new alert. Severity and operator state are
  // deliberately unchanged.
  bool promoteActiveRecord(uint32_t durableRecordId);
  void replayEvent(uint32_t recordId, Severity severity);
  void replayTransition(uint32_t targetRecordId, const char* state);
  bool canApplyOperatorAction(uint32_t recordId) const;
  uint32_t activeRecordId() const { return activeRecordId_; }
  Severity activeSeverity() const { return activeSeverity_; }
  const char* state() const { return state_; }

 private:
  uint32_t activeRecordId_ = 0;
  Severity activeSeverity_ = kNone;
  const char* state_ = "resolved";
};

// A recovery-time emergency must be interpreted against the reducer rebuilt
// from durable journal history. The pre-remount RAM snapshot identifies the
// checkpoint being promoted, but it is never authoritative over replay.
AlertReducer::Action decideRecoveredEmergency(
    const AlertReducer& replayedAlerts, uint32_t recordId,
    AlertReducer::Severity severity);

// Byte retention policy used by the LittleFS journal. It keeps the temporary
// replacement bounded independently of event-size variation.
class JournalWindow {
 public:
  struct Plan {
    bool needsCompaction;
    uint32_t bytesToDiscard;
    uint32_t targetBytes;
    Plan(bool needs = false, uint32_t discard = 0, uint32_t target = 0)
        : needsCompaction(needs), bytesToDiscard(discard), targetBytes(target) {}
  };

  JournalWindow(uint32_t highWaterBytes, uint32_t lowWaterBytes)
      : highWaterBytes_(highWaterBytes), lowWaterBytes_(lowWaterBytes) {}
  bool shouldCompact(uint32_t bytes) const { return bytes >= highWaterBytes_; }
  bool canStageReplacement(uint32_t freeBytes,
                           uint32_t reserveBytes) const {
    return freeBytes >= reserveBytes &&
           freeBytes - reserveBytes >= lowWaterBytes_;
  }
  Plan plan(uint32_t bytes) const;

 private:
  uint32_t highWaterBytes_;
  uint32_t lowWaterBytes_;
};

// Tracks the longest contiguous prefix consisting of complete, valid NDJSON
// records. Startup recovery rewrites only this prefix before appending again.
class JournalRecoveryPolicy {
 public:
  void observeLine(size_t bytes, bool newlineTerminated, bool validJsonObject);
  size_t retainedBytes() const { return retainedBytes_; }
  bool needsRecovery() const { return retainedBytes_ != scannedBytes_; }

 private:
  size_t scannedBytes_ = 0;
  size_t retainedBytes_ = 0;
  bool invalidTail_ = false;
};

}  // namespace coastcue
