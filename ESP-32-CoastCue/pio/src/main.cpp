#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <RadioLib.h>
#include <esp_system.h>
#include <mbedtls/md.h>

#include "coastcue_config.h"
#include "coastcue_pins.h"
#include "boat_button_logic.h"
#include "boat_emergency_scheduler.h"
#include "boat_incident_snapshot.h"
#include "boat_gps_policy.h"
#include "boat_report_scheduler.h"
#include "incident_protocol.h"
#include "shore_logic.h"

#if defined(COASTCUE_ROLE_BOAT) && defined(COASTCUE_ROLE_SHORE)
#error "Select exactly one CoastCue role"
#elif !defined(COASTCUE_ROLE_BOAT) && !defined(COASTCUE_ROLE_SHORE)
#error "Select exactly one CoastCue role"
#endif

#if defined(COASTCUE_ROLE_SHORE)
#include "shore_runtime.h"
#else
#include <Preferences.h>
#include <Wire.h>
#include <TinyGPSPlus.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif

#if defined(COASTCUE_ROLE_BOAT)
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#define COASTCUE_PMU_ENABLED 1
#define COASTCUE_PMU_LABEL "AXP2101"
#else
#define COASTCUE_PMU_ENABLED 0
#define COASTCUE_PMU_LABEL "none"
#endif

#if !defined(COASTCUE_LORA_FREQUENCY_MHZ) || !defined(COASTCUE_LORA_BANDWIDTH_KHZ) || !defined(COASTCUE_LORA_SPREADING_FACTOR) || \
    !defined(COASTCUE_LORA_CODING_RATE) || !defined(COASTCUE_LORA_TX_POWER_DBM)
#error "Select a complete PlatformIO radio profile"
#endif
#ifndef COASTCUE_LORA_SYNC_WORD
#define COASTCUE_LORA_SYNC_WORD 0x12
#endif
#ifndef COASTCUE_LORA_PREAMBLE_LENGTH
#define COASTCUE_LORA_PREAMBLE_LENGTH 8
#endif

#if defined(COASTCUE_ROLE_SHORE)
constexpr bool IS_SHORE = true;
#else
constexpr bool IS_SHORE = false;
#endif

static_assert(COASTCUE_LORA_FREQUENCY_MHZ == 915.0 &&
              COASTCUE_LORA_BANDWIDTH_KHZ == 500.0 &&
              COASTCUE_LORA_TX_POWER_DBM == 13,
              "US RF settings must match the reviewed 915 MHz profile");

constexpr bool authKeyIsLowerHexAt(const char *key, size_t index) {
  return index == 64 ? key[index] == '\0'
      : (((key[index] >= '0' && key[index] <= '9') ||
          (key[index] >= 'a' && key[index] <= 'f')) &&
         authKeyIsLowerHexAt(key, index + 1));
}

constexpr bool configuredAuthKeyIsLowerHex() {
  return authKeyIsLowerHexAt(COASTCUE_LORA_AUTH_KEY, 0);
}
static_assert(configuredAuthKeyIsLowerHex(),
              "COASTCUE_LORA_AUTH_KEY must be a 64-character lowercase hex key");

constexpr uint32_t ALERT_DISPLAY_MS = 30000;
constexpr uint32_t SOS_RETRY_MS = 2000;
constexpr uint32_t RADIO_RETRY_MS = 5000;
constexpr uint32_t SHORE_RADIO_CHECK_MS = 10000;
constexpr uint32_t GPS_LOG_INTERVAL_MS = 5000;
constexpr uint32_t GPS_NO_DATA_WARN_MS = 15000;
// A 255-byte frame is on air for 0.43 s, well below this watchdog margin.
constexpr uint32_t LORA_TX_TIMEOUT_MS = 15000;
// Shore answers ~0.25 s after an uplink plus <=0.43 s airtime; 3 s leaves
// margin for its flash writes.
constexpr uint32_t LORA_RESPONSE_WINDOW_MS = 3000;
// Heartbeat period while the shore may still answer: each heartbeat is its
// only downlink slot.
constexpr uint32_t INCIDENT_REPORT_PERIOD_MS = 3000;
constexpr uint32_t STATE_RECEIPT_TURNAROUND_MS = 200;
constexpr uint32_t SHORE_RESPONSE_TURNAROUND_MS = 250;
constexpr uint32_t BOAT_INCIDENT_CHECKPOINT_RETRY_MS = 5000;
constexpr uint16_t BUZZER_FREQ = 2000;
constexpr uint8_t BUZZER_CHANNEL = 0;
constexpr uint8_t BUZZER_RES = 8;
constexpr uint32_t DEBOUNCE_MS = 35;
constexpr size_t LORA_MAX_PACKET_BYTES = 255;
static_assert(LORA_MAX_PACKET_BYTES == coastcue::kLoraPacketMaxBytes,
              "transport ceiling must match the current protocol model");

const char *const BOAT_ID = COASTCUE_BOAT_ID;

Module loraModule(COASTCUE_PIN_LORA_NSS, COASTCUE_PIN_LORA_DIO0,
                  COASTCUE_PIN_LORA_RST, COASTCUE_PIN_LORA_DIO1);
SX1276 loraRadio(&loraModule);

#if COASTCUE_PMU_ENABLED
XPowersPMU coastcuePower;
#endif

#if !defined(COASTCUE_ROLE_SHORE)
HardwareSerial GPS(1);
TinyGPSPlus gps;
Adafruit_SSD1306 display(COASTCUE_OLED_WIDTH, COASTCUE_OLED_HEIGHT, &Wire,
                         COASTCUE_OLED_RESET);
#endif

volatile bool loraPacketReady = false;
volatile bool loraTransmitDone = false;
#if defined(COASTCUE_ROLE_SHORE)
ShoreRuntime shoreRuntime;
bool shoreDownlinkScheduled = false;
uint32_t shoreDownlinkDueAt = 0;
String shoreDownlinkPacket;
String shoreDownlinkIncidentId;
String shoreDownlinkState;
uint32_t shoreDownlinkStateSeq = 0;
#endif
bool loraReady = false;
bool loraTransmitInFlight = false;
bool pmuReady = !COASTCUE_PMU_ENABLED;
bool boatHardwareReady = IS_SHORE;
bool displayReady = false;
bool buzzing = false;
uint32_t buzzOffAt = 0;
uint32_t lastRadioAttemptAt = 0;
uint32_t lastGpsLogAt = 0;
uint32_t loraTransmitStartedAt = 0;
String nodeUid;
coastcue::BoatEmergencyScheduler emergencyScheduler;
BoatReportScheduler normalReportScheduler;
String lastEventText;
uint32_t eventTextUntil = 0;

#if !defined(COASTCUE_ROLE_SHORE)
coastcue::BoatIncidentState boatIncidentState;
coastcue::BoatTerminalReconciliation boatTerminalReconciliation;
coastcue::BoatEmergencyKind boatIncidentKind =
    coastcue::BoatEmergencyKind::kNone;
Preferences boatIncidentPreferences;
bool boatReceiveWindowActive = false;
uint32_t boatReceiveWindowUntil = 0;
bool boatStateReceiptPending = false;
bool boatStateReceiptInFlight = false;
uint32_t boatStateReceiptDueAt = 0;
uint32_t boatStateReceiptSeq = 0;
String boatStateReceiptIncidentId;
String boatEmergencyTransmitIncidentId;
bool boatSosIdentityPending = false;
bool boatIncidentCheckpointPending = false;
uint32_t boatIncidentCheckpointRetryAt = 0;
#endif

struct ButtonState {
  bool rawLevel = HIGH;
  bool stableLevel = HIGH;
  bool sosFired = false;
  uint32_t rawChangedAt = 0;
  uint32_t pressedAt = 0;
} buttonState;

void setBlue(bool on);
void setRed(bool on);
void showEvent(const String &message);
void startBuzz(uint32_t durationMs, uint32_t frequency = BUZZER_FREQ);

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    switch (c) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        escaped += static_cast<uint8_t>(c) < 0x20 ? ' ' : c;
        break;
    }
  }
  return escaped;
}

String nodeUidFromEfuseMac() {
  // ESP.getEfuseMac() reads the hardware eFuse and does not initialize Wi-Fi.
  const uint64_t efuseMac = ESP.getEfuseMac();
  char value[19];
  snprintf(value, sizeof(value), "esp32-%012llx",
           static_cast<unsigned long long>(efuseMac & 0xFFFFFFFFFFFFULL));
  return String(value);
}

String randomNonceHex() {
  char value[17];
  snprintf(value, sizeof(value), "%08lx%08lx",
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  return String(value);
}

// HMAC-SHA-256 over the exact unsigned bytes, truncated to 128 bits as 32
// lowercase hex characters. The key format is checked at compile time above.
bool hmacSha256TruncatedHex(const String &unsignedPacket, String &macHex) {
  const char *key = COASTCUE_LORA_AUTH_KEY;
  uint8_t keyBytes[32];
  for (size_t i = 0; i < sizeof(keyBytes); ++i) {
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    keyBytes[i] = static_cast<uint8_t>(nibble(key[i * 2]) << 4 | nibble(key[i * 2 + 1]));
  }
  uint8_t digest[32];
  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), keyBytes,
                      sizeof(keyBytes),
                      reinterpret_cast<const unsigned char *>(unsignedPacket.c_str()),
                      unsignedPacket.length(), digest) != 0) {
    return false;
  }
  char encoded[33];
  for (size_t i = 0; i < 16; ++i) snprintf(encoded + i * 2, 3, "%02x", digest[i]);
  macHex = String(encoded);
  return true;
}

bool authenticateUnsignedPacket(const String &unsignedPacket,
                                String &wirePacket) {
  if (!unsignedPacket.endsWith("}")) return false;
  String macHex;
  if (!hmacSha256TruncatedHex(unsignedPacket, macHex)) return false;
  wirePacket = unsignedPacket.substring(0, unsignedPacket.length() - 1);
  wirePacket += ",\"mac\":\"";
  wirePacket += macHex;
  wirePacket += "\"}";
  return true;
}

#if !defined(COASTCUE_ROLE_SHORE)
bool isFreshGpsFix() {
  return gps.location.isValid() &&
         gps.location.age() <= static_cast<uint32_t>(COASTCUE_GPS_FRESHNESS_MS);
}

int readBatteryPercent() {
  if (pmuReady && coastcuePower.isBatteryConnect()) {
    const int batteryPercent = coastcuePower.getBatteryPercent();
    if (batteryPercent >= 0 && batteryPercent <= 100) {
      return batteryPercent;
    }
  }
  return -1;
}

// Wire format is deterministic: unsigned JSON ends in '}', HMAC input is those
// exact UTF-8 bytes, and the authenticated wire packet replaces that final '}'
// with ,"mac":"<32 lowercase hex chars>"}. The mac field is always last.
bool buildAuthenticatedPacket(const char *status, int batteryPercent,
                              String &wirePacket,
                              const char *incidentId = nullptr) {
  const bool emergency =
      strcmp(status, "help") == 0 || strcmp(status, "sos") == 0;
  const bool incident = coastcue::isValidIncidentId(incidentId);
  // Emergencies always carry an Incident; listening telemetry may carry one.
  if ((emergency || incidentId != nullptr) && !incident) return false;
  const uint32_t gpsAgeMs =
      gps.location.isValid() ? gps.location.age() : UINT32_MAX;
  const coastcue::BoatGpsPolicy gpsPolicy = coastcue::selectBoatGpsPolicy(
      gps.location.isValid(), gpsAgeMs, emergency, COASTCUE_GPS_FRESHNESS_MS);
  const bool gpsValid = gpsPolicy.includeCoordinates;
  String unsignedPacket = "{\"nonce\":\"";
  unsignedPacket += randomNonceHex();
  unsignedPacket += "\",\"node_uid\":\"";
  unsignedPacket += jsonEscape(nodeUid);
  if (incident) {
    // Keep the emergency uplink below the SX127x 255-byte ceiling. The shore
    // expands this compact wire key back to `incident_id` in its journal.
    unsignedPacket += "\",\"i\":\"";
    unsignedPacket += incidentId;
  }
  unsignedPacket += "\",\"status\":\"";
  unsignedPacket += jsonEscape(status);
  unsignedPacket += "\",\"gps\":{\"valid\":";
  unsignedPacket += gpsValid ? "true" : "false";
  if (gpsValid) {
    unsignedPacket += ",\"latitude\":";
    unsignedPacket += String(gps.location.lat(), 6);
    unsignedPacket += ",\"longitude\":";
    unsignedPacket += String(gps.location.lng(), 6);
    unsignedPacket += ",\"age_s\":";
    unsignedPacket += String(gpsPolicy.ageSeconds);
  }
  unsignedPacket += "}";

  if (batteryPercent >= 0) {
    unsignedPacket += ",\"battery_percent\":";
    unsignedPacket += String(batteryPercent);
  }
  unsignedPacket += "}";

  return authenticateUnsignedPacket(unsignedPacket, wirePacket);
}
#endif

bool packetFitsTransport(const String &packet) {
  return packet.length() <= LORA_MAX_PACKET_BYTES;
}

#if !defined(COASTCUE_ROLE_SHORE)
// Battery telemetry is optional: drop it if it pushes the frame past 255 bytes.
bool buildFittedAuthenticatedPacket(const char *status, String &wirePacket,
                                    const char *incidentId = nullptr) {
  const int batteryPercent = readBatteryPercent();
  return (buildAuthenticatedPacket(status, batteryPercent, wirePacket, incidentId) &&
          packetFitsTransport(wirePacket)) ||
         (batteryPercent >= 0 &&
          buildAuthenticatedPacket(status, -1, wirePacket, incidentId) &&
          packetFitsTransport(wirePacket));
}
#endif

#if defined(COASTCUE_ROLE_SHORE)
bool buildAuthenticatedIncidentStatePacket(
    const ShoreRadioDownlink &downlink, String &wirePacket) {
  coastcue::IncidentState state = coastcue::IncidentState::kNone;
  if (!coastcue::isValidIncidentId(downlink.incidentId.c_str()) ||
      !coastcue::parseIncidentStateWireName(downlink.state.c_str(), state) ||
      state == coastcue::IncidentState::kOpen ||
      state == coastcue::IncidentState::kNone ||
      downlink.stateSeq == 0 || downlink.targetNodeUid.isEmpty()) {
    return false;
  }
  String unsignedPacket =
      "{\"kind\":\"incident_state\",\"nonce\":\"";
  unsignedPacket += randomNonceHex();
  unsignedPacket += "\",\"node_uid\":\"";
  unsignedPacket += jsonEscape(nodeUid);
  unsignedPacket += "\",\"target_node_uid\":\"";
  unsignedPacket += jsonEscape(downlink.targetNodeUid);
  unsignedPacket += "\",\"incident_id\":\"";
  unsignedPacket += downlink.incidentId;
  unsignedPacket += "\",\"state\":\"";
  unsignedPacket += downlink.state;
  unsignedPacket += "\",\"state_seq\":";
  unsignedPacket += String(downlink.stateSeq);
  unsignedPacket += "}";
  return authenticateUnsignedPacket(unsignedPacket, wirePacket) &&
         packetFitsTransport(wirePacket);
}
#else
bool buildAuthenticatedStateReceiptPacket(const String &incidentId,
                                          uint32_t stateSeq,
                                          String &wirePacket) {
  if (!coastcue::isValidIncidentId(incidentId.c_str()) || stateSeq == 0) {
    return false;
  }
  String unsignedPacket =
      "{\"kind\":\"state_receipt\",\"nonce\":\"";
  unsignedPacket += randomNonceHex();
  unsignedPacket += "\",\"node_uid\":\"";
  unsignedPacket += jsonEscape(nodeUid);
  unsignedPacket += "\",\"incident_id\":\"";
  unsignedPacket += incidentId;
  unsignedPacket += "\",\"state_seq\":";
  unsignedPacket += String(stateSeq);
  unsignedPacket += "}";
  return authenticateUnsignedPacket(unsignedPacket, wirePacket) &&
         packetFitsTransport(wirePacket);
}
#endif

bool verifyAuthenticatedPacket(const String &wirePacket) {
  const int macFieldAt = wirePacket.lastIndexOf(",\"mac\":\"");
  if (macFieldAt <= 0 || !wirePacket.endsWith("\"}")) {
    return false;
  }
  const int macStart = macFieldAt + 8;
  if (wirePacket.length() != static_cast<unsigned int>(macStart + 32 + 2)) {
    return false;
  }
  const String suppliedMac = wirePacket.substring(macStart, macStart + 32);
  for (size_t i = 0; i < suppliedMac.length(); ++i) {
    const char c = suppliedMac.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }

  const String unsignedPacket = wirePacket.substring(0, macFieldAt) + "}";
  String expectedMac;
  if (!hmacSha256TruncatedHex(unsignedPacket, expectedMac)) {
    return false;
  }
  uint8_t difference = 0;
  for (size_t i = 0; i < 32; ++i) {
    difference |= expectedMac.charAt(i) ^ suppliedMac.charAt(i);
  }
  return difference == 0;
}

bool readJsonPositiveUInt32(JsonVariantConst value, uint32_t &result) {
  if (value.is<uint64_t>()) {
    const uint64_t parsed = value.as<uint64_t>();
    if (parsed == 0 || parsed > UINT32_MAX) return false;
    result = static_cast<uint32_t>(parsed);
    return true;
  }
  if (value.is<int64_t>()) {
    const int64_t parsed = value.as<int64_t>();
    if (parsed <= 0 || parsed > static_cast<int64_t>(UINT32_MAX)) {
      return false;
    }
    result = static_cast<uint32_t>(parsed);
    return true;
  }
  return false;
}

#if defined(COASTCUE_ROLE_SHORE)
bool readJsonInteger(JsonVariantConst value, int64_t &result) {
  if (value.is<int64_t>()) {
    result = value.as<int64_t>();
    return true;
  }
  if (value.is<uint64_t>()) {
    const uint64_t unsignedValue = value.as<uint64_t>();
    if (unsignedValue <= static_cast<uint64_t>(INT64_MAX)) {
      result = static_cast<int64_t>(unsignedValue);
      return true;
    }
  }
  return false;
}

bool readJsonNumber(JsonVariantConst value, double &result) {
  if (!value.is<double>()) return false;
  result = value.as<double>();
  return true;
}

void parseBoatRadioUplink(JsonObjectConst input,
                         coastcue::BoatRadioUplink &packet) {
  const char *nonce = input["nonce"].as<const char *>();
  if (input["nonce"].is<const char *>()) packet.nonce = nonce;
  const char *nodeUid = input["node_uid"].as<const char *>();
  if (input["node_uid"].is<const char *>()) packet.nodeUid = nodeUid;
  packet.incidentIdPresent = !input["i"].isUnbound();
  const char *incidentId = input["i"].as<const char *>();
  if (input["i"].is<const char *>()) packet.incidentId = incidentId;
  const char *status = input["status"].as<const char *>();
  if (input["status"].is<const char *>()) packet.status = status;

  if (input["gps"].is<JsonObjectConst>()) {
    const JsonObjectConst gps = input["gps"].as<JsonObjectConst>();
    if (gps["valid"].is<bool>()) packet.gps.valid = gps["valid"].as<bool>();
    double coordinate = 0;
    if (readJsonNumber(gps["latitude"], coordinate)) {
      packet.gps.latitude = coordinate;
    }
    if (readJsonNumber(gps["longitude"], coordinate)) {
      packet.gps.longitude = coordinate;
    }
    int64_t ageSeconds = 0;
    if (readJsonInteger(gps["age_s"], ageSeconds)) {
      packet.gps.ageSeconds = ageSeconds;
    }
  }

  const JsonVariantConst battery = input["battery_percent"];
  packet.batteryPresent = !battery.isUnbound();
  int64_t batteryPercent = 0;
  if (packet.batteryPresent && readJsonInteger(battery, batteryPercent) &&
      batteryPercent >= INT_MIN && batteryPercent <= INT_MAX) {
    packet.batteryPercent = static_cast<int>(batteryPercent);
  }
}

enum class RadioUplinkKind : uint8_t {
  kInvalid,
  kTelemetry,
  kIncident,
  kStateReceipt,
};

RadioUplinkKind validateRadioUplinkJson(const String &wirePacket) {
  JsonDocument document;
  if (deserializeJson(document, wirePacket) != DeserializationError::Ok ||
      !document.is<JsonObject>()) {
    return RadioUplinkKind::kInvalid;
  }
  if (!document["v"].isUnbound() || !document["boat_id"].isUnbound()) {
    return RadioUplinkKind::kInvalid;
  }

  const String kind = document["kind"] | "";
  if (!document["kind"].isUnbound()) {
    if (kind != "state_receipt" || !document["i"].isUnbound() ||
        !document["status"].isUnbound() || !document["gps"].isUnbound() ||
        !document["battery_percent"].isUnbound() ||
        !document["state"].isUnbound() ||
        !document["target_node_uid"].isUnbound()) {
      return RadioUplinkKind::kInvalid;
    }
    const JsonVariantConst nonceField = document["nonce"];
    const JsonVariantConst nodeUidField = document["node_uid"];
    const JsonVariantConst incidentIdField = document["incident_id"];
    const char *nonce = nonceField.is<const char *>()
                            ? nonceField.as<const char *>()
                            : nullptr;
    const char *nodeUid = nodeUidField.is<const char *>()
                              ? nodeUidField.as<const char *>()
                              : nullptr;
    const char *incidentId = incidentIdField.is<const char *>()
                                 ? incidentIdField.as<const char *>()
                                 : nullptr;
    int64_t stateSeq = 0;
    if (!coastcue::isValidIncidentId(nonce) ||
        !coastcue::isValidIncidentId(incidentId) || nodeUid == nullptr ||
        nodeUid[0] == '\0' ||
        !readJsonInteger(document["state_seq"], stateSeq) ||
        stateSeq <= 0 || stateSeq > UINT32_MAX) {
      return RadioUplinkKind::kInvalid;
    }
    return RadioUplinkKind::kStateReceipt;
  }
  if (!document["incident_id"].isUnbound() ||
      !document["state"].isUnbound() ||
      !document["state_seq"].isUnbound() ||
      !document["target_node_uid"].isUnbound()) {
    return RadioUplinkKind::kInvalid;
  }
  coastcue::BoatRadioUplink uplink;
  parseBoatRadioUplink(document.as<JsonObjectConst>(), uplink);
  const String status = document["status"] | "";
  if (status == "normal") {
    return coastcue::validateTelemetryUplink(uplink)
               ? RadioUplinkKind::kTelemetry
               : RadioUplinkKind::kInvalid;
  }
  return coastcue::validateIncidentUplink(uplink)
             ? RadioUplinkKind::kIncident
             : RadioUplinkKind::kInvalid;
}
#endif

#if !defined(COASTCUE_ROLE_SHORE)
bool parseAuthenticatedIncidentStatePacket(
    const String &wirePacket, String &incidentId,
    coastcue::IncidentState &state, uint32_t &stateSeq) {
  JsonDocument document;
  if (deserializeJson(document, wirePacket) != DeserializationError::Ok ||
      !document.is<JsonObjectConst>() || !document["v"].isUnbound() ||
      !document["boat_id"].isUnbound() ||
      !document["i"].isUnbound() || !document["status"].isUnbound() ||
      !document["gps"].isUnbound() ||
      !document["battery_percent"].isUnbound() ||
      !document["kind"].is<const char *>() ||
      strcmp(document["kind"].as<const char *>(), "incident_state") != 0 ||
      !document["nonce"].is<const char *>() ||
      !coastcue::isValidIncidentId(
          document["nonce"].as<const char *>()) ||
      !document["node_uid"].is<const char *>() ||
      document["node_uid"].as<const char *>()[0] == '\0' ||
      !document["target_node_uid"].is<const char *>() ||
      nodeUid != document["target_node_uid"].as<const char *>() ||
      !document["incident_id"].is<const char *>() ||
      !document["state"].is<const char *>()) {
    return false;
  }
  const char *parsedIncidentId =
      document["incident_id"].as<const char *>();
  uint32_t parsedStateSeq = 0;
  if (!readJsonPositiveUInt32(document["state_seq"], parsedStateSeq)) {
    return false;
  }
  coastcue::IncidentState parsedState = coastcue::IncidentState::kNone;
  if (!coastcue::parseIncidentStateWireName(
          document["state"].as<const char *>(), parsedState)) {
    return false;
  }
  const coastcue::IncidentDownlink metadata{
      parsedIncidentId, parsedState, parsedStateSeq};
  if (!coastcue::isValidIncidentDownlink(metadata)) return false;
  incidentId = parsedIncidentId;
  state = parsedState;
  stateSeq = parsedStateSeq;
  return true;
}
#endif

void IRAM_ATTR onLoraPacketReady() { loraPacketReady = true; }
void IRAM_ATTR onLoraTransmitDone() { loraTransmitDone = true; }

void setLoraReceiveAction() {
  loraRadio.clearDio0Action();
  loraPacketReady = false;
  loraRadio.setDio0Action(onLoraPacketReady, RISING);
}

void setLoraTransmitAction() {
  loraRadio.clearDio0Action();
  loraTransmitDone = false;
  loraRadio.setDio0Action(onLoraTransmitDone, RISING);
}

bool loraStartReceive() {
  setLoraReceiveAction();
  const int state = loraRadio.startReceive();
  if (state == RADIOLIB_ERR_NONE) return true;
  Serial.printf("[LORA] startReceive failed: %d\n", state);
  return false;
}

bool startLoRaTransport() {
  if (!IS_SHORE && !pmuReady) {
    Serial.println("[FATAL] PMU not ready; LoRa startup blocked");
    return false;
  }
  SPI.begin(COASTCUE_PIN_LORA_SCK, COASTCUE_PIN_LORA_MISO,
            COASTCUE_PIN_LORA_MOSI, COASTCUE_PIN_LORA_NSS);
  const int state = loraRadio.begin(
      COASTCUE_LORA_FREQUENCY_MHZ, COASTCUE_LORA_BANDWIDTH_KHZ,
      COASTCUE_LORA_SPREADING_FACTOR, COASTCUE_LORA_CODING_RATE,
      COASTCUE_LORA_SYNC_WORD, COASTCUE_LORA_TX_POWER_DBM,
      COASTCUE_LORA_PREAMBLE_LENGTH);
  if (state != RADIOLIB_ERR_NONE) {
    loraReady = false;
    Serial.printf("[LORA] begin failed: %d\n", state);
    return false;
  }
  loraRadio.setCRC(true);
#if defined(COASTCUE_ROLE_SHORE)
  shoreDownlinkScheduled = false;
  shoreDownlinkPacket = "";
  loraReady = loraStartReceive();
#else
  setLoraTransmitAction();
  loraTransmitInFlight = false;
  boatReceiveWindowActive = false;
  boatStateReceiptInFlight = false;
  loraReady = true;
  setBlue(true);
#endif
  Serial.printf("[LORA] %s\n", loraReady ? "ready" : "offline");
  return loraReady;
}

bool loraTransmitPacket(const String &packet) {
  if (!loraReady || !packetFitsTransport(packet)) return false;
  if (loraTransmitInFlight) return false;
  loraRadio.standby();
  setLoraTransmitAction();
  String writablePacket = packet;
  const int state = loraRadio.startTransmit(writablePacket);
  if (state != RADIOLIB_ERR_NONE) {
    loraReady = false;
    if (!IS_SHORE) setBlue(false);
    Serial.printf("[LORA] transmit failed: %d\n", state);
    return false;
  }
  loraTransmitInFlight = true;
  loraTransmitStartedAt = millis();
  return true;
}

#if defined(COASTCUE_ROLE_SHORE)
bool handleTransportPacket(const String &packet) {
  if (!verifyAuthenticatedPacket(packet)) {
    Serial.println("[LORA] unauthenticated packet dropped");
    return false;
  }
  if (!coastcue::hasUniqueRadioTopLevelKeys(
          std::string(packet.c_str(), packet.length()))) {
    Serial.println("[LORA] duplicate or malformed radio packet dropped");
    return false;
  }
  const RadioUplinkKind kind = validateRadioUplinkJson(packet);
  if (kind == RadioUplinkKind::kInvalid) {
    Serial.println("[LORA] invalid radio packet dropped");
    return false;
  }
  if (!shoreRuntime.acceptNonce(packet)) {
    Serial.println("[LORA] invalid or replayed packet dropped");
    return false;
  }
#if COASTCUE_VERBOSE_SERIAL
  Serial.println(packet);
#endif
  return shoreRuntime.ingestAuthenticatedPacket(
      packet, loraRadio.getRSSI(), loraRadio.getSNR());
}

void serviceShoreRadio() {
  if (!loraReady) return;
  if (loraTransmitInFlight) {
    if (!loraTransmitDone &&
        millis() - loraTransmitStartedAt < LORA_TX_TIMEOUT_MS) {
      return;
    }
    const bool timedOut = !loraTransmitDone;
    const int finishState = loraRadio.finishTransmit();
    loraTransmitDone = false;
    loraTransmitInFlight = false;
    if (timedOut) {
      Serial.println("[LORA] shore downlink TX timeout; response retained");
    }
    if (finishState != RADIOLIB_ERR_NONE) {
      Serial.printf("[LORA] shore downlink finish failed: %d\n",
                    finishState);
    }
    loraReady = loraStartReceive();
    return;
  }
  if (shoreDownlinkScheduled) {
    if (static_cast<int32_t>(millis() - shoreDownlinkDueAt) < 0) {
      return;
    }
    shoreDownlinkScheduled = false;
    if (loraTransmitPacket(shoreDownlinkPacket)) {
      Serial.printf(
          "[LORA] downlink incident=%s state=%s seq=%lu\n",
          shoreDownlinkIncidentId.c_str(), shoreDownlinkState.c_str(),
          static_cast<unsigned long>(shoreDownlinkStateSeq));
      shoreDownlinkPacket = "";
      return;
    }
    Serial.println("[LORA] downlink TX start failed; response retained");
    shoreDownlinkPacket = "";
    loraReady = loraStartReceive();
    return;
  }
  if (!loraPacketReady) {
    // A radio reset by a supply glitch keeps answering SPI but no longer
    // listens. Re-run begin() through the normal retry path.
    static uint32_t lastRadioCheckAt = 0;
    if (millis() - lastRadioCheckAt < SHORE_RADIO_CHECK_MS) return;
    lastRadioCheckAt = millis();
    const int16_t opMode =
        loraModule.SPIgetRegValue(RADIOLIB_SX127X_REG_OP_MODE);
    constexpr int16_t RX_MODE_MASK = RADIOLIB_SX127X_LORA | 0b00000111;
    if ((opMode & RX_MODE_MASK) !=
        (RADIOLIB_SX127X_LORA | RADIOLIB_SX127X_RXCONTINUOUS)) {
      Serial.printf("[LORA] receiver not listening (op_mode=%d); restarting\n",
                    opMode);
      loraReady = false;
    }
    return;
  }

  loraPacketReady = false;
  String packet;
  const int state = loraRadio.readData(packet);
  if (state == RADIOLIB_ERR_NONE && packet.length() > 0) {
    const bool responseSlot = handleTransportPacket(packet);
    if (responseSlot) {
      ShoreRadioDownlink downlink;
      String response;
      if (shoreRuntime.takePendingRadioDownlink(downlink) &&
          buildAuthenticatedIncidentStatePacket(downlink, response)) {
        shoreDownlinkPacket = response;
        shoreDownlinkIncidentId = downlink.incidentId;
        shoreDownlinkState = downlink.state;
        shoreDownlinkStateSeq = downlink.stateSeq;
        shoreDownlinkDueAt = millis() + SHORE_RESPONSE_TURNAROUND_MS;
        shoreDownlinkScheduled = true;
        return;
      }
    }
  } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
    Serial.println("[LORA] CRC mismatch");
  } else if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] receive failed: %d\n", state);
  }
  loraReady = loraStartReceive();
}
#endif

#if !defined(COASTCUE_ROLE_SHORE)
const char *emergencyStatus(coastcue::BoatEmergencyKind kind) {
  return kind == coastcue::BoatEmergencyKind::kSos ? "sos" : "help";
}

String emergencyLabel(coastcue::BoatEmergencyKind kind) {
  return kind == coastcue::BoatEmergencyKind::kSos ? "SOS" : "Help";
}

const char *boatIncidentDisplayText(coastcue::IncidentState state) {
  switch (state) {
    case coastcue::IncidentState::kOpen: return "Awaiting shore";
    case coastcue::IncidentState::kReceipt: return "Shore received";
    case coastcue::IncidentState::kAcknowledged: return "Staff acknowledged";
    case coastcue::IncidentState::kResolved: return "Incident resolved";
    case coastcue::IncidentState::kNone: return "LoRa ready";
  }
  return "LoRa ready";
}

bool persistBoatIncident() {
  if (!boatIncidentPreferences.begin("ccincident", false)) {
    Serial.println("[CoastCue] incident checkpoint unavailable");
    return false;
  }
  if (!boatIncidentState.hasIncident()) {
    const bool cleared = boatIncidentPreferences.clear();
    boatIncidentPreferences.end();
    if (!cleared) {
      Serial.println("[CoastCue] incident checkpoint clear failed");
    }
    return cleared;
  }
  coastcue::BoatIncidentSnapshot snapshot;
  memcpy(snapshot.incidentId, boatIncidentState.incidentId(), 17);
  snapshot.state = boatIncidentState.state();
  snapshot.stateSeq = boatIncidentState.lastStateSeq();
  snapshot.hasStateSeq = boatIncidentState.hasStateSeq();
  snapshot.kind = boatIncidentKind;
  uint8_t encoded[coastcue::kBoatIncidentSnapshotBytes];
  const bool encodedSnapshot = coastcue::encodeBoatIncidentSnapshot(
      snapshot, encoded, sizeof(encoded));
  const bool stored =
      encodedSnapshot &&
      boatIncidentPreferences.putBytes("snapshot", encoded, sizeof(encoded)) ==
          sizeof(encoded);
  boatIncidentPreferences.end();
  if (!stored) Serial.println("[CoastCue] incident checkpoint write failed");
  return stored;
}

void scheduleBoatIncidentCheckpointRetry() {
  boatIncidentCheckpointPending = true;
  boatIncidentCheckpointRetryAt =
      millis() + BOAT_INCIDENT_CHECKPOINT_RETRY_MS;
}

void serviceBoatIncidentCheckpoint() {
  if (!boatIncidentCheckpointPending ||
      !boatIncidentState.hasIncident() ||
      static_cast<int32_t>(millis() - boatIncidentCheckpointRetryAt) < 0) {
    return;
  }
  if (persistBoatIncident()) {
    boatIncidentCheckpointPending = false;
    Serial.printf("[CoastCue] incident checkpoint recovered id=%s\n",
                  boatIncidentState.incidentId());
    showEvent("Incident saved");
  } else {
    boatIncidentCheckpointRetryAt =
        millis() + BOAT_INCIDENT_CHECKPOINT_RETRY_MS;
  }
}

void restoreBoatIncident() {
  if (!boatIncidentPreferences.begin("ccincident", true)) return;
  const bool hasSnapshot = boatIncidentPreferences.isKey("snapshot");
  coastcue::BoatIncidentSnapshot snapshot;
  bool decodedSnapshot = false;
  if (hasSnapshot &&
      boatIncidentPreferences.getBytesLength("snapshot") ==
          coastcue::kBoatIncidentSnapshotBytes) {
    uint8_t encoded[coastcue::kBoatIncidentSnapshotBytes];
    const size_t read = boatIncidentPreferences.getBytes(
        "snapshot", encoded, sizeof(encoded));
    decodedSnapshot = read == sizeof(encoded) &&
                      coastcue::decodeBoatIncidentSnapshot(
                          encoded, sizeof(encoded), snapshot);
  }
  boatIncidentPreferences.end();
  if (hasSnapshot && !decodedSnapshot) {
    Serial.println("[CoastCue] invalid incident snapshot ignored");
    return;
  }
  if (!hasSnapshot) return;
  if (!boatIncidentState.restore(snapshot.incidentId, snapshot.state,
                                 snapshot.stateSeq, snapshot.hasStateSeq) ||
      !coastcue::isValidBoatEmergencyKind(snapshot.kind)) {
    return;
  }
  boatIncidentKind = snapshot.kind;
  if (boatIncidentState.isTerminal()) {
    boatTerminalReconciliation.begin();
  }
  if (boatIncidentState.shouldRetryOpen()) {
    emergencyScheduler.queue(snapshot.kind);
  }
}

void beginBoatIncidentIfNeeded(coastcue::BoatEmergencyKind kind) {
  const coastcue::BoatIncidentIdentityAction identityAction =
      coastcue::decideBoatIncidentIdentity(
          boatIncidentState.hasIncident(), boatIncidentState.isTerminal(),
          boatIncidentKind, kind);
  if (identityAction ==
      coastcue::BoatIncidentIdentityAction::kKeepCurrent) {
    return;
  }
  const String incidentId = randomNonceHex();
  if (!boatIncidentState.begin(incidentId.c_str())) return;
  boatIncidentKind = kind;
  if (coastcue::shouldRetirePendingStateReceipt(
          boatStateReceiptPending, boatStateReceiptInFlight,
          boatStateReceiptIncidentId.c_str(),
          boatIncidentState.incidentId())) {
    Serial.printf(
        "[LORA] retired superseded state receipt incident=%s seq=%lu\n",
        boatStateReceiptIncidentId.c_str(),
        static_cast<unsigned long>(boatStateReceiptSeq));
    boatStateReceiptPending = false;
    boatStateReceiptIncidentId = "";
    boatStateReceiptSeq = 0;
  }
  if (persistBoatIncident()) {
    boatIncidentCheckpointPending = false;
  } else {
    scheduleBoatIncidentCheckpointRetry();
    Serial.printf(
        "[CoastCue] incident live-only until checkpoint recovers id=%s\n",
        boatIncidentState.incidentId());
  }
  Serial.printf("[CoastCue] incident opened id=%s severity=%s\n",
                boatIncidentState.incidentId(), emergencyStatus(kind));
}

void scheduleBoatStateReceipt(const String &incidentId, uint32_t stateSeq) {
  boatStateReceiptIncidentId = incidentId;
  boatStateReceiptSeq = stateSeq;
  boatStateReceiptDueAt = millis() + STATE_RECEIPT_TURNAROUND_MS;
  boatStateReceiptPending = true;
}

void closeBoatReceiveWindow() {
  if (!boatReceiveWindowActive) return;
  boatReceiveWindowActive = false;
  const int state = loraRadio.standby();
  if (state != RADIOLIB_ERR_NONE) {
    loraReady = false;
    Serial.printf("[LORA] boat standby failed: %d\n", state);
    return;
  }
  setLoraTransmitAction();
}

bool boatAwaitingShore() {
  return boatIncidentState.hasIncident() &&
         (!boatIncidentState.isTerminal() ||
          boatTerminalReconciliation.shouldListen());
}

void startBoatReceiveWindow() {
  if (!loraReady || loraTransmitInFlight || !boatAwaitingShore()) return;
  loraReady = loraStartReceive();
  if (!loraReady) return;
  boatReceiveWindowActive = true;
  boatReceiveWindowUntil = millis() + LORA_RESPONSE_WINDOW_MS;
}

void applyBoatIncidentDownlink(const String &incidentId,
                               coastcue::IncidentState state,
                               uint32_t stateSeq) {
  const coastcue::BoatIncidentState previousState = boatIncidentState;
  const coastcue::IncidentApplyResult result =
      boatIncidentState.applyDownlink({incidentId.c_str(), state, stateSeq});
  if (result == coastcue::IncidentApplyResult::kInvalid ||
      result == coastcue::IncidentApplyResult::kConflict) {
    return;
  }
  if (result == coastcue::IncidentApplyResult::kApplied) {
    if (!persistBoatIncident()) {
      boatIncidentState = previousState;
      showEvent("State save failed");
      Serial.printf(
          "[LORA] incident state not confirmed: checkpoint failed id=%s "
          "seq=%lu\n",
          incidentId.c_str(), static_cast<unsigned long>(stateSeq));
      return;
    }
    boatIncidentCheckpointPending = false;
    // An in-flight Help→SOS handoff owns the queued SOS. A Receipt for the
    // old Help must not cancel that higher-severity request.
    if (!boatSosIdentityPending) emergencyScheduler.cancelPending();
    showEvent(boatIncidentDisplayText(boatIncidentState.state()));
    if (boatIncidentState.isTerminal()) setRed(false);
    Serial.printf("[LORA] incident=%s state=%s seq=%lu\n",
                  boatIncidentState.incidentId(),
                  coastcue::incidentStateWireName(boatIncidentState.state()),
                  static_cast<unsigned long>(
                      boatIncidentState.lastStateSeq()));
  }
  if (boatIncidentState.isTerminal() &&
      state == coastcue::IncidentState::kResolved) {
    if (result == coastcue::IncidentApplyResult::kApplied) {
      boatTerminalReconciliation.begin();
    } else {
      boatTerminalReconciliation.observeStateReplay();
    }
  }
  if (boatIncidentState.hasStateSeq() &&
      coastcue::shouldSendStateReceipt(result, true)) {
    scheduleBoatStateReceipt(
        String(boatIncidentState.incidentId()),
        boatIncidentState.lastStateSeq());
  }
}

void serviceBoatReceiveWindow() {
  if (!boatReceiveWindowActive) return;
  if (loraPacketReady) {
    loraPacketReady = false;
    String packet;
    const int radioState = loraRadio.readData(packet);
    String incidentId;
    coastcue::IncidentState state = coastcue::IncidentState::kNone;
    uint32_t stateSeq = 0;
    const bool valid =
        radioState == RADIOLIB_ERR_NONE && !packet.isEmpty() &&
        verifyAuthenticatedPacket(packet) &&
        coastcue::hasUniqueRadioTopLevelKeys(
            std::string(packet.c_str(), packet.length())) &&
        parseAuthenticatedIncidentStatePacket(packet, incidentId, state,
                                              stateSeq);
    if (valid) {
      closeBoatReceiveWindow();
      applyBoatIncidentDownlink(incidentId, state, stateSeq);
      return;
    }
    if (radioState == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("[LORA] boat downlink CRC mismatch");
    } else {
      Serial.println("[LORA] invalid boat downlink ignored");
    }
    loraReady = loraStartReceive();
    if (!loraReady) boatReceiveWindowActive = false;
  }
  if (boatReceiveWindowActive &&
      static_cast<int32_t>(millis() - boatReceiveWindowUntil) >= 0) {
    closeBoatReceiveWindow();
    if (boatIncidentState.shouldRetryOpen()) {
      showEvent("No shore receipt");
    } else if (boatIncidentState.isTerminal()) {
      boatTerminalReconciliation.observeQuietWindow();
    }
  }
}

void serviceBoatStateReceipt() {
  if (!boatStateReceiptPending || boatStateReceiptInFlight ||
      boatReceiveWindowActive || loraTransmitInFlight || !loraReady ||
      (emergencyScheduler.hasPending() &&
       emergencyScheduler.activeKind() == coastcue::BoatEmergencyKind::kSos) ||
      static_cast<int32_t>(millis() - boatStateReceiptDueAt) < 0) {
    return;
  }
  String packet;
  if (!buildAuthenticatedStateReceiptPacket(
          boatStateReceiptIncidentId, boatStateReceiptSeq, packet) ||
      !loraTransmitPacket(packet)) {
    boatStateReceiptDueAt = millis() + 1000;
    return;
  }
  boatStateReceiptInFlight = true;
}

void reportEmergencyCompletion(coastcue::BoatEmergencyKind kind,
                               uint8_t attempt, bool localTxCompleted,
                               coastcue::BoatEmergencyCompletion completion) {
  const String label = emergencyLabel(kind);
  if (localTxCompleted) {
    showEvent(label + " broadcast " + attempt + "/3");
    startBuzz(300);
    Serial.printf(
        "[CoastCue] %s radio broadcast attempt %u/3 complete; delivery "
        "awaiting shore receipt\n",
        label.c_str(), attempt);
  } else {
    showEvent(label + " TX failed " + attempt + "/3");
    Serial.printf("[CoastCue] %s local TX attempt %u/3 failed\n",
                  label.c_str(), attempt);
  }
  if (completion == coastcue::BoatEmergencyCompletion::kUpgraded) {
    showEvent("SOS queued after Help");
  } else if (completion == coastcue::BoatEmergencyCompletion::kExhausted) {
    Serial.printf(
        "[CoastCue] %s burst complete; periodic incident retries continue\n",
        label.c_str());
  }
}

// Returns whether the finished attempt still belongs to the current Incident.
bool finishEmergencyAttempt(bool localTxCompleted) {
  const bool current = coastcue::shouldOpenIncidentResponseWindow(
      boatEmergencyTransmitIncidentId.c_str(), boatIncidentState);
  const auto kind = emergencyScheduler.activeKind();
  const uint8_t attempt = emergencyScheduler.attemptCount();
  const auto completion = emergencyScheduler.attemptFinished(millis());
  if (completion == coastcue::BoatEmergencyCompletion::kUpgraded) {
    boatSosIdentityPending = true;
  }
  reportEmergencyCompletion(kind, attempt, localTxCompleted, completion);
  boatEmergencyTransmitIncidentId = "";
  return current;
}

void serviceBoatTransmit() {
  if (!loraTransmitInFlight) return;
  if (!loraTransmitDone &&
      millis() - loraTransmitStartedAt >= LORA_TX_TIMEOUT_MS) {
    const int finishState = loraRadio.finishTransmit();
    loraTransmitDone = false;
    loraTransmitInFlight = false;
    if (finishState != RADIOLIB_ERR_NONE) {
      loraReady = false;
      setBlue(false);
      Serial.printf("[LORA] boat TX timeout cleanup failed: %d\n",
                    finishState);
    }
    if (boatStateReceiptInFlight) {
      boatStateReceiptInFlight = false;
      boatStateReceiptDueAt = millis() + 1000;
      return;
    }
    if (normalReportScheduler.normalReportInFlight()) {
      normalReportScheduler.normalReportFinished(false, millis());
    }
    if (emergencyScheduler.attemptInFlight()) {
      finishEmergencyAttempt(false);
    } else {
      showEvent("LoRa TX timeout; retrying");
    }
    return;
  }
  if (!loraTransmitDone) return;
  loraTransmitDone = false;
  loraTransmitInFlight = false;
  const int state = loraRadio.finishTransmit();
  if (state != RADIOLIB_ERR_NONE) {
    if (boatStateReceiptInFlight) {
      boatStateReceiptInFlight = false;
      boatStateReceiptDueAt = millis() + 1000;
    }
    loraReady = false;
    setBlue(false);
    showEvent("LoRa TX failed; retrying");
    if (normalReportScheduler.normalReportInFlight()) {
      normalReportScheduler.normalReportFinished(false, millis());
    }
    if (emergencyScheduler.attemptInFlight()) finishEmergencyAttempt(false);
    return;
  }
  if (boatStateReceiptInFlight) {
    boatStateReceiptInFlight = false;
    boatStateReceiptPending = false;
    Serial.printf("[LORA] boat state receipt incident=%s seq=%lu\n",
                  boatStateReceiptIncidentId.c_str(),
                  static_cast<unsigned long>(boatStateReceiptSeq));
    return;
  }
  bool openResponseWindow = false;
  if (emergencyScheduler.attemptInFlight()) {
    openResponseWindow = finishEmergencyAttempt(true);
    if (!openResponseWindow) {
      Serial.println(
          "[LORA] skipped stale Incident response window after escalation");
    }
  } else if (normalReportScheduler.normalReportInFlight()) {
    normalReportScheduler.normalReportFinished(true, millis());
    openResponseWindow = true;
  }
  if (openResponseWindow) startBoatReceiveWindow();
}
#endif

void setBlue(bool on) { digitalWrite(COASTCUE_PIN_LED_BLUE, on ? HIGH : LOW); }
void setRed(bool on) { digitalWrite(COASTCUE_PIN_LED_RED, on ? HIGH : LOW); }

void showEvent(const String &message) {
  lastEventText = message;
  eventTextUntil = millis() + ALERT_DISPLAY_MS;
}

void startBuzz(uint32_t durationMs, uint32_t frequency) {
  ledcWriteTone(BUZZER_CHANNEL, frequency);
  buzzing = true;
  buzzOffAt = millis() + durationMs;
}

void serviceBuzz() {
  if (buzzing && static_cast<int32_t>(millis() - buzzOffAt) >= 0) {
    ledcWriteTone(BUZZER_CHANNEL, 0);
    buzzing = false;
  }
}

#if !defined(COASTCUE_ROLE_SHORE)
void queueEmergency(const char *status, const char *label) {
  const auto kind = strcmp(status, "sos") == 0
                        ? coastcue::BoatEmergencyKind::kSos
                        : coastcue::BoatEmergencyKind::kHelp;
  const bool startsFreshIncident =
      !boatIncidentState.hasIncident() || boatIncidentState.isTerminal();
  if (!coastcue::shouldQueueBoatEmergencyAction(
          boatIncidentState.hasIncident(), boatIncidentState.isTerminal(),
          boatIncidentState.shouldRetryOpen(), boatIncidentKind, kind)) {
    showEvent(emergencyLabel(kind) + " already delivered");
    return;
  }

  // Keep the old Help identity through its completed uplink's response slot.
  // Replacing it before that window opens would make a valid Receipt for Help
  // look stale and leave a silent gap before the queued SOS can transmit.
  const bool deferSosIdentity =
      kind == coastcue::BoatEmergencyKind::kSos &&
      coastcue::shouldDeferBoatSosIdentityHandoff(
          boatIncidentState.hasIncident() && !boatIncidentState.isTerminal() &&
              boatIncidentKind == coastcue::BoatEmergencyKind::kHelp,
          emergencyScheduler.attemptInFlight(), boatReceiveWindowActive);
  if (deferSosIdentity) {
    const auto result = emergencyScheduler.queue(kind);
    if (coastcue::shouldMarkBoatSosIdentityPending(result)) {
      boatSosIdentityPending = true;
    }
    // Every SOS press gets visible and audible confirmation.
    showEvent(result == coastcue::BoatEmergencyQueueResult::kUpgradePending
                  ? "SOS queued after Help TX"
              : result == coastcue::BoatEmergencyQueueResult::kDuplicate
                  ? "SOS already queued"
                  : "SOS queued after Help reply");
    setRed(true);
    startBuzz(150);
    return;
  }
  beginBoatIncidentIfNeeded(kind);
  const auto result = emergencyScheduler.queue(kind);
  if (result == coastcue::BoatEmergencyQueueResult::kDuplicate) {
    showEvent(emergencyLabel(emergencyScheduler.activeKind()) +
              " already queued");
    return;
  }
  if (result == coastcue::BoatEmergencyQueueResult::kUpgradePending) {
    showEvent("SOS queued after Help TX");
  } else if (result == coastcue::BoatEmergencyQueueResult::kUpgraded) {
    showEvent("SOS replaced queued Help");
  } else if (startsFreshIncident) {
    showEvent(String(label) + " requested");
  } else {
    showEvent(String(label) + " queued");
  }
  setRed(true);
  startBuzz(150);
}

void serviceQueuedEmergency(bool forceAttempt = false) {
  // Pending receipt for the old Help is deliberately not a handoff gate.
  // Starting the higher-severity SOS retires that unsent receipt above; an
  // already transmitting receipt is the only radio operation we must await.
  if (boatSosIdentityPending && !boatReceiveWindowActive &&
      !loraTransmitInFlight && !boatStateReceiptInFlight) {
    boatSosIdentityPending = false;
    beginBoatIncidentIfNeeded(coastcue::BoatEmergencyKind::kSos);
  }
  if (!emergencyScheduler.hasPending() || !loraReady ||
      loraTransmitInFlight || boatReceiveWindowActive ||
      boatStateReceiptInFlight ||
      (boatStateReceiptPending &&
       emergencyScheduler.activeKind() != coastcue::BoatEmergencyKind::kSos)) {
    return;
  }
  const uint32_t now = millis();
  if (!emergencyScheduler.attemptDue(now, SOS_RETRY_MS, forceAttempt)) return;

  const auto kind = emergencyScheduler.activeKind();
  const String label = emergencyLabel(kind);
  String packet;
  if (!buildFittedAuthenticatedPacket(
          emergencyStatus(kind), packet, boatIncidentState.incidentId())) {
    boatEmergencyTransmitIncidentId = "";
    const uint8_t attempt = emergencyScheduler.attemptCount() + 1;
    const auto completion = emergencyScheduler.attemptStartFailed(now);
    showEvent(label + " packet failed " + attempt + "/3");
    Serial.printf("[FATAL] unable to build authenticated %s attempt %u/3\n",
                  label.c_str(), attempt);
    if (completion == coastcue::BoatEmergencyCompletion::kExhausted) {
      Serial.printf("[CoastCue] %s queue released after 3 build failures\n",
                    label.c_str());
    }
    return;
  }
  if (loraTransmitPacket(packet)) {
    boatEmergencyTransmitIncidentId = boatIncidentState.incidentId();
    emergencyScheduler.attemptStarted(now);
    showEvent(label + " broadcasting " +
              emergencyScheduler.attemptCount() + "/3");
  } else {
    boatEmergencyTransmitIncidentId = "";
    const uint8_t attempt = emergencyScheduler.attemptCount() + 1;
    const auto completion = emergencyScheduler.attemptStartFailed(now);
    showEvent(label + " TX start failed " + attempt + "/3");
    if (completion == coastcue::BoatEmergencyCompletion::kExhausted) {
      Serial.printf("[CoastCue] %s queue released after 3 TX start failures\n",
                    label.c_str());
    }
  }
}
#endif

#if !defined(COASTCUE_ROLE_SHORE)
bool sendNormalReport() {
  const bool retryOpenIncident =
      boatIncidentState.shouldRetryOpen() &&
      (boatIncidentKind == coastcue::BoatEmergencyKind::kHelp ||
       boatIncidentKind == coastcue::BoatEmergencyKind::kSos);
  const char *const reportStatus =
      retryOpenIncident ? emergencyStatus(boatIncidentKind) : "normal";
  // While the boat listens, telemetry names its Incident; without it the shore
  // stops replaying revisions nobody will confirm.
  const char *const incidentId = retryOpenIncident || boatAwaitingShore()
                                     ? boatIncidentState.incidentId()
                                     : nullptr;
  String packet;
  if (!buildFittedAuthenticatedPacket(reportStatus, packet, incidentId)) {
    Serial.println(
        "[CoastCue] periodic packet exceeds LoRa budget or MAC failed");
    return false;
  }
  const bool freshGpsFixInPacket = isFreshGpsFix();
  if (!loraTransmitPacket(packet)) {
    return false;
  }
  normalReportScheduler.normalReportStarted(freshGpsFixInPacket);
  if (retryOpenIncident) {
    Serial.printf("[CoastCue] periodic incident retry id=%s severity=%s\n",
                  incidentId, reportStatus);
  }
  return true;
}
#endif

#if !defined(COASTCUE_ROLE_SHORE)
void pumpGps() {
  while (GPS.available()) gps.encode(static_cast<char>(GPS.read()));
}

void printVerboseGps() {
#if COASTCUE_VERBOSE_SERIAL
  const uint32_t now = millis();
  if (now - lastGpsLogAt < GPS_LOG_INTERVAL_MS) return;
  lastGpsLogAt = now;

  const unsigned long chars = gps.charsProcessed();
  const unsigned long satellites =
      gps.satellites.isValid() ? gps.satellites.value() : 0;
  if (chars < 10) {
    Serial.printf("[GPS] data=%s fix=waiting chars=%lu satellites=%lu\n",
                  now >= GPS_NO_DATA_WARN_MS ? "no" : "waiting", chars,
                  satellites);
    return;
  }

  if (isFreshGpsFix()) {
    Serial.printf("[GPS] data=yes fix=fresh satellites=%lu age_ms=%lu\n",
                  satellites,
                  static_cast<unsigned long>(gps.location.age()));
  } else {
    Serial.printf("[GPS] data=yes fix=waiting chars=%lu satellites=%lu\n",
                  chars, satellites);
  }
#endif
}

coastcue::BoatButtonEvent pollButton() {
  const uint32_t now = millis();
  const bool rawLevel = digitalRead(COASTCUE_PIN_BUTTON);
  if (rawLevel != buttonState.rawLevel) {
    buttonState.rawLevel = rawLevel;
    buttonState.rawChangedAt = now;
  }
  if (rawLevel == buttonState.stableLevel) {
    if (coastcue::shouldFireBoatSosWhileHeld(
            buttonState.stableLevel == LOW, buttonState.sosFired,
            now - buttonState.pressedAt)) {
      buttonState.sosFired = true;
      return coastcue::BoatButtonEvent::kSos;
    }
    return coastcue::BoatButtonEvent::kNone;
  }
  if (now - buttonState.rawChangedAt < DEBOUNCE_MS) {
    return coastcue::BoatButtonEvent::kNone;
  }
  buttonState.stableLevel = rawLevel;
  if (rawLevel == LOW) {
    buttonState.pressedAt = now;
    buttonState.sosFired = false;
    return coastcue::BoatButtonEvent::kNone;
  }
  if (buttonState.sosFired) {
    buttonState.sosFired = false;
    return coastcue::BoatButtonEvent::kNone;
  }
  // Help is classified on release so the same physical control can reserve a
  // continued three-second hold for immediate SOS.
  return coastcue::classifyBoatButtonRelease(now - buttonState.pressedAt);
}

void initTBeamPower() {
  pmuReady = coastcuePower.begin(Wire, AXP2101_SLAVE_ADDRESS,
                                COASTCUE_PIN_I2C_SDA, COASTCUE_PIN_I2C_SCL);
  if (!pmuReady) {
    Serial.println("[PMU] AXP2101 not found; LoRa/GPS power rails unconfirmed");
    return;
  }
  coastcuePower.setALDO2Voltage(3300);
  coastcuePower.enableALDO2();
  coastcuePower.setALDO3Voltage(3300);
  coastcuePower.enableALDO3();
  coastcuePower.enableBattDetection();
  coastcuePower.enableVbusVoltageMeasure();
  coastcuePower.enableBattVoltageMeasure();
  coastcuePower.enableSystemVoltageMeasure();
  Serial.println("[PMU] AXP2101 ALDO2 radio and ALDO3 GPS rails enabled");
  vTaskDelay(pdMS_TO_TICKS(100));
}
#endif

void printBootBanner() {
  Serial.printf("[CoastCue] boot role=%s board=%s node_uid=%s boat_id=%s\n",
                IS_SHORE ? "shore" : "boat", COASTCUE_BOARD_LABEL,
                nodeUid.c_str(), BOAT_ID);
  Serial.printf("[CoastCue] transport=LoRa region=US freq=%.1fMHz bw=%.1fkHz sf=%d cr=%d power=%ddBm\n",
                static_cast<double>(COASTCUE_LORA_FREQUENCY_MHZ),
                static_cast<double>(COASTCUE_LORA_BANDWIDTH_KHZ),
                COASTCUE_LORA_SPREADING_FACTOR, COASTCUE_LORA_CODING_RATE,
                COASTCUE_LORA_TX_POWER_DBM);
  Serial.printf("[CoastCue] pmu=%s ready=%s gps_freshness_ms=%lu auth=hmac-sha256-128\n",
                COASTCUE_PMU_LABEL, pmuReady ? "yes" : "no",
                static_cast<unsigned long>(COASTCUE_GPS_FRESHNESS_MS));
}

void drawScreen() {
#if !defined(COASTCUE_ROLE_SHORE)
  if (!displayReady) return;
  static uint32_t lastDrawAt = 0;
  if (millis() - lastDrawAt < 200) return;
  lastDrawAt = millis();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (static_cast<int32_t>(millis() - eventTextUntil) < 0 &&
      lastEventText.length() > 0) {
    display.print(lastEventText);
  } else if (boatIncidentState.hasIncident()) {
    if (boatIncidentCheckpointPending) display.print("SAVE! ");
    display.print(boatIncidentDisplayText(boatIncidentState.state()));
  } else {
    display.print(loraReady ? "LoRa ready" : "LoRa offline");
  }
  display.setCursor(0, 12);
  if (isFreshGpsFix()) {
    display.printf("GPS: %.5f", gps.location.lat());
    display.setCursor(30, 22);  // under the latitude, past "GPS: "
    display.printf("%.5f", gps.location.lng());
  } else {
    display.print("GPS: no fix");
  }
  display.display();
#endif
}

void setup() {
  Serial.begin(115200);
  nodeUid = nodeUidFromEfuseMac();

#if defined(COASTCUE_ROLE_SHORE)
  shoreRuntime.begin(nodeUid, BOAT_ID);
#else
  pinMode(COASTCUE_PIN_LED_BLUE, OUTPUT);
  pinMode(COASTCUE_PIN_LED_RED, OUTPUT);
  pinMode(COASTCUE_PIN_BUTTON, INPUT_PULLUP);
  ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ, BUZZER_RES);
  ledcAttachPin(COASTCUE_PIN_BUZZER, BUZZER_CHANNEL);
  restoreBoatIncident();
  if (boatIncidentState.hasIncident() && !boatIncidentState.isTerminal()) {
    setRed(true);
  }
  initTBeamPower();
  boatHardwareReady = pmuReady;
  if (!boatHardwareReady) {
    setRed(true);
  } else {
    GPS.begin(9600, SERIAL_8N1, COASTCUE_PIN_GPS_RX, COASTCUE_PIN_GPS_TX);
    displayReady = display.begin(SSD1306_SWITCHCAPVCC, COASTCUE_OLED_ADDR);
    if (!displayReady) {
      Serial.println("[WARN] OLED init failed; safety radio remains enabled");
    }
  }
#endif

  printBootBanner();
#if !defined(COASTCUE_ROLE_SHORE)
  if (!boatHardwareReady) {
    Serial.println("[FATAL] PMU not ready; GPS and LoRa startup blocked");
    return;
  }
#endif
  startLoRaTransport();
  lastRadioAttemptAt = millis();
#if !defined(COASTCUE_ROLE_SHORE)
  normalReportScheduler.scheduleFirstPeriodicReport(millis(),
                                                     COASTCUE_REPORT_PERIOD_MS);
#endif
}

void loop() {
#if !defined(COASTCUE_ROLE_SHORE)
  if (!boatHardwareReady) {
    serviceBuzz();
    drawScreen();
    return;
  }

  pumpGps();
  printVerboseGps();
  const coastcue::BoatButtonEvent event = pollButton();
  if (event == coastcue::BoatButtonEvent::kHelp) {
    queueEmergency("help", "Help");
    serviceQueuedEmergency(true);
  } else if (event == coastcue::BoatButtonEvent::kSos) {
    queueEmergency("sos", "SOS");
    serviceQueuedEmergency(true);
  }
  serviceBoatIncidentCheckpoint();
#endif

#if defined(COASTCUE_ROLE_SHORE)
  serviceShoreRadio();
  shoreRuntime.service();
#endif
  if (!loraReady && millis() - lastRadioAttemptAt >= RADIO_RETRY_MS) {
    lastRadioAttemptAt = millis();
    startLoRaTransport();
  }
#if !defined(COASTCUE_ROLE_SHORE)
  serviceBoatTransmit();
  serviceBoatReceiveWindow();
  serviceBoatStateReceipt();
  serviceQueuedEmergency();
  const bool normalReportDue = normalReportScheduler.normalReportDue(
      millis(),
      boatAwaitingShore() ? INCIDENT_REPORT_PERIOD_MS
                          : COASTCUE_REPORT_PERIOD_MS,
      isFreshGpsFix());
  if (loraReady && !loraTransmitInFlight && !boatReceiveWindowActive &&
      !boatStateReceiptPending && !emergencyScheduler.hasPending() &&
      normalReportDue) {
    sendNormalReport();
  }
#endif
  serviceBuzz();
#if !defined(COASTCUE_ROLE_SHORE)
  drawScreen();
#endif
}
