#include "shore_logic.h"

#include "incident_protocol.h"

#include <cmath>
#include <cstring>
#include <stdio.h>

namespace coastcue {

bool isValidJournalEpoch(const char* epoch) {
  if (epoch == nullptr || std::strlen(epoch) != 24) return false;
  for (const char* cursor = epoch; *cursor != '\0'; ++cursor) {
    if (!((*cursor >= '0' && *cursor <= '9') ||
          (*cursor >= 'a' && *cursor <= 'f'))) {
      return false;
    }
  }
  return true;
}

JournalIdentityDecision decideJournalIdentity(const char* mirroredEpoch,
                                               const char* filesystemEpoch,
                                               bool journalHasHistory,
                                               bool mirrorIsProvisional) {
  const bool mirrorValid = isValidJournalEpoch(mirroredEpoch);
  const bool filesystemValid = isValidJournalEpoch(filesystemEpoch);
  if (mirrorValid && filesystemValid) {
    if (mirrorIsProvisional) {
      return JournalIdentityDecision::kAdoptFilesystem;
    }
    return std::strcmp(mirroredEpoch, filesystemEpoch) == 0
               ? JournalIdentityDecision::kUseMatching
               : JournalIdentityDecision::kReject;
  }
  if (filesystemValid) {
    return JournalIdentityDecision::kAdoptFilesystem;
  }
  if (journalHasHistory) return JournalIdentityDecision::kReject;
  if (mirrorValid) return JournalIdentityDecision::kSeedEmptyFilesystem;
  return JournalIdentityDecision::kCreateNew;
}

namespace {

class JsonTopLevelKeyScanner {
 public:
  explicit JsonTopLevelKeyScanner(const std::string& json) : json_(json) {}

  bool hasUniqueKnownKeys() {
    skipWhitespace();
    if (!scanObject(kTopLevelObject, 0)) return false;
    skipWhitespace();
    return position_ == json_.size();
  }

 private:
  enum ObjectScope { kOtherObject, kTopLevelObject, kGpsObject };

  void skipWhitespace() {
    while (position_ < json_.size() &&
           (json_[position_] == ' ' || json_[position_] == '\n' ||
            json_[position_] == '\r' || json_[position_] == '\t')) {
      ++position_;
    }
  }

  bool consume(char expected) {
    if (position_ >= json_.size() || json_[position_] != expected) return false;
    ++position_;
    return true;
  }

  static int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  }

  bool scanUnicodeEscape(std::string& output) {
    if (position_ + 4 > json_.size()) return false;
    unsigned codepoint = 0;
    for (size_t i = 0; i < 4; ++i) {
      const int nibble = hexValue(json_[position_++]);
      if (nibble < 0) return false;
      codepoint = (codepoint << 4) | static_cast<unsigned>(nibble);
    }
    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
      if (position_ + 6 > json_.size() || json_[position_] != '\\' ||
          json_[position_ + 1] != 'u') return false;
      position_ += 2;
      unsigned lowSurrogate = 0;
      for (size_t i = 0; i < 4; ++i) {
        const int nibble = hexValue(json_[position_++]);
        if (nibble < 0) return false;
        lowSurrogate = (lowSurrogate << 4) | static_cast<unsigned>(nibble);
      }
      if (lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF) return false;
      output += static_cast<char>(0x80);
      return true;
    }
    if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) return false;
    output += codepoint < 0x80 ? static_cast<char>(codepoint)
                               : static_cast<char>(0x80);
    return true;
  }

  bool scanString(std::string& output) {
    if (!consume('"')) return false;
    output.clear();
    while (position_ < json_.size()) {
      const unsigned char value = static_cast<unsigned char>(json_[position_++]);
      if (value == '"') return true;
      if (value < 0x20) return false;
      if (value != '\\') {
        output += static_cast<char>(value);
        continue;
      }
      if (position_ >= json_.size()) return false;
      switch (json_[position_++]) {
        case '"': output += '"'; break;
        case '\\': output += '\\'; break;
        case '/': output += '/'; break;
        case 'b': output += '\b'; break;
        case 'f': output += '\f'; break;
        case 'n': output += '\n'; break;
        case 'r': output += '\r'; break;
        case 't': output += '\t'; break;
        case 'u':
          if (!scanUnicodeEscape(output)) return false;
          break;
        default: return false;
      }
    }
    return false;
  }

  bool scanNumber() {
    if (position_ < json_.size() && json_[position_] == '-') ++position_;
    if (position_ >= json_.size()) return false;
    if (json_[position_] == '0') {
      ++position_;
    } else if (json_[position_] >= '1' && json_[position_] <= '9') {
      do {
        ++position_;
      } while (position_ < json_.size() && json_[position_] >= '0' &&
               json_[position_] <= '9');
    } else {
      return false;
    }
    if (position_ < json_.size() && json_[position_] == '.') {
      ++position_;
      const size_t fractionStart = position_;
      while (position_ < json_.size() && json_[position_] >= '0' &&
             json_[position_] <= '9') ++position_;
      if (fractionStart == position_) return false;
    }
    if (position_ < json_.size() &&
        (json_[position_] == 'e' || json_[position_] == 'E')) {
      ++position_;
      if (position_ < json_.size() &&
          (json_[position_] == '+' || json_[position_] == '-')) ++position_;
      const size_t exponentStart = position_;
      while (position_ < json_.size() && json_[position_] >= '0' &&
             json_[position_] <= '9') ++position_;
      if (exponentStart == position_) return false;
    }
    return true;
  }

  bool scanLiteral(const char* literal) {
    for (const char* cursor = literal; *cursor != '\0'; ++cursor) {
      if (position_ >= json_.size() || json_[position_++] != *cursor) return false;
    }
    return true;
  }

  bool scanArray(uint8_t depth) {
    if (!consume('[')) return false;
    skipWhitespace();
    if (consume(']')) return true;
    while (true) {
      if (!scanValue(depth + 1)) return false;
      skipWhitespace();
      if (consume(']')) return true;
      if (!consume(',')) return false;
      skipWhitespace();
    }
  }

  int knownKeyIndex(const std::string& key, ObjectScope scope) const {
    static const char* const kKnownKeys[] = {
        "nonce", "node_uid", "status", "gps", "battery_percent", "mac",
        "kind", "incident_id", "state", "state_seq", "target_node_uid", "i"};
    static const char* const kGpsKeys[] = {
        "valid", "latitude", "longitude", "age_s", "diag"};
    const char* const* keys = scope == kTopLevelObject ? kKnownKeys : kGpsKeys;
    const size_t count = scope == kTopLevelObject
                             ? sizeof(kKnownKeys) / sizeof(kKnownKeys[0])
                             : sizeof(kGpsKeys) / sizeof(kGpsKeys[0]);
    for (size_t i = 0; i < count; ++i) {
      if (key == keys[i]) return static_cast<int>(i);
    }
    return -1;
  }

  bool scanObject(ObjectScope scope, uint8_t depth) {
    if (depth > 32 || !consume('{')) return false;
    bool seen[14] = {false};
    skipWhitespace();
    if (consume('}')) return true;
    while (true) {
      std::string key;
      if (!scanString(key)) return false;
      if (scope != kOtherObject) {
        const int index = knownKeyIndex(key, scope);
        if (index >= 0 && seen[index]) return false;
        if (index >= 0) seen[index] = true;
      }
      skipWhitespace();
      if (!consume(':')) return false;
      skipWhitespace();
      if (scope == kTopLevelObject && key == "gps" &&
          position_ < json_.size() && json_[position_] == '{') {
        if (!scanObject(kGpsObject, depth + 1)) return false;
      } else if (!scanValue(depth + 1)) {
        return false;
      }
      skipWhitespace();
      if (consume('}')) return true;
      if (!consume(',')) return false;
      skipWhitespace();
    }
  }

  bool scanValue(uint8_t depth) {
    if (depth > 32 || position_ >= json_.size()) return false;
    switch (json_[position_]) {
      case '"': {
        std::string ignored;
        return scanString(ignored);
      }
      case '{': return scanObject(kOtherObject, depth);
      case '[': return scanArray(depth);
      case 't': return scanLiteral("true");
      case 'f': return scanLiteral("false");
      case 'n': return scanLiteral("null");
      default: return scanNumber();
    }
  }

  const std::string& json_;
  size_t position_ = 0;
};

void appendKey(std::string& json, const char* key) {
  if (json.size() > 1) json += ',';
  json += '"';
  json += key;
  json += "\":";
}

void appendEscapedString(std::string& json, const char* value) {
  json += '"';
  if (value != nullptr) {
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(value);
         *cursor != '\0'; ++cursor) {
      switch (*cursor) {
        case '"': json += "\\\""; break;
        case '\\': json += "\\\\"; break;
        case '\b': json += "\\b"; break;
        case '\f': json += "\\f"; break;
        case '\n': json += "\\n"; break;
        case '\r': json += "\\r"; break;
        case '\t': json += "\\t"; break;
        default:
          if (*cursor < 0x20) {
            char escaped[7];
            snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
            json += escaped;
          } else {
            json += static_cast<char>(*cursor);
          }
      }
    }
  }
  json += '"';
}

void appendString(std::string& json, const char* key, const char* value) {
  appendKey(json, key);
  appendEscapedString(json, value);
}

void appendBool(std::string& json, const char* key, bool value) {
  appendKey(json, key);
  json += value ? "true" : "false";
}

void appendUInt(std::string& json, const char* key, uint32_t value) {
  appendKey(json, key);
  char encoded[11];
  snprintf(encoded, sizeof(encoded), "%lu", static_cast<unsigned long>(value));
  json += encoded;
}

void appendInt64(std::string& json, const char* key, int64_t value) {
  appendKey(json, key);
  char encoded[22];
  snprintf(encoded, sizeof(encoded), "%lld", static_cast<long long>(value));
  json += encoded;
}

void appendFloat(std::string& json, const char* key, float value) {
  appendKey(json, key);
  char encoded[24];
  snprintf(encoded, sizeof(encoded), "%.1f", static_cast<double>(value));
  json += encoded;
}

std::string buildStatusPayload(const StatusSnapshot& status,
                               bool includeOptionalDiagnostics) {
  std::string json = "{";
  appendBool(json, "f", status.storageReady);
  appendUInt(json, "c", status.journalCount);
  if (includeOptionalDiagnostics && status.latestNodeUid != nullptr &&
      status.latestNodeUid[0] != '\0') {
    appendString(json, "n", status.latestNodeUid);
  }
  if (status.hasRadioMetrics) {
    appendUInt(json, "a", status.lastPacketAgeMs);
  }
  appendString(json, "h", status.radioHealth);
  if (includeOptionalDiagnostics && status.hasRadioMetrics) {
    appendFloat(json, "r", status.rssi);
    appendFloat(json, "q", status.snr);
  }
  appendString(json, "s", status.alertState);
  appendUInt(json, "i", status.activeRecordId);
  appendString(json, "e", status.severity);
  appendBool(json, "p", status.pairingOpen);
  json += '}';
  return json;
}

std::string buildCriticalStatusPayload(const StatusSnapshot& status) {
  std::string json = "{";
  appendBool(json, "f", status.storageReady);
  appendString(json, "s", status.alertState);
  appendUInt(json, "i", status.activeRecordId);
  appendString(json, "e", status.severity);
  appendBool(json, "p", status.pairingOpen);
  json += '}';
  return json;
}

}  // namespace

std::string encodeStatusPayload(const StatusSnapshot& status) {
  std::string payload = buildStatusPayload(status, true);
  if (payload.size() <= kBleStatusPayloadMaxBytes) return payload;
  payload = buildStatusPayload(status, false);
  if (payload.size() <= kBleStatusPayloadMaxBytes) return payload;
  return buildCriticalStatusPayload(status);
}

bool hasUniqueRadioTopLevelKeys(const std::string& json) {
  return JsonTopLevelKeyScanner(json).hasUniqueKnownKeys();
}

std::string encodeBusyCommandResult(const BusyCommandResult& result) {
  std::string json = "{";
  appendString(json, "kind", "command_result");
  if (!result.commandId.empty()) appendString(json, "command_id", result.commandId.c_str());
  if (!result.operation.empty()) appendString(json, "op", result.operation.c_str());
  if (result.hasRecordId) appendInt64(json, "record_id", result.recordId);
  appendString(json, "source", "ble");
  appendBool(json, "ok", false);
  appendString(json, "error", "busy");
  json += '}';
  return json;
}

std::string encodeSyncFailureResult(const SyncFailureResult& result) {
  std::string json = "{";
  appendString(json, "kind", "command_result");
  appendString(json, "op", "sync");
  if (!result.syncId.empty()) appendString(json, "sync_id", result.syncId.c_str());
  appendString(json, "source", "ble");
  appendBool(json, "ok", false);
  appendString(json, "error", result.error.empty() ? "sync_failed" : result.error.c_str());
  json += '}';
  return json;
}

size_t bleAttPayloadBytes(uint16_t mtu) {
  if (mtu < 23) return 20;
  return static_cast<size_t>(mtu) - 3;
}

size_t bleEventUnframedLimit(size_t attPayloadBytes) {
  return attPayloadBytes < kBleEventChunkMaxBytes ? attPayloadBytes
                                                  : kBleEventChunkMaxBytes;
}

size_t bleEventChunkBytes(size_t attPayloadBytes) {
  if (attPayloadBytes <= kBleEventFrameHeaderBytes) return 0;
  const size_t room = attPayloadBytes - kBleEventFrameHeaderBytes;
  return room < kBleEventChunkMaxBytes ? room : kBleEventChunkMaxBytes;
}

NotifyDeliveryDecision decideNotifyDelivery(bool notified, uint8_t failCount,
                                            bool protectFromAbandon) {
  if (notified) {
    return {NotifyDeliveryAction::kAdvance, 0, kBleNotifyGapMs};
  }
  const uint8_t nextFail =
      failCount >= kBleNotifyMaxFailures
          ? kBleNotifyMaxFailures
          : static_cast<uint8_t>(failCount + 1);
  if (nextFail >= kBleNotifyMaxFailures) {
    if (protectFromAbandon) {
      return {NotifyDeliveryAction::kRetryBackoff, kBleNotifyMaxFailures - 1,
              400};
    }
    return {NotifyDeliveryAction::kAbandon, nextFail, 0};
  }
  uint32_t delay = kBleNotifyGapMs;
  for (uint8_t step = 1; step < nextFail; ++step) {
    if (delay > 200) {
      delay = 400;
      break;
    }
    delay *= 2;
  }
  return {NotifyDeliveryAction::kRetryBackoff, nextFail, delay};
}

size_t syncJournalStartOffset(uint32_t requestAfter, uint32_t hintAfter,
                              size_t hintOffset) {
  if (hintOffset == 0) return 0;
  if (requestAfter < hintAfter) return 0;
  return hintOffset;
}

void SyncPageScanner::begin(uint32_t after, uint8_t limit) {
  after_ = after;
  limit_ = limit == 0 ? 1 : limit;
  sent_ = 0;
  last_ = after;
  expected_ = after + 1;
  firstReturned_ = 0;
  hasMore_ = false;
  hasGap_ = false;
  missingRangeBoundCount_ = 0;
}

bool SyncPageScanner::shouldYield(size_t linesThisService) const {
  return linesThisService >= kSyncScanLinesPerService;
}

void SyncPageScanner::noteMissingRange(uint32_t fromInclusive,
                                       uint32_t toInclusive) {
  if (missingRangeBoundCount_ + 2 > kMaxMissingBounds) return;
  missingRangeBounds_[missingRangeBoundCount_++] = fromInclusive;
  missingRangeBounds_[missingRangeBoundCount_++] = toInclusive;
  hasGap_ = true;
}

SyncRecordDisposition SyncPageScanner::observe(uint32_t recordId) {
  if (recordId == 0 || recordId <= after_) return SyncRecordDisposition::kSkip;
  if (sent_ >= limit_) {
    hasMore_ = last_ > after_;
    return SyncRecordDisposition::kPageFull;
  }
  if (recordId > expected_) {
    noteMissingRange(expected_, recordId - 1);
  }
  expected_ = recordId + 1;
  if (firstReturned_ == 0) firstReturned_ = recordId;
  last_ = recordId;
  ++sent_;
  return SyncRecordDisposition::kAccept;
}

namespace {

bool isLowercaseHexIdentity(const std::string& value) {
  if (value.size() != 16) return false;
  for (const char digit : value) {
    if (!((digit >= '0' && digit <= '9') ||
          (digit >= 'a' && digit <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool validateCommonBoatUplink(const BoatRadioUplink& packet) {
  if (!packet.nonce || !packet.nodeUid || !packet.status ||
      !packet.gps.valid) {
    return false;
  }
  if ((*packet.nodeUid).empty() || !isLowercaseHexIdentity(*packet.nonce)) {
    return false;
  }
  if (*packet.gps.valid) {
    if (!packet.gps.latitude || !packet.gps.longitude ||
        !std::isfinite(*packet.gps.latitude) ||
        !std::isfinite(*packet.gps.longitude) ||
        *packet.gps.latitude < -90.0 || *packet.gps.latitude > 90.0 ||
        *packet.gps.longitude < -180.0 || *packet.gps.longitude > 180.0) {
      return false;
    }
  }
  if (packet.gps.ageSeconds &&
      (*packet.gps.ageSeconds < 0 || *packet.gps.ageSeconds > 4294967)) {
    return false;
  }
  return !packet.batteryPresent ||
         (packet.batteryPercent && *packet.batteryPercent >= 0 &&
          *packet.batteryPercent <= 100);
}

}  // namespace

bool validateTelemetryUplink(const BoatRadioUplink& packet) {
  return validateCommonBoatUplink(packet) && !packet.incidentIdPresent &&
         *packet.status == "normal";
}

bool validateIncidentUplink(const BoatRadioUplink& packet) {
  if (!validateCommonBoatUplink(packet) || !packet.incidentIdPresent ||
      !packet.incidentId ||
      (*packet.status != "help" && *packet.status != "sos")) {
    return false;
  }
  const IncidentUplink incident{(*packet.incidentId).c_str(),
                                IncidentState::kOpen, true};
  return isValidIncidentUplink(incident);
}

bool commandRequiresCommandId(const char* operation) {
  return operation != nullptr &&
         (std::strcmp(operation, "ack") == 0 ||
          std::strcmp(operation, "resolve") == 0);
}

bool isValidCommandId(const char* commandId) {
  return commandId != nullptr && commandId[0] != '\0';
}

uint32_t normalizeBleCommandRecordId(uint64_t raw,
                                     bool isNonNegativeInteger) {
  if (!isNonNegativeInteger || raw == 0 || raw > UINT32_MAX) return 0;
  return static_cast<uint32_t>(raw);
}

bool isOperatorSafetyCommand(const char* operation) {
  return commandRequiresCommandId(operation);
}

namespace {

const char* extractJsonStringValue(const char* json, const char* key) {
  if (json == nullptr || key == nullptr || key[0] == '\0') return nullptr;
  const size_t keyLen = std::strlen(key);
  const char* cursor = json;
  while ((cursor = std::strstr(cursor, key)) != nullptr) {
    if (cursor > json) {
      const char before = cursor[-1];
      if (before != '"' && before != '{' && before != ',' && before != ' ' &&
          before != '\n' && before != '\r' && before != '\t') {
        cursor += keyLen;
        continue;
      }
    }
    const char* afterKey = cursor + keyLen;
    if (*afterKey != '"') {
      cursor += keyLen;
      continue;
    }
    ++afterKey;
    while (*afterKey == ' ' || *afterKey == '\t' || *afterKey == '\n' ||
           *afterKey == '\r') {
      ++afterKey;
    }
    if (*afterKey != ':') {
      cursor += keyLen;
      continue;
    }
    ++afterKey;
    while (*afterKey == ' ' || *afterKey == '\t' || *afterKey == '\n' ||
           *afterKey == '\r') {
      ++afterKey;
    }
    if (*afterKey != '"') return nullptr;
    return afterKey + 1;
  }
  return nullptr;
}

bool jsonStringValueEquals(const char* valueStart, const char* expected) {
  if (valueStart == nullptr || expected == nullptr) return false;
  const size_t expectedLen = std::strlen(expected);
  return std::strncmp(valueStart, expected, expectedLen) == 0 &&
         valueStart[expectedLen] == '"';
}

bool jsonHasKind(const char* json, const char* kind) {
  return jsonStringValueEquals(extractJsonStringValue(json, "kind"), kind);
}

}  // namespace

bool isOperatorSafetyCommandJson(const char* json) {
  const char* op = extractJsonStringValue(json, "op");
  return jsonStringValueEquals(op, "ack") ||
         jsonStringValueEquals(op, "resolve");
}

bool shouldSoundAttentionBuzzer(const char* alertState, uint8_t severity,
                                bool locallySilenced) {
  if (locallySilenced || severity == 0) return false;
  return alertState == nullptr || std::strcmp(alertState, "resolved") != 0;
}

CommandEnqueueTarget decideCommandEnqueueTarget(bool isOperator,
                                                bool generalHasSpace,
                                                bool operatorHasSpace,
                                                bool oversized) {
  if (oversized) return CommandEnqueueTarget::kBusy;
  if (generalHasSpace) return CommandEnqueueTarget::kGeneral;
  if (isOperator && operatorHasSpace) return CommandEnqueueTarget::kOperator;
  return CommandEnqueueTarget::kBusy;
}

bool copyJsonStringField(const char* json, const char* key, char* out,
                         size_t outSize) {
  if (out == nullptr || outSize == 0) return false;
  out[0] = '\0';
  const char* value = extractJsonStringValue(json, key);
  if (value == nullptr) return false;
  size_t n = 0;
  while (value[n] != '\0' && value[n] != '"' && n + 1 < outSize) {
    if (value[n] == '\\') return false;
    ++n;
  }
  if (value[n] != '"') return false;
  std::memcpy(out, value, n);
  out[n] = '\0';
  return n > 0;
}

CommandAdmission decideCommandAdmission(bool isOperatorCommand,
                                        size_t currentDepth, size_t capacity,
                                        size_t reservedForOperator) {
  if (capacity == 0 || currentDepth >= capacity) {
    return CommandAdmission::kRefuseBusy;
  }
  const size_t reserved =
      reservedForOperator > capacity ? capacity : reservedForOperator;
  const size_t nonOperatorLimit = capacity - reserved;
  if (isOperatorCommand) return CommandAdmission::kAdmit;
  if (currentDepth >= nonOperatorLimit) return CommandAdmission::kRefuseBusy;
  return CommandAdmission::kAdmit;
}

const char* operatorIncidentPrompt(bool acknowledged, bool phoneConnected) {
  if (phoneConnected) {
    return acknowledged ? "Phone/btn: done" : "Phone/btn: ack";
  }
  return acknowledged ? "Double-click: done" : "Press once: ack";
}

OutgoingKind classifyOutgoingJson(const char* json) {
  if (json == nullptr) return OutgoingKind::kOther;
  if (jsonHasKind(json, "command_result")) {
    const char* op = extractJsonStringValue(json, "op");
    if (jsonStringValueEquals(op, "ack") ||
        jsonStringValueEquals(op, "resolve")) {
      return OutgoingKind::kOperatorResult;
    }
  }
  if (jsonHasKind(json, "sync_complete")) return OutgoingKind::kSyncPayload;
  if (std::strstr(json, "\"sync_id\"") != nullptr) {
    return OutgoingKind::kSyncPayload;
  }
  return OutgoingKind::kOther;
}

OutgoingAdmission decideOutgoingAdmission(OutgoingKind kind, size_t depth,
                                          size_t capacity) {
  if (kind == OutgoingKind::kOperatorResult) {
    return OutgoingAdmission::kEnqueueFront;
  }
  if (depth >= capacity) return OutgoingAdmission::kRefuse;
  return OutgoingAdmission::kEnqueueTail;
}

bool mayDropOutgoingForOperator(OutgoingKind kind) {
  return kind == OutgoingKind::kSyncPayload;
}

bool shouldPromoteBleAuthenticatedLink(bool encrypted, bool connectionBonded,
                                       bool addressBonded,
                                       bool identityAddressBonded,
                                       size_t bondCount) {
  return encrypted && bondCount == 1 &&
         (connectionBonded || addressBonded || identityAddressBonded);
}

std::string encodeOperatorNotifyFailureResult(
    const OperatorNotifyFailureResult& result) {
  std::string json = "{";
  appendString(json, "kind", "command_result");
  if (!result.commandId.empty()) {
    appendString(json, "command_id", result.commandId.c_str());
  }
  if (!result.operation.empty()) {
    appendString(json, "op", result.operation.c_str());
  }
  if (result.hasRecordId) appendInt64(json, "record_id", result.recordId);
  if (result.source != nullptr && result.source[0] != '\0') {
    appendString(json, "source", result.source);
  }
  appendBool(json, "ok", false);
  appendString(json, "error", "notify_failed");
  json += '}';
  return json;
}

AlertReducer::Action AlertReducer::ingest(uint32_t recordId, Severity severity) {
  if (severity == kNone || recordId == 0) return {};
  if (state_[0] == 'r' || activeRecordId_ == 0) {
    activeRecordId_ = recordId;
    activeSeverity_ = severity;
    state_ = "open";
    return {kOpened, 0};
  }
  if (severity >= activeSeverity_) {
    const uint32_t previous = activeRecordId_;
    activeRecordId_ = recordId;
    activeSeverity_ = severity;
    state_ = "open";
    return {kSuperseded, previous};
  }
  // A Help arriving while SOS is active must be represented as resolved so
  // clients never create a second, unresolvable alert. It must not reopen or
  // re-sound an acknowledged SOS; the active Incident's operator state wins.
  return {kAbsorbed, recordId};
}

bool AlertReducer::promoteActiveRecord(uint32_t durableRecordId) {
  if (durableRecordId == 0 || activeRecordId_ == 0) return false;
  activeRecordId_ = durableRecordId;
  return true;
}

AlertReducer::Action decideRecoveredEmergency(
    const AlertReducer& replayedAlerts, uint32_t recordId,
    AlertReducer::Severity severity) {
  AlertReducer preview = replayedAlerts;
  return preview.ingest(recordId, severity);
}

void AlertReducer::replayEvent(uint32_t recordId, Severity severity) {
  if (severity == kNone || recordId == 0) return;
  if (activeRecordId_ == 0 || state_[0] == 'r' || severity >= activeSeverity_) {
    activeRecordId_ = recordId;
    activeSeverity_ = severity;
    state_ = "open";
  }
}

void AlertReducer::replayTransition(uint32_t targetRecordId, const char* state) {
  if (targetRecordId == 0 || targetRecordId != activeRecordId_ || state == nullptr) return;
  if (strcmp(state, "acknowledged") == 0) state_ = "acknowledged";
  if (strcmp(state, "resolved") == 0) {
    state_ = "resolved";
    activeSeverity_ = kNone;
  }
}

bool AlertReducer::canApplyOperatorAction(uint32_t recordId) const {
  return activeRecordId_ != 0 &&
         (recordId == 0 || recordId == activeRecordId_) &&
         state_[0] != 'r';
}

JournalWindow::Plan JournalWindow::plan(uint32_t bytes) const {
  if (!shouldCompact(bytes)) return {};
  return {true, bytes - lowWaterBytes_, lowWaterBytes_};
}

void JournalRecoveryPolicy::observeLine(size_t bytes, bool newlineTerminated,
                                        bool validJsonObject) {
  scannedBytes_ += bytes;
  if (invalidTail_) return;
  if (!newlineTerminated || !validJsonObject) {
    invalidTail_ = true;
    return;
  }
  retainedBytes_ = scannedBytes_;
}

}  // namespace coastcue

namespace coastcue {

bool isRadioFresh(bool hasReceivedPacket, uint32_t nowMs,
                  uint32_t lastPacketAtMs, uint32_t maxAgeMs) {
  return hasReceivedPacket &&
         static_cast<uint32_t>(nowMs - lastPacketAtMs) < maxAgeMs;
}

bool shouldCheckpointVolatileEmergency(const char* status, bool persisted) {
  if (persisted || status == nullptr) return false;
  return std::string(status) == "help" || std::string(status) == "sos";
}

bool mayApplyOperatorTransition(bool changed, bool transitionPersisted) {
  return !changed || transitionPersisted;
}

bool shouldBindIncidentIdentity(bool emergency, bool hasIncidentId,
                                bool hasNodeUid,
                                bool recordIsActive) {
  return emergency && hasIncidentId && hasNodeUid && recordIsActive;
}

EmergencyCheckpointRecovery decideEmergencyCheckpointRecovery(
    bool storageReady, bool hasLiveOnlySnapshot, bool incidentMatches,
    bool nonceMatches) {
  if (!storageReady) return EmergencyCheckpointRecovery::kRestoreLiveOnly;
  if (hasLiveOnlySnapshot && incidentMatches && nonceMatches) {
    return EmergencyCheckpointRecovery::kPromoteLiveOnly;
  }
  return EmergencyCheckpointRecovery::kAppendDurable;
}

}  // namespace coastcue
