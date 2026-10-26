#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace coastcue {

enum class GpsProtocol : uint8_t {
  kUnknown,
  kNmea,
  kUbx,
  kMixed,
};

enum class GpsDiagnosticPhase : uint8_t {
  kScan,
  kQuiet,
  kActive,
  kConfirm,
  kDone,
};

constexpr uint32_t GPS_DIAGNOSTIC_QUIET_MS = 3UL * 60UL * 1000UL;
constexpr uint32_t GPS_DIAGNOSTIC_ACTIVE_MS = 3UL * 60UL * 1000UL;
constexpr uint32_t GPS_DIAGNOSTIC_CONFIRM_MS = 4UL * 60UL * 1000UL;

inline GpsDiagnosticPhase gpsDiagnosticPhaseForElapsed(uint32_t elapsedMs) {
  if (elapsedMs < GPS_DIAGNOSTIC_QUIET_MS) {
    return GpsDiagnosticPhase::kQuiet;
  }
  if (elapsedMs < GPS_DIAGNOSTIC_QUIET_MS + GPS_DIAGNOSTIC_ACTIVE_MS) {
    return GpsDiagnosticPhase::kActive;
  }
  if (elapsedMs < GPS_DIAGNOSTIC_QUIET_MS + GPS_DIAGNOSTIC_ACTIVE_MS +
                      GPS_DIAGNOSTIC_CONFIRM_MS) {
    return GpsDiagnosticPhase::kConfirm;
  }
  return GpsDiagnosticPhase::kDone;
}

inline const char *gpsDiagnosticPhaseName(GpsDiagnosticPhase phase) {
  switch (phase) {
    case GpsDiagnosticPhase::kScan: return "scan";
    case GpsDiagnosticPhase::kQuiet: return "quiet";
    case GpsDiagnosticPhase::kActive: return "active";
    case GpsDiagnosticPhase::kConfirm: return "confirm";
    case GpsDiagnosticPhase::kDone: return "done";
  }
  return "unknown";
}

inline char gpsDiagnosticPhaseCode(GpsDiagnosticPhase phase) {
  switch (phase) {
    case GpsDiagnosticPhase::kScan: return 'S';
    case GpsDiagnosticPhase::kQuiet: return 'Q';
    case GpsDiagnosticPhase::kActive: return 'A';
    case GpsDiagnosticPhase::kConfirm: return 'C';
    case GpsDiagnosticPhase::kDone: return 'D';
  }
  return 'X';
}

inline const char *gpsProtocolName(GpsProtocol protocol) {
  switch (protocol) {
    case GpsProtocol::kNmea: return "NMEA";
    case GpsProtocol::kUbx: return "UBX";
    case GpsProtocol::kMixed: return "MIXED";
    case GpsProtocol::kUnknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

inline char gpsProtocolCode(GpsProtocol protocol) {
  switch (protocol) {
    case GpsProtocol::kNmea: return 'N';
    case GpsProtocol::kUbx: return 'U';
    case GpsProtocol::kMixed: return 'M';
    case GpsProtocol::kUnknown: return 'X';
  }
  return 'X';
}

struct GpsDiagnosticSnapshot {
  uint32_t bytes = 0;
  uint32_t nmeaPassed = 0;
  uint32_t nmeaFailed = 0;
  uint32_t ubxPassed = 0;
  uint32_t ubxFailed = 0;
  uint32_t unknownBytes = 0;
  uint32_t ggaSentences = 0;
  uint32_t gsaSentences = 0;
  uint32_t gsvSentences = 0;
  uint32_t rmcSentences = 0;
  uint32_t maxByteGapMs = 0;
  int visibleSatellites = -1;
  int minVisibleSatellites = -1;
  int usedSatellites = -1;
  int maxCn0DbHz = -1;
  int minGsvMaxCn0DbHz = -1;
  int fixQuality = -1;
  int fixType = -1;
  int hdopHundredths = -1;
  char rmcStatus = '?';
  GpsProtocol protocol = GpsProtocol::kUnknown;
};

class GpsStreamDiagnostics {
 public:
  void reset() {
    snapshot_ = GpsDiagnosticSnapshot{};
    nmeaLength_ = 0;
    collectingNmea_ = false;
    ubxState_ = UbxState::kSync1;
    ubxPayloadLength_ = 0;
    ubxPayloadRead_ = 0;
    ubxChecksumA_ = 0;
    ubxChecksumB_ = 0;
    ubxReceivedChecksumA_ = 0;
    lastByteAt_ = 0;
    lastNmea_[0] = '\0';
  }

  void feed(uint8_t byte, uint32_t nowMs) {
    ++snapshot_.bytes;
    if (lastByteAt_ != 0) {
      const uint32_t gap = nowMs - lastByteAt_;
      if (gap > snapshot_.maxByteGapMs) snapshot_.maxByteGapMs = gap;
    }
    lastByteAt_ = nowMs;

    if (collectingNmea_) {
      consumeNmea(byte);
      return;
    }
    if (ubxState_ != UbxState::kSync1) {
      consumeUbx(byte);
      return;
    }
    if (byte == '$') {
      collectingNmea_ = true;
      nmeaLength_ = 0;
      nmeaLine_[nmeaLength_++] = '$';
      return;
    }
    if (byte == 0xB5) {
      ubxState_ = UbxState::kSync2;
      return;
    }
    if (byte != '\r' && byte != '\n') ++snapshot_.unknownBytes;
  }

  GpsDiagnosticSnapshot snapshot(uint32_t nowMs) const {
    GpsDiagnosticSnapshot result = snapshot_;
    if (lastByteAt_ != 0) {
      const uint32_t currentGap = nowMs - lastByteAt_;
      if (currentGap > result.maxByteGapMs) result.maxByteGapMs = currentGap;
    }
    if (result.nmeaPassed > 0 && result.ubxPassed > 0) {
      result.protocol = GpsProtocol::kMixed;
    } else if (result.nmeaPassed > 0) {
      result.protocol = GpsProtocol::kNmea;
    } else if (result.ubxPassed > 0) {
      result.protocol = GpsProtocol::kUbx;
    }
    return result;
  }

  const char *lastNmea() const { return lastNmea_; }

 private:
  static constexpr size_t NMEA_LINE_BYTES = 128;
  static constexpr uint16_t MAX_UBX_PAYLOAD_BYTES = 1024;

  enum class UbxState : uint8_t {
    kSync1,
    kSync2,
    kClass,
    kId,
    kLengthLow,
    kLengthHigh,
    kPayload,
    kChecksumA,
    kChecksumB,
  };

  static int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
  }

  static int integerField(const char *value) {
    if (value == nullptr || *value == '\0') return -1;
    return std::atoi(value);
  }

  static int hundredthsField(const char *value) {
    if (value == nullptr || *value == '\0') return -1;
    const double parsed = std::strtod(value, nullptr);
    return static_cast<int>(parsed * 100.0 + 0.5);
  }

  static bool sentenceTypeIs(const char *identifier, const char *suffix) {
    const size_t length = std::strlen(identifier);
    return length >= 3 && std::strcmp(identifier + length - 3, suffix) == 0;
  }

  void consumeNmea(uint8_t byte) {
    if (byte == '$') {
      nmeaLength_ = 0;
      nmeaLine_[nmeaLength_++] = '$';
      return;
    }
    if (nmeaLength_ + 1 >= NMEA_LINE_BYTES) {
      ++snapshot_.nmeaFailed;
      collectingNmea_ = false;
      nmeaLength_ = 0;
      return;
    }
    if (byte != '\r' && byte != '\n') {
      nmeaLine_[nmeaLength_++] = static_cast<char>(byte);
    }
    if (byte == '\n') {
      nmeaLine_[nmeaLength_] = '\0';
      parseNmeaLine();
      collectingNmea_ = false;
      nmeaLength_ = 0;
    }
  }

  void parseNmeaLine() {
    char *asterisk = std::strrchr(nmeaLine_, '*');
    if (asterisk == nullptr || asterisk - nmeaLine_ < 2 ||
        std::strlen(asterisk + 1) < 2) {
      ++snapshot_.nmeaFailed;
      return;
    }
    uint8_t checksum = 0;
    for (char *cursor = nmeaLine_ + 1; cursor < asterisk; ++cursor) {
      checksum ^= static_cast<uint8_t>(*cursor);
    }
    const int high = hexValue(asterisk[1]);
    const int low = hexValue(asterisk[2]);
    if (high < 0 || low < 0 || checksum != ((high << 4) | low)) {
      ++snapshot_.nmeaFailed;
      return;
    }

    ++snapshot_.nmeaPassed;
    std::strncpy(lastNmea_, nmeaLine_, sizeof(lastNmea_) - 1);
    lastNmea_[sizeof(lastNmea_) - 1] = '\0';
    *asterisk = '\0';

    char *fields[24] = {nullptr};
    size_t fieldCount = 0;
    char *field = nmeaLine_ + 1;
    fields[fieldCount++] = field;
    for (char *cursor = field; *cursor != '\0' && fieldCount < 24; ++cursor) {
      if (*cursor == ',') {
        *cursor = '\0';
        fields[fieldCount++] = cursor + 1;
      }
    }
    if (fieldCount == 0) return;

    if (sentenceTypeIs(fields[0], "GSV")) {
      ++snapshot_.gsvSentences;
      int sentenceMaxCn0 = -1;
      if (fieldCount > 3) {
        const int visible = integerField(fields[3]);
        if (visible >= 0) {
          snapshot_.visibleSatellites = visible;
          if (snapshot_.minVisibleSatellites < 0 ||
              visible < snapshot_.minVisibleSatellites) {
            snapshot_.minVisibleSatellites = visible;
          }
        }
      }
      for (size_t index = 7; index < fieldCount; index += 4) {
        const int cn0 = integerField(fields[index]);
        if (cn0 > snapshot_.maxCn0DbHz) snapshot_.maxCn0DbHz = cn0;
        if (cn0 > sentenceMaxCn0) sentenceMaxCn0 = cn0;
      }
      if (sentenceMaxCn0 >= 0 &&
          (snapshot_.minGsvMaxCn0DbHz < 0 ||
           sentenceMaxCn0 < snapshot_.minGsvMaxCn0DbHz)) {
        snapshot_.minGsvMaxCn0DbHz = sentenceMaxCn0;
      }
    } else if (sentenceTypeIs(fields[0], "GGA")) {
      ++snapshot_.ggaSentences;
      if (fieldCount > 6) snapshot_.fixQuality = integerField(fields[6]);
      if (fieldCount > 7) snapshot_.usedSatellites = integerField(fields[7]);
      if (fieldCount > 8) snapshot_.hdopHundredths = hundredthsField(fields[8]);
    } else if (sentenceTypeIs(fields[0], "GSA")) {
      ++snapshot_.gsaSentences;
      if (fieldCount > 2) snapshot_.fixType = integerField(fields[2]);
    } else if (sentenceTypeIs(fields[0], "RMC")) {
      ++snapshot_.rmcSentences;
      if (fieldCount > 2 && fields[2][0] != '\0') snapshot_.rmcStatus = fields[2][0];
    }
  }

  void updateUbxChecksum(uint8_t byte) {
    ubxChecksumA_ = static_cast<uint8_t>(ubxChecksumA_ + byte);
    ubxChecksumB_ = static_cast<uint8_t>(ubxChecksumB_ + ubxChecksumA_);
  }

  void resetUbx() {
    ubxState_ = UbxState::kSync1;
    ubxPayloadLength_ = 0;
    ubxPayloadRead_ = 0;
    ubxChecksumA_ = 0;
    ubxChecksumB_ = 0;
    ubxReceivedChecksumA_ = 0;
  }

  void consumeUbx(uint8_t byte) {
    switch (ubxState_) {
      case UbxState::kSync1:
        return;
      case UbxState::kSync2:
        if (byte == 0x62) {
          ubxChecksumA_ = 0;
          ubxChecksumB_ = 0;
          ubxState_ = UbxState::kClass;
        } else {
          // Count the failed 0xB5 prefix, then treat this byte as a possible
          // start of the next frame instead of discarding it.
          ++snapshot_.unknownBytes;
          resetUbx();
          if (byte == '$') {
            collectingNmea_ = true;
            nmeaLength_ = 0;
            nmeaLine_[nmeaLength_++] = '$';
          } else if (byte == 0xB5) {
            ubxState_ = UbxState::kSync2;
          } else if (byte != '\r' && byte != '\n') {
            ++snapshot_.unknownBytes;
          }
        }
        return;
      case UbxState::kClass:
        updateUbxChecksum(byte);
        ubxState_ = UbxState::kId;
        return;
      case UbxState::kId:
        updateUbxChecksum(byte);
        ubxState_ = UbxState::kLengthLow;
        return;
      case UbxState::kLengthLow:
        ubxPayloadLength_ = byte;
        updateUbxChecksum(byte);
        ubxState_ = UbxState::kLengthHigh;
        return;
      case UbxState::kLengthHigh:
        ubxPayloadLength_ |= static_cast<uint16_t>(byte) << 8;
        updateUbxChecksum(byte);
        if (ubxPayloadLength_ > MAX_UBX_PAYLOAD_BYTES) {
          ++snapshot_.ubxFailed;
          resetUbx();
        } else {
          ubxPayloadRead_ = 0;
          ubxState_ = ubxPayloadLength_ == 0 ? UbxState::kChecksumA
                                             : UbxState::kPayload;
        }
        return;
      case UbxState::kPayload:
        updateUbxChecksum(byte);
        if (++ubxPayloadRead_ >= ubxPayloadLength_) {
          ubxState_ = UbxState::kChecksumA;
        }
        return;
      case UbxState::kChecksumA:
        ubxReceivedChecksumA_ = byte;
        ubxState_ = UbxState::kChecksumB;
        return;
      case UbxState::kChecksumB:
        if (ubxReceivedChecksumA_ == ubxChecksumA_ && byte == ubxChecksumB_) {
          ++snapshot_.ubxPassed;
        } else {
          ++snapshot_.ubxFailed;
        }
        resetUbx();
        return;
    }
  }

  GpsDiagnosticSnapshot snapshot_{};
  char nmeaLine_[NMEA_LINE_BYTES] = {0};
  char lastNmea_[NMEA_LINE_BYTES] = {0};
  size_t nmeaLength_ = 0;
  bool collectingNmea_ = false;
  UbxState ubxState_ = UbxState::kSync1;
  uint16_t ubxPayloadLength_ = 0;
  uint16_t ubxPayloadRead_ = 0;
  uint8_t ubxChecksumA_ = 0;
  uint8_t ubxChecksumB_ = 0;
  uint8_t ubxReceivedChecksumA_ = 0;
  uint32_t lastByteAt_ = 0;
};

inline bool gpsSnapshotHasFix(const GpsDiagnosticSnapshot &snapshot) {
  return snapshot.fixQuality > 0 || snapshot.fixType >= 2 ||
         snapshot.rmcStatus == 'A';
}

inline const char *classifyGpsDiagnostic(
    const GpsDiagnosticSnapshot &quiet,
    const GpsDiagnosticSnapshot &active,
    const GpsDiagnosticSnapshot &confirm,
    bool gpsRailEnabled,
    int gpsRailConfiguredMv) {
  if (!gpsRailEnabled || gpsRailConfiguredMv < 3000) {
    return "GPS_POWER_RAIL_CONFIG";
  }

  const uint32_t totalBytes = quiet.bytes + active.bytes + confirm.bytes;
  const uint32_t totalNmea =
      quiet.nmeaPassed + active.nmeaPassed + confirm.nmeaPassed;
  const uint32_t totalNmeaFailed =
      quiet.nmeaFailed + active.nmeaFailed + confirm.nmeaFailed;
  const uint32_t totalUbx = quiet.ubxPassed + active.ubxPassed + confirm.ubxPassed;
  const uint32_t totalGsv =
      quiet.gsvSentences + active.gsvSentences + confirm.gsvSentences;

  if (totalNmea == 0) {
    if (totalUbx > 0) return "UBX_ONLY_OR_NMEA_DISABLED";
    if (totalBytes < 10) return "NO_GPS_UART_DATA";
    return "GPS_PROTOCOL_OR_BAUD";
  }
  if (totalNmeaFailed > totalNmea) return "NMEA_CORRUPT_OR_UART_NOISE";

  const bool quietFix = gpsSnapshotHasFix(quiet);
  const bool quietStableSatelliteReception =
      quiet.minVisibleSatellites >= 3;
  const bool quietStableSignal = quiet.minGsvMaxCn0DbHz >= 20;
  const bool quietReceptionBaseline =
      quietFix || quietStableSatelliteReception || quietStableSignal;
  const bool activeReceptionCollapsed =
      (quietFix && !gpsSnapshotHasFix(active)) ||
      (quietStableSatelliteReception &&
       active.minVisibleSatellites >= 0 &&
       quiet.minVisibleSatellites - active.minVisibleSatellites >= 3) ||
      (quietStableSignal && active.minGsvMaxCn0DbHz >= 0 &&
       quiet.minGsvMaxCn0DbHz - active.minGsvMaxCn0DbHz >= 10) ||
      active.nmeaFailed > quiet.nmeaFailed + 5;
  if (quietReceptionBaseline && activeReceptionCollapsed) {
    return "LORA_POWER_OR_EMI";
  }

  if (gpsSnapshotHasFix(quiet) || gpsSnapshotHasFix(active) ||
      gpsSnapshotHasFix(confirm)) {
    return "FIX_OK";
  }
  if (totalGsv == 0) return "GSV_DISABLED_OR_GPS_CONFIG";

  const int maxVisible = quiet.visibleSatellites > active.visibleSatellites
                             ? (quiet.visibleSatellites > confirm.visibleSatellites
                                    ? quiet.visibleSatellites
                                    : confirm.visibleSatellites)
                             : (active.visibleSatellites > confirm.visibleSatellites
                                    ? active.visibleSatellites
                                    : confirm.visibleSatellites);
  if (maxVisible <= 0) return "GPS_ANTENNA_OR_RF_PATH";

  const int maxCn0 = quiet.maxCn0DbHz > active.maxCn0DbHz
                         ? (quiet.maxCn0DbHz > confirm.maxCn0DbHz
                                ? quiet.maxCn0DbHz
                                : confirm.maxCn0DbHz)
                         : (active.maxCn0DbHz > confirm.maxCn0DbHz
                                ? active.maxCn0DbHz
                                : confirm.maxCn0DbHz);
  if (maxCn0 < 20) return "WEAK_GPS_SIGNAL";
  return "SATELLITES_VISIBLE_NO_FIX";
}

inline char gpsDiagnosticClassificationCode(const char *classification) {
  if (classification == nullptr || std::strcmp(classification, "PENDING") == 0) return 'P';
  if (std::strcmp(classification, "FIX_OK") == 0) return 'F';
  if (std::strcmp(classification, "LORA_POWER_OR_EMI") == 0) return 'E';
  if (std::strcmp(classification, "WEAK_GPS_SIGNAL") == 0) return 'W';
  if (std::strcmp(classification, "SATELLITES_VISIBLE_NO_FIX") == 0) return 'S';
  if (std::strcmp(classification, "GPS_ANTENNA_OR_RF_PATH") == 0) return 'A';
  if (std::strcmp(classification, "GSV_DISABLED_OR_GPS_CONFIG") == 0) return 'G';
  if (std::strcmp(classification, "NO_GPS_UART_DATA") == 0) return 'N';
  if (std::strcmp(classification, "GPS_PROTOCOL_OR_BAUD") == 0) return 'B';
  if (std::strcmp(classification, "UBX_ONLY_OR_NMEA_DISABLED") == 0) return 'U';
  if (std::strcmp(classification, "NMEA_CORRUPT_OR_UART_NOISE") == 0) return 'C';
  if (std::strcmp(classification, "GPS_POWER_RAIL_CONFIG") == 0) return 'V';
  return 'X';
}

inline const char *gpsDiagnosticClassificationName(char code) {
  switch (code) {
    case 'P': return "PENDING";
    case 'F': return "FIX_OK";
    case 'E': return "LORA_POWER_OR_EMI";
    case 'W': return "WEAK_GPS_SIGNAL";
    case 'S': return "SATELLITES_VISIBLE_NO_FIX";
    case 'A': return "GPS_ANTENNA_OR_RF_PATH";
    case 'G': return "GSV_DISABLED_OR_GPS_CONFIG";
    case 'N': return "NO_GPS_UART_DATA";
    case 'B': return "GPS_PROTOCOL_OR_BAUD";
    case 'U': return "UBX_ONLY_OR_NMEA_DISABLED";
    case 'C': return "NMEA_CORRUPT_OR_UART_NOISE";
    case 'V': return "GPS_POWER_RAIL_CONFIG";
    default: return "UNKNOWN";
  }
}

}  // namespace coastcue
