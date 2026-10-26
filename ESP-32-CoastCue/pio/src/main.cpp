#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <RadioLib.h>
#include <esp_system.h>
#include <mbedtls/md.h>

#if __has_include("coastcue_config.h")
#include "coastcue_config.h"
#else
#include "coastcue_config.example.h"
#endif
#include "coastcue_pins.h"
#include "boat_button_logic.h"
#include "boat_emergency_scheduler.h"
#include "boat_incident_snapshot.h"
#include "boat_gps_policy.h"
#include "boat_report_scheduler.h"
#include "gps_diagnostics.h"
#include "incident_protocol.h"
#include "shore_logic.h"

#ifndef COASTCUE_BENCH_BOAT_PERSIST_FAIL_AFTER_SUCCESSES
#define COASTCUE_BENCH_BOAT_PERSIST_FAIL_AFTER_SUCCESSES 0
#endif
#ifndef COASTCUE_BENCH_BOAT_PERSIST_FAILURE_COUNT
#define COASTCUE_BENCH_BOAT_PERSIST_FAILURE_COUNT 0
#endif

#if defined(COASTCUE_ROLE_BOAT) && defined(COASTCUE_ROLE_SHORE)
#error "Select exactly one CoastCue role"
#elif !defined(COASTCUE_ROLE_BOAT) && !defined(COASTCUE_ROLE_SHORE)
#error "Select exactly one CoastCue role"
#endif

#if defined(COASTCUE_ROLE_SHORE) && COASTCUE_BATTERY_ADC_ENABLED
#error "COASTCUE_BATTERY_ADC_ENABLED is supported on the T-Beam boat profile only"
#endif

#if defined(COASTCUE_ROLE_SHORE)
#include "shore_runtime.h"
#else
#include <Preferences.h>
#include <Wire.h>
#include <TinyGPSPlus.h>
#if COASTCUE_OLED_ENABLED
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif
#endif

#if !defined(COASTCUE_ROLE_SHORE) && \
    COASTCUE_BOARD_PROFILE == COASTCUE_BOARD_TBEAM_SX127X && \
    COASTCUE_TBEAM_PMU == COASTCUE_TBEAM_PMU_AXP2101
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#define COASTCUE_PMU_ENABLED 1
#define COASTCUE_PMU_LABEL "AXP2101"
#elif !defined(COASTCUE_ROLE_SHORE) && \
    COASTCUE_BOARD_PROFILE == COASTCUE_BOARD_TBEAM_SX127X && \
    COASTCUE_TBEAM_PMU == COASTCUE_TBEAM_PMU_AXP192
#define XPOWERS_CHIP_AXP192
#include <XPowersLib.h>
#define COASTCUE_PMU_ENABLED 1
#define COASTCUE_PMU_LABEL "AXP192"
#elif !defined(COASTCUE_ROLE_SHORE) && \
    COASTCUE_BOARD_PROFILE == COASTCUE_BOARD_TBEAM_SX127X && \
    COASTCUE_TBEAM_PMU != COASTCUE_TBEAM_PMU_NONE
#error "Unsupported COASTCUE_TBEAM_PMU value for T-Beam SX127x"
#else
#define COASTCUE_PMU_ENABLED 0
#define COASTCUE_PMU_LABEL "none"
#endif

#ifndef COASTCUE_TRANSPORT_LORA
#define COASTCUE_TRANSPORT_LORA 1
#endif
#if !COASTCUE_TRANSPORT_LORA
#error "CoastCue firmware supports LoRa transport only."
#endif
#ifndef COASTCUE_LORA_RADIO_SX1278
#define COASTCUE_LORA_RADIO_SX1278 1
#endif
#ifndef COASTCUE_LORA_FREQUENCY_MHZ
#define COASTCUE_LORA_FREQUENCY_MHZ 433.0
#endif
#ifndef COASTCUE_LORA_BANDWIDTH_KHZ
#define COASTCUE_LORA_BANDWIDTH_KHZ 125.0
#endif
#ifndef COASTCUE_LORA_SPREADING_FACTOR
#define COASTCUE_LORA_SPREADING_FACTOR 9
#endif
#ifndef COASTCUE_LORA_CODING_RATE
#define COASTCUE_LORA_CODING_RATE 7
#endif
#ifndef COASTCUE_LORA_SYNC_WORD
#define COASTCUE_LORA_SYNC_WORD 0x12
#endif
#ifndef COASTCUE_LORA_TX_POWER_DBM
#define COASTCUE_LORA_TX_POWER_DBM 17
#endif
#ifndef COASTCUE_LORA_PREAMBLE_LENGTH
#define COASTCUE_LORA_PREAMBLE_LENGTH 8
#endif
#ifndef COASTCUE_LORA_AUTH_KEY
#define COASTCUE_LORA_AUTH_KEY "replace-with-a-unique-64-lowercase-hex-character-deployment-key"
#endif
#ifndef COASTCUE_GPS_FRESHNESS_MS
#define COASTCUE_GPS_FRESHNESS_MS 30000UL
#endif

#if defined(COASTCUE_ROLE_SHORE)
constexpr bool IS_SHORE = true;
#else
constexpr bool IS_SHORE = false;
#endif

#if defined(COASTCUE_REGION_CN) && defined(COASTCUE_REGION_US)
#error "Select exactly one CoastCue region"
#elif defined(COASTCUE_REGION_CN)
constexpr const char *REGION_LABEL = "CN";
#elif defined(COASTCUE_REGION_US)
constexpr const char *REGION_LABEL = "US";
#else
#error "Select a CoastCue region"
#endif

// The declared CN profile is 10 dBm. Refuse the historical 17 dBm generic
// fallback if somebody creates a CN build outside the reviewed profiles.
#if defined(COASTCUE_REGION_CN) && COASTCUE_LORA_TX_POWER_DBM > 10
#error "CN builds must explicitly select the reviewed 10 dBm LoRa power"
#endif

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
constexpr uint32_t GPS_DIAGNOSTIC_INTERVAL_MS = 5000;
constexpr uint32_t GPS_NO_DATA_WARN_MS = 15000;
// SF11/125 kHz/CR4/8 255-byte frames remain well below this watchdog margin.
constexpr uint32_t LORA_TX_TIMEOUT_MS = 15000;
// A near-maximum authenticated state frame at the CN MVP's SF11 / CR4/8 can
// remain on air for roughly eight seconds. Ten seconds leaves turnaround and
// loop-scheduling margin without keeping the receiver open continuously.
constexpr uint32_t LORA_RESPONSE_WINDOW_MS = 10000;
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

#if defined(COASTCUE_GPS_DIAGNOSTICS)
#ifndef COASTCUE_GPS_DIAGNOSTIC_REPORT_PERIOD_MS
#define COASTCUE_GPS_DIAGNOSTIC_REPORT_PERIOD_MS 15000UL
#endif
constexpr uint32_t GPS_DIAGNOSTIC_BAUD_DWELL_MS = 13000;
constexpr uint32_t GPS_DIAGNOSTIC_BAUDS[] = {
    9600, 4800, 19200, 38400, 57600, 115200, 230400};
constexpr size_t GPS_DIAGNOSTIC_BAUD_COUNT =
    sizeof(GPS_DIAGNOSTIC_BAUDS) / sizeof(GPS_DIAGNOSTIC_BAUDS[0]);
constexpr uint32_t ACTIVE_REPORT_PERIOD_MS =
    COASTCUE_GPS_DIAGNOSTIC_REPORT_PERIOD_MS;
#else
constexpr uint32_t ACTIVE_REPORT_PERIOD_MS = COASTCUE_REPORT_PERIOD_MS;
#endif

const char *const BOAT_ID = COASTCUE_BOAT_ID;

#if COASTCUE_LORA_RADIO_SX1278
SX1278 loraRadio = new Module(COASTCUE_PIN_LORA_NSS, COASTCUE_PIN_LORA_DIO0,
                              COASTCUE_PIN_LORA_RST, COASTCUE_PIN_LORA_DIO1);
#else
SX1276 loraRadio = new Module(COASTCUE_PIN_LORA_NSS, COASTCUE_PIN_LORA_DIO0,
                              COASTCUE_PIN_LORA_RST, COASTCUE_PIN_LORA_DIO1);
#endif

#if COASTCUE_PMU_ENABLED
XPowersPMU coastcuePower;
#endif

#if !defined(COASTCUE_ROLE_SHORE)
HardwareSerial GPS(1);
TinyGPSPlus gps;
#if COASTCUE_OLED_ENABLED
Adafruit_SSD1306 display(COASTCUE_OLED_WIDTH, COASTCUE_OLED_HEIGHT, &Wire,
                         COASTCUE_OLED_RESET);
#endif
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
bool displayReady = !COASTCUE_OLED_ENABLED;
bool buzzing = false;
uint32_t buzzOffAt = 0;
uint32_t lastRadioAttemptAt = 0;
uint32_t lastGpsDiagnosticAt = 0;
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
uint32_t boatIncidentPersistSuccessCount = 0;
uint32_t boatIncidentInjectedFailureCount = 0;
#endif

#if defined(COASTCUE_GPS_DIAGNOSTICS)
coastcue::GpsStreamDiagnostics gpsStreamDiagnostics;
coastcue::GpsDiagnosticSnapshot gpsQuietSnapshot;
coastcue::GpsDiagnosticSnapshot gpsActiveSnapshot;
coastcue::GpsDiagnosticSnapshot gpsConfirmSnapshot;
coastcue::GpsDiagnosticPhase gpsDiagnosticPhase =
    coastcue::GpsDiagnosticPhase::kScan;
Preferences gpsDiagnosticPreferences;
uint32_t gpsDiagnosticBauds[GPS_DIAGNOSTIC_BAUD_COUNT] = {0};
size_t gpsDiagnosticBaudIndex = 0;
uint32_t gpsDiagnosticBaudStartedAt = 0;
uint32_t gpsDiagnosticAcquisitionStartedAt = 0;
uint32_t gpsDiagnosticSelectedBaud = 0;
uint32_t gpsDiagnosticLastTxAt = 0;
uint8_t gpsDiagnosticFinalTxCount = 0;
bool gpsDiagnosticScanning = true;
bool gpsDiagnosticTxPending = false;
bool gpsDiagnosticFinalSaved = false;
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
#if defined(COASTCUE_GPS_DIAGNOSTICS)
String buildGpsDiagnosticCompact(bool terse);
#endif

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

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool decodeAuthKey(uint8_t keyBytes[32]) {
  const char *key = COASTCUE_LORA_AUTH_KEY;
  if (strlen(key) != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int high = hexValue(key[i * 2]);
    const int low = hexValue(key[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    keyBytes[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool hmacSha256TruncatedHex(const String &unsignedPacket, String &macHex) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) {
    return false;
  }

  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  uint8_t digest[32] = {0};
  uint8_t keyBytes[32] = {0};
  if (!decodeAuthKey(keyBytes)) {
    mbedtls_md_free(&context);
    return false;
  }
  const int setupResult = mbedtls_md_setup(&context, info, 1);
  const int startResult = setupResult == 0
                              ? mbedtls_md_hmac_starts(
                                    &context,
                                    keyBytes, sizeof(keyBytes))
                              : -1;
  const int updateResult = startResult == 0
                               ? mbedtls_md_hmac_update(
                                     &context,
                                     reinterpret_cast<const unsigned char *>(
                                         unsignedPacket.c_str()),
                                     unsignedPacket.length())
                               : -1;
  const int finishResult = updateResult == 0
                               ? mbedtls_md_hmac_finish(&context, digest)
                               : -1;
  mbedtls_md_free(&context);
  if (finishResult != 0) {
    return false;
  }

  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  char encoded[33];
  for (size_t i = 0; i < 16; ++i) {
    encoded[i * 2] = HEX_DIGITS[(digest[i] >> 4) & 0x0F];
    encoded[i * 2 + 1] = HEX_DIGITS[digest[i] & 0x0F];
  }
  encoded[32] = '\0';
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

bool isFreshGpsFix() {
#if defined(COASTCUE_ROLE_SHORE)
  return false;
#else
  return gps.location.isValid() &&
         gps.location.age() <= static_cast<uint32_t>(COASTCUE_GPS_FRESHNESS_MS);
#endif
}

int readBatteryPercent() {
#if COASTCUE_PMU_ENABLED
  if (pmuReady && coastcuePower.isBatteryConnect()) {
    const int batteryPercent = coastcuePower.getBatteryPercent();
    if (batteryPercent >= 0 && batteryPercent <= 100) {
      return batteryPercent;
    }
  }
#endif

#if COASTCUE_BATTERY_ADC_ENABLED
  const int raw = analogRead(COASTCUE_PIN_BATTERY_ADC);
  const float pinMv = (raw * 3300.0f) / 4095.0f;
  const uint32_t cellMv = static_cast<uint32_t>(
      pinMv * COASTCUE_BATTERY_DIVIDER_RATIO);
  if (cellMv <= COASTCUE_BATTERY_MIN_MV) return 0;
  if (cellMv >= COASTCUE_BATTERY_MAX_MV) return 100;
  return static_cast<int>((cellMv - COASTCUE_BATTERY_MIN_MV) * 100 /
                          (COASTCUE_BATTERY_MAX_MV - COASTCUE_BATTERY_MIN_MV));
#else
  return -1;
#endif
}

// Wire format is deterministic: unsigned JSON ends in '}', HMAC input is those
// exact UTF-8 bytes, and the authenticated wire packet replaces that final '}'
// with ,"mac":"<32 lowercase hex chars>"}. The mac field is always last.
bool buildAuthenticatedPacket(const char *status, int batteryPercent,
                              String &wirePacket,
                              bool includeGpsDiagnostic = true,
                              const char *incidentId = nullptr) {
#if defined(COASTCUE_ROLE_SHORE)
  (void)status;
  (void)batteryPercent;
  (void)wirePacket;
  (void)includeGpsDiagnostic;
  (void)incidentId;
  return false;
#else
  const bool emergency =
      strcmp(status, "help") == 0 || strcmp(status, "sos") == 0;
  const bool incident = coastcue::isValidIncidentId(incidentId);
  if (emergency != incident) return false;
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
#if defined(COASTCUE_GPS_DIAGNOSTICS)
  // A valid fix is already most of the evidence, so use a two-byte diagnostic
  // code there. Invalid reports carry the full compact observation set.
  if (includeGpsDiagnostic) {
    unsignedPacket += ",\"diag\":\"";
    unsignedPacket += buildGpsDiagnosticCompact(gpsPolicy.fresh);
    unsignedPacket += "\"";
  }
#else
  (void)includeGpsDiagnostic;
#endif
  unsignedPacket += "}";

  if (batteryPercent >= 0) {
    unsignedPacket += ",\"battery_percent\":";
    unsignedPacket += String(batteryPercent);
  }
  unsignedPacket += "}";

  return authenticateUnsignedPacket(unsignedPacket, wirePacket);
#endif
}

bool packetFitsTransport(const String &packet) {
  return packet.length() <= LORA_MAX_PACKET_BYTES;
}

bool buildFittedAuthenticatedPacket(const char *status, String &wirePacket,
                                    const char *incidentId = nullptr) {
  const int batteryPercent = readBatteryPercent();
  if (!buildAuthenticatedPacket(status, batteryPercent, wirePacket, true,
                                incidentId)) {
    return false;
  }
  if (packetFitsTransport(wirePacket)) {
    return true;
  }

  // A fresh GPS report is the critical payload. If optional battery telemetry
  // pushes it over the SX127x 255-byte ceiling, rebuild without battery.
  if (batteryPercent >= 0) {
    if (!buildAuthenticatedPacket(status, -1, wirePacket, true,
                                  incidentId)) {
      return false;
    }
    if (packetFitsTransport(wirePacket)) {
      return true;
    }
  }
#if defined(COASTCUE_GPS_DIAGNOSTICS)
  // Keep last-known emergency coordinates and their age ahead of optional
  // diagnostic telemetry when the SX127x payload reaches 255 bytes.
  if (buildAuthenticatedPacket(status, -1, wirePacket, false, incidentId) &&
      packetFitsTransport(wirePacket)) {
    return true;
  }
#endif
  return false;
}

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

void printRemoteGpsDiagnostic(const String &wirePacket) {
  // Normal production packets have no diagnostic field. Avoid a second JSON
  // parse on every uplink while preserving the opt-in GPS diagnostic profile.
  if (wirePacket.indexOf("\"diag\":") < 0) return;
  JsonDocument document;
  if (deserializeJson(document, wirePacket) != DeserializationError::Ok) return;
  const char *compact = document["gps"]["diag"].as<const char *>();
  if (compact == nullptr) return;

  const auto phaseName = [](char code) {
    return code == 'S' ? "scan"
           : code == 'Q' ? "quiet"
           : code == 'A' ? "active"
           : code == 'C' ? "confirm"
           : code == 'D' ? "done"
                         : "unknown";
  };
  if (strlen(compact) == 2) {
    const JsonObjectConst gps = document["gps"].as<JsonObjectConst>();
    double latitude = 0.0;
    double longitude = 0.0;
    int64_t ageSeconds = 0;
    if ((gps["valid"] | false) &&
        readJsonNumber(gps["latitude"], latitude) &&
        readJsonNumber(gps["longitude"], longitude) &&
        readJsonInteger(gps["age_s"], ageSeconds) && ageSeconds >= 0) {
      Serial.printf(
          "[GPS-DIAG] remote phase=%s classification=%s fix=yes "
          "latitude=%.6f longitude=%.6f age_s=%lld\n",
          phaseName(compact[0]),
          coastcue::gpsDiagnosticClassificationName(compact[1]), latitude,
          longitude, static_cast<long long>(ageSeconds));
    } else {
      Serial.printf(
          "[GPS-DIAG] remote phase=%s classification=%s "
          "fix=yes coordinates=unavailable\n",
          phaseName(compact[0]),
          coastcue::gpsDiagnosticClassificationName(compact[1]));
    }
    return;
  }

  char phaseCode = 'X';
  char protocolCode = 'X';
  char classificationCode = 'X';
  unsigned long baud = 0;
  int visible = -1;
  int used = -1;
  int maxCn0 = -1;
  unsigned long nmeaPassed = 0;
  unsigned long nmeaFailed = 0;
  int railEnabled = 0;
  int railMv = -1;
  int batteryMv = -1;
  const int fields = sscanf(
      compact, "%c|%c|%lu|%d|%d|%d|%lu|%lu|%d|%d|%d|%c",
      &phaseCode, &protocolCode, &baud, &visible, &used, &maxCn0,
      &nmeaPassed, &nmeaFailed, &railEnabled, &railMv, &batteryMv,
      &classificationCode);
  if (fields != 12) {
    Serial.printf("[GPS-DIAG] remote malformed=%s\n", compact);
    return;
  }
  const char *protocol = protocolCode == 'N' ? "NMEA"
                         : protocolCode == 'U' ? "UBX"
                         : protocolCode == 'M' ? "MIXED"
                                               : "UNKNOWN";
  Serial.printf(
      "[GPS-DIAG] remote phase=%s classification=%s protocol=%s "
      "baud=%lu visible=%d used=%d "
      "max_cn0=%d nmea_ok=%lu nmea_bad=%lu aldo3=%s "
      "aldo3_configured_mv=%d battery_mv=%d\n",
      phaseName(phaseCode),
      coastcue::gpsDiagnosticClassificationName(classificationCode), protocol,
      baud, visible, used, maxCn0, nmeaPassed, nmeaFailed,
      railEnabled ? "on" : "off", railMv, batteryMv);
}

bool acceptFreshNonce(const String &wirePacket) {
  return shoreRuntime.acceptNonce(wirePacket);
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

#if defined(ESP32)
void IRAM_ATTR onLoraPacketReady() { loraPacketReady = true; }
void IRAM_ATTR onLoraTransmitDone() { loraTransmitDone = true; }
#else
void onLoraPacketReady() { loraPacketReady = true; }
void onLoraTransmitDone() { loraTransmitDone = true; }
#endif

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
  if (!acceptFreshNonce(packet)) {
    Serial.println("[LORA] invalid or replayed packet dropped");
    return false;
  }
#if COASTCUE_VERBOSE_SERIAL
  // Raw authenticated payloads are opt-in bench evidence, never production
  // heartbeat chatter.
  Serial.println(packet);
#endif
  if (kind != RadioUplinkKind::kStateReceipt) {
    printRemoteGpsDiagnostic(packet);
  }
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
  if (!loraPacketReady) return;

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
#if COASTCUE_BENCH_BOAT_PERSIST_FAILURE_COUNT > 0
  if (boatIncidentPersistSuccessCount >=
          COASTCUE_BENCH_BOAT_PERSIST_FAIL_AFTER_SUCCESSES &&
      boatIncidentInjectedFailureCount <
          COASTCUE_BENCH_BOAT_PERSIST_FAILURE_COUNT) {
    ++boatIncidentInjectedFailureCount;
    Serial.println(
        "[BENCH-FAULT] injected boat Incident checkpoint failure");
    return false;
  }
#endif
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
  if (!stored) {
    Serial.println("[CoastCue] incident checkpoint write failed");
  } else {
    ++boatIncidentPersistSuccessCount;
  }
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
#if COASTCUE_BENCH_BOAT_PERSIST_FAILURE_COUNT > 0
  // A restored snapshot is the successful write that arms the documented
  // post-reboot fault drill. Without this fault-profile-only seed, the first
  // SOS escalation after reboot would succeed instead of exercising rollback
  // and the five-second checkpoint retry.
  boatIncidentPersistSuccessCount =
      COASTCUE_BENCH_BOAT_PERSIST_FAIL_AFTER_SUCCESSES;
#endif
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

void startBoatReceiveWindow() {
  if (!loraReady || loraTransmitInFlight ||
      !boatIncidentState.hasIncident() ||
      (boatIncidentState.isTerminal() &&
       !boatTerminalReconciliation.shouldListen())) {
    return;
  }
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
      const auto kind = emergencyScheduler.activeKind();
      const uint8_t attempt = emergencyScheduler.attemptCount();
      const auto completion = emergencyScheduler.attemptFinished(millis());
      if (completion == coastcue::BoatEmergencyCompletion::kUpgraded) {
        boatSosIdentityPending = true;
      }
      reportEmergencyCompletion(kind, attempt, false, completion);
      boatEmergencyTransmitIncidentId = "";
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
    if (emergencyScheduler.attemptInFlight()) {
      const auto kind = emergencyScheduler.activeKind();
      const uint8_t attempt = emergencyScheduler.attemptCount();
      const auto completion = emergencyScheduler.attemptFinished(millis());
      if (completion == coastcue::BoatEmergencyCompletion::kUpgraded) {
        boatSosIdentityPending = true;
      }
      reportEmergencyCompletion(kind, attempt, false, completion);
      boatEmergencyTransmitIncidentId = "";
    }
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
    const bool responseMatchesCurrentIncident =
        coastcue::shouldOpenIncidentResponseWindow(
            boatEmergencyTransmitIncidentId.c_str(), boatIncidentState);
    const auto kind = emergencyScheduler.activeKind();
    const uint8_t attempt = emergencyScheduler.attemptCount();
    const auto completion = emergencyScheduler.attemptFinished(millis());
    if (completion == coastcue::BoatEmergencyCompletion::kUpgraded) {
      boatSosIdentityPending = true;
    }
    reportEmergencyCompletion(kind, attempt, true, completion);
    openResponseWindow = responseMatchesCurrentIncident;
    if (!responseMatchesCurrentIncident) {
      Serial.println(
          "[LORA] skipped stale Incident response window after escalation");
    }
    boatEmergencyTransmitIncidentId = "";
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
    if (result == coastcue::BoatEmergencyQueueResult::kUpgradePending) {
      showEvent("SOS queued after Help TX");
    } else if (result == coastcue::BoatEmergencyQueueResult::kUpgraded) {
      showEvent("SOS queued after Help reply");
    } else if (result == coastcue::BoatEmergencyQueueResult::kQueued) {
      // A completed Help burst can leave its receive window open without a
      // scheduler entry.  Queueing SOS is valid here, but it must still give
      // the person holding the button immediate visible/audible confirmation.
      showEvent("SOS queued after Help reply");
    } else {
      // Defensive: this state should not normally occur while the active
      // Incident is still Help, but a repeated SOS request must never look
      // like an ignored press.
      showEvent("SOS already queued");
    }
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

#if defined(COASTCUE_GPS_DIAGNOSTICS)
bool gpsDiagnosticRailEnabled() {
#if COASTCUE_TBEAM_PMU == COASTCUE_TBEAM_PMU_AXP2101
  return pmuReady && coastcuePower.isEnableALDO3();
#else
  return pmuReady;
#endif
}

int gpsDiagnosticRailConfiguredMv() {
#if COASTCUE_TBEAM_PMU == COASTCUE_TBEAM_PMU_AXP2101
  return pmuReady ? coastcuePower.getALDO3Voltage() : -1;
#else
  return -1;
#endif
}

int gpsDiagnosticBatteryMv() {
#if COASTCUE_PMU_ENABLED
  return pmuReady && coastcuePower.isBatteryConnect()
             ? coastcuePower.getBattVoltage()
             : -1;
#else
  return -1;
#endif
}

coastcue::GpsDiagnosticSnapshot currentGpsDiagnosticSnapshot(uint32_t now) {
  if (gpsDiagnosticPhase == coastcue::GpsDiagnosticPhase::kDone) {
    return gpsConfirmSnapshot;
  }
  return gpsStreamDiagnostics.snapshot(now);
}

const char *currentGpsDiagnosticClassification() {
  if (gpsDiagnosticPhase == coastcue::GpsDiagnosticPhase::kDone) {
    return coastcue::classifyGpsDiagnostic(
        gpsQuietSnapshot, gpsActiveSnapshot, gpsConfirmSnapshot,
        gpsDiagnosticRailEnabled(), gpsDiagnosticRailConfiguredMv());
  }
  return "PENDING";
}

String buildGpsDiagnosticCompact(bool terse) {
  const coastcue::GpsDiagnosticSnapshot snapshot =
      currentGpsDiagnosticSnapshot(millis());
  const char classificationCode = coastcue::gpsDiagnosticClassificationCode(
      currentGpsDiagnosticClassification());
  if (terse) {
    char compact[3] = {
        coastcue::gpsDiagnosticPhaseCode(gpsDiagnosticPhase),
        classificationCode, '\0'};
    return String(compact);
  }

  char compact[84];
  snprintf(compact, sizeof(compact),
           "%c|%c|%lu|%d|%d|%d|%lu|%lu|%d|%d|%d|%c",
           coastcue::gpsDiagnosticPhaseCode(gpsDiagnosticPhase),
           coastcue::gpsProtocolCode(snapshot.protocol),
           static_cast<unsigned long>(gpsDiagnosticSelectedBaud),
           snapshot.visibleSatellites, snapshot.usedSatellites,
           snapshot.maxCn0DbHz,
           static_cast<unsigned long>(snapshot.nmeaPassed),
           static_cast<unsigned long>(snapshot.nmeaFailed),
           gpsDiagnosticRailEnabled() ? 1 : 0,
           gpsDiagnosticRailConfiguredMv(), gpsDiagnosticBatteryMv(),
           classificationCode);
  return String(compact);
}

void printGpsDiagnosticSnapshot(const char *label,
                                const coastcue::GpsDiagnosticSnapshot &snapshot) {
  Serial.printf(
      "[GPS-DIAG] %s phase=%s baud=%lu protocol=%s bytes=%lu "
      "nmea_ok=%lu nmea_bad=%lu ubx_ok=%lu ubx_bad=%lu "
      "gsv=%lu visible=%d min_visible=%d used=%d max_cn0=%d "
      "min_gsv_max_cn0=%d fix_quality=%d fix_type=%d rmc=%c "
      "hdop_x100=%d max_gap_ms=%lu\n",
      label, coastcue::gpsDiagnosticPhaseName(gpsDiagnosticPhase),
      static_cast<unsigned long>(gpsDiagnosticSelectedBaud),
      coastcue::gpsProtocolName(snapshot.protocol),
      static_cast<unsigned long>(snapshot.bytes),
      static_cast<unsigned long>(snapshot.nmeaPassed),
      static_cast<unsigned long>(snapshot.nmeaFailed),
      static_cast<unsigned long>(snapshot.ubxPassed),
      static_cast<unsigned long>(snapshot.ubxFailed),
      static_cast<unsigned long>(snapshot.gsvSentences),
      snapshot.visibleSatellites, snapshot.minVisibleSatellites,
      snapshot.usedSatellites, snapshot.maxCn0DbHz,
      snapshot.minGsvMaxCn0DbHz, snapshot.fixQuality, snapshot.fixType,
      snapshot.rmcStatus, snapshot.hdopHundredths,
      static_cast<unsigned long>(snapshot.maxByteGapMs));
#if COASTCUE_TBEAM_PMU == COASTCUE_TBEAM_PMU_AXP2101
  Serial.printf(
      "[GPS-DIAG] power aldo3_enabled=%s aldo3_configured_mv=%d "
      "battery_mv=%d system_mv=%u vbus_mv=%u\n",
      gpsDiagnosticRailEnabled() ? "yes" : "no",
      gpsDiagnosticRailConfiguredMv(), gpsDiagnosticBatteryMv(),
      pmuReady ? coastcuePower.getSystemVoltage() : 0,
      pmuReady ? coastcuePower.getVbusVoltage() : 0);
#endif
  if (snapshot.nmeaPassed > 0 && gpsStreamDiagnostics.lastNmea()[0] != '\0') {
    Serial.printf("[GPS-DIAG] last_nmea=%s\n",
                  gpsStreamDiagnostics.lastNmea());
  }
}

void startGpsDiagnosticBaud(uint32_t baud) {
  GPS.end();
  delay(20);
  GPS.begin(baud, SERIAL_8N1, COASTCUE_PIN_GPS_RX, COASTCUE_PIN_GPS_TX);
  gps = TinyGPSPlus();
  gpsStreamDiagnostics.reset();
  gpsDiagnosticBaudStartedAt = millis();
  gpsDiagnosticSelectedBaud = baud;
  Serial.printf("[GPS-DIAG] probing baud=%lu for %lu ms\n",
                static_cast<unsigned long>(baud),
                static_cast<unsigned long>(GPS_DIAGNOSTIC_BAUD_DWELL_MS));
}

void lockGpsDiagnosticBaud(uint32_t now, bool protocolDetected = true) {
  const coastcue::GpsDiagnosticSnapshot snapshot =
      gpsStreamDiagnostics.snapshot(now);
  Serial.printf("[GPS-DIAG] baud %s baud=%lu protocol=%s\n",
                protocolDetected ? "locked" : "fallback",
                static_cast<unsigned long>(gpsDiagnosticSelectedBaud),
                coastcue::gpsProtocolName(snapshot.protocol));
  if (protocolDetected) {
    gpsDiagnosticPreferences.putUInt("baud", gpsDiagnosticSelectedBaud);
  }
  gpsDiagnosticScanning = false;
  gpsDiagnosticAcquisitionStartedAt = now;
  gpsDiagnosticPhase = coastcue::GpsDiagnosticPhase::kQuiet;
  gpsDiagnosticTxPending = false;
  gpsDiagnosticLastTxAt = 0;
  gpsDiagnosticFinalTxCount = 0;
  gpsDiagnosticFinalSaved = false;
  gps = TinyGPSPlus();
  gpsStreamDiagnostics.reset();
  Serial.println(
      "[GPS-DIAG] phase=quiet duration_s=180 LoRa periodic TX suppressed");
}

void beginGpsDiagnostics() {
  gpsDiagnosticPreferences.begin("ccgpsdiag", false);
  const String previous = gpsDiagnosticPreferences.getString("last", "");
  if (!previous.isEmpty()) {
    Serial.printf("[GPS-DIAG] previous_result %s\n", previous.c_str());
  }

  for (size_t index = 0; index < GPS_DIAGNOSTIC_BAUD_COUNT; ++index) {
    gpsDiagnosticBauds[index] = GPS_DIAGNOSTIC_BAUDS[index];
  }
  const uint32_t savedBaud = gpsDiagnosticPreferences.getUInt("baud", 0);
  for (size_t index = 1; index < GPS_DIAGNOSTIC_BAUD_COUNT; ++index) {
    if (gpsDiagnosticBauds[index] == savedBaud) {
      const uint32_t first = gpsDiagnosticBauds[0];
      gpsDiagnosticBauds[0] = savedBaud;
      gpsDiagnosticBauds[index] = first;
      break;
    }
  }
  gpsDiagnosticBaudIndex = 0;
  gpsDiagnosticScanning = true;
  gpsDiagnosticPhase = coastcue::GpsDiagnosticPhase::kScan;
  startGpsDiagnosticBaud(gpsDiagnosticBauds[0]);
}

void serviceGpsDiagnosticBaudScan(uint32_t now) {
  const coastcue::GpsDiagnosticSnapshot snapshot =
      gpsStreamDiagnostics.snapshot(now);
  if (snapshot.nmeaPassed >= 2 || snapshot.ubxPassed >= 1) {
    lockGpsDiagnosticBaud(now);
    return;
  }
  if (now - gpsDiagnosticBaudStartedAt < GPS_DIAGNOSTIC_BAUD_DWELL_MS) return;

  printGpsDiagnosticSnapshot("baud_probe", snapshot);
  ++gpsDiagnosticBaudIndex;
  if (gpsDiagnosticBaudIndex < GPS_DIAGNOSTIC_BAUD_COUNT) {
    startGpsDiagnosticBaud(gpsDiagnosticBauds[gpsDiagnosticBaudIndex]);
    return;
  }

  Serial.println(
      "[GPS-DIAG] no valid NMEA or UBX at supported baud rates; "
      "falling back to 9600 for the field phases");
  startGpsDiagnosticBaud(9600);
  lockGpsDiagnosticBaud(now, false);
}

void saveGpsDiagnosticFinalResult() {
  const char *classification = coastcue::classifyGpsDiagnostic(
      gpsQuietSnapshot, gpsActiveSnapshot, gpsConfirmSnapshot,
      gpsDiagnosticRailEnabled(), gpsDiagnosticRailConfiguredMv());
  String result = "classification=";
  result += classification;
  result += " baud=";
  result += String(gpsDiagnosticSelectedBaud);
  result += " quiet_view=";
  result += String(gpsQuietSnapshot.visibleSatellites);
  result += " active_view=";
  result += String(gpsActiveSnapshot.visibleSatellites);
  result += " confirm_view=";
  result += String(gpsConfirmSnapshot.visibleSatellites);
  result += " quiet_min_view=";
  result += String(gpsQuietSnapshot.minVisibleSatellites);
  result += " active_min_view=";
  result += String(gpsActiveSnapshot.minVisibleSatellites);
  result += " confirm_min_view=";
  result += String(gpsConfirmSnapshot.minVisibleSatellites);
  result += " quiet_cn0=";
  result += String(gpsQuietSnapshot.maxCn0DbHz);
  result += " active_cn0=";
  result += String(gpsActiveSnapshot.maxCn0DbHz);
  result += " confirm_cn0=";
  result += String(gpsConfirmSnapshot.maxCn0DbHz);
  result += " quiet_min_peak_cn0=";
  result += String(gpsQuietSnapshot.minGsvMaxCn0DbHz);
  result += " active_min_peak_cn0=";
  result += String(gpsActiveSnapshot.minGsvMaxCn0DbHz);
  result += " confirm_min_peak_cn0=";
  result += String(gpsConfirmSnapshot.minGsvMaxCn0DbHz);
  result += " q_ok_bad=";
  result += String(gpsQuietSnapshot.nmeaPassed);
  result += "/";
  result += String(gpsQuietSnapshot.nmeaFailed);
  result += " a_ok_bad=";
  result += String(gpsActiveSnapshot.nmeaPassed);
  result += "/";
  result += String(gpsActiveSnapshot.nmeaFailed);
  result += " c_ok_bad=";
  result += String(gpsConfirmSnapshot.nmeaPassed);
  result += "/";
  result += String(gpsConfirmSnapshot.nmeaFailed);
  result += " aldo3=";
  result += gpsDiagnosticRailEnabled() ? "on" : "off";
  result += " aldo3_mv=";
  result += String(gpsDiagnosticRailConfiguredMv());
  gpsDiagnosticPreferences.putString("last", result);
  gpsDiagnosticFinalSaved = true;
  Serial.printf("[GPS-DIAG] FINAL %s\n", result.c_str());
}

void serviceGpsDiagnosticPhases(uint32_t now) {
  if (gpsDiagnosticScanning) {
    serviceGpsDiagnosticBaudScan(now);
    return;
  }

  const coastcue::GpsDiagnosticPhase nextPhase =
      coastcue::gpsDiagnosticPhaseForElapsed(now - gpsDiagnosticAcquisitionStartedAt);
  if (nextPhase != gpsDiagnosticPhase) {
    const coastcue::GpsDiagnosticSnapshot completed =
        gpsStreamDiagnostics.snapshot(now);
    if (gpsDiagnosticPhase == coastcue::GpsDiagnosticPhase::kQuiet) {
      gpsQuietSnapshot = completed;
    } else if (gpsDiagnosticPhase == coastcue::GpsDiagnosticPhase::kActive) {
      gpsActiveSnapshot = completed;
    } else if (gpsDiagnosticPhase == coastcue::GpsDiagnosticPhase::kConfirm) {
      gpsConfirmSnapshot = completed;
    }
    printGpsDiagnosticSnapshot("phase_complete", completed);
    gpsDiagnosticPhase = nextPhase;
    if (nextPhase != coastcue::GpsDiagnosticPhase::kDone) {
      gpsStreamDiagnostics.reset();
    }
    gpsDiagnosticTxPending = nextPhase != coastcue::GpsDiagnosticPhase::kQuiet;
    Serial.printf("[GPS-DIAG] phase=%s\n",
                  coastcue::gpsDiagnosticPhaseName(nextPhase));
    if (nextPhase == coastcue::GpsDiagnosticPhase::kDone &&
        !gpsDiagnosticFinalSaved) {
      saveGpsDiagnosticFinalResult();
    }
  }

  if (gpsDiagnosticPhase == coastcue::GpsDiagnosticPhase::kDone) return;

  if (now - lastGpsDiagnosticAt >= GPS_DIAGNOSTIC_INTERVAL_MS) {
    lastGpsDiagnosticAt = now;
    printGpsDiagnosticSnapshot("live", gpsStreamDiagnostics.snapshot(now));
  }
}

bool gpsDiagnosticTransmitDue(uint32_t now) {
  if (gpsDiagnosticScanning ||
      gpsDiagnosticPhase == coastcue::GpsDiagnosticPhase::kQuiet) {
    return false;
  }
  if (gpsDiagnosticPhase == coastcue::GpsDiagnosticPhase::kDone &&
      gpsDiagnosticFinalTxCount >= 3) {
    return false;
  }
  return gpsDiagnosticTxPending ||
         now - gpsDiagnosticLastTxAt >= ACTIVE_REPORT_PERIOD_MS;
}

void gpsDiagnosticTransmitStarted(uint32_t now) {
  gpsDiagnosticTxPending = false;
  gpsDiagnosticLastTxAt = now;
  if (gpsDiagnosticPhase == coastcue::GpsDiagnosticPhase::kDone) {
    ++gpsDiagnosticFinalTxCount;
  }
}
#endif

bool sendNormalReport() {
#if defined(COASTCUE_ROLE_SHORE)
  return false;
#else
  const bool retryOpenIncident =
      coastcue::shouldSendOpenIncidentOnPeriodicSlot(boatIncidentState) &&
      (boatIncidentKind == coastcue::BoatEmergencyKind::kHelp ||
       boatIncidentKind == coastcue::BoatEmergencyKind::kSos);
  const char *const reportStatus =
      retryOpenIncident ? emergencyStatus(boatIncidentKind) : "normal";
  const char *const incidentId =
      retryOpenIncident ? boatIncidentState.incidentId() : nullptr;
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
#if defined(COASTCUE_GPS_DIAGNOSTICS)
  const coastcue::GpsDiagnosticSnapshot snapshot =
      currentGpsDiagnosticSnapshot(millis());
  Serial.printf(
      "[GPS-DIAG] tx valid=%s phase=%s visible=%d used=%d max_cn0=%d "
      "nmea_ok=%lu nmea_bad=%lu bytes=%u\n",
                freshGpsFixInPacket ? "yes" : "no",
                coastcue::gpsDiagnosticPhaseName(gpsDiagnosticPhase),
                snapshot.visibleSatellites, snapshot.usedSatellites,
                snapshot.maxCn0DbHz,
                static_cast<unsigned long>(snapshot.nmeaPassed),
                static_cast<unsigned long>(snapshot.nmeaFailed),
                static_cast<unsigned int>(packet.length()));
#endif
  normalReportScheduler.normalReportStarted(freshGpsFixInPacket);
  if (retryOpenIncident) {
    Serial.printf("[CoastCue] periodic incident retry id=%s severity=%s\n",
                  incidentId, reportStatus);
  }
  return true;
#endif
}

#if !defined(COASTCUE_ROLE_SHORE)
void pumpGps() {
  while (GPS.available()) {
    const uint8_t byte = static_cast<uint8_t>(GPS.read());
    gps.encode(static_cast<char>(byte));
#if defined(COASTCUE_GPS_DIAGNOSTICS)
    gpsStreamDiagnostics.feed(byte, millis());
#endif
  }
}

void printGpsDiagnostic() {
#if defined(COASTCUE_GPS_DIAGNOSTICS)
  serviceGpsDiagnosticPhases(millis());
#elif COASTCUE_VERBOSE_SERIAL
  const uint32_t now = millis();
  if (now - lastGpsDiagnosticAt < GPS_DIAGNOSTIC_INTERVAL_MS) return;
  lastGpsDiagnosticAt = now;

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
#endif  // COASTCUE_GPS_DIAGNOSTICS / COASTCUE_VERBOSE_SERIAL
}
#endif

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
#if !COASTCUE_PMU_ENABLED
  pmuReady = true;
#elif COASTCUE_TBEAM_PMU == COASTCUE_TBEAM_PMU_AXP2101
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
#elif COASTCUE_TBEAM_PMU == COASTCUE_TBEAM_PMU_AXP192
  pmuReady = coastcuePower.begin(Wire, AXP192_SLAVE_ADDRESS,
                                COASTCUE_PIN_I2C_SDA, COASTCUE_PIN_I2C_SCL);
  if (!pmuReady) {
    Serial.println("[PMU] AXP192 not found; LoRa/GPS power rails unconfirmed");
    return;
  }
  coastcuePower.setLDO2Voltage(3300);
  coastcuePower.enableLDO2();
  coastcuePower.setLDO3Voltage(3300);
  coastcuePower.enableLDO3();
  coastcuePower.enableBattDetection();
  coastcuePower.enableVbusVoltageMeasure();
  coastcuePower.enableBattVoltageMeasure();
  coastcuePower.enableSystemVoltageMeasure();
  Serial.println("[PMU] AXP192 LDO2 radio and LDO3 GPS rails enabled");
#endif
#if COASTCUE_PMU_ENABLED
  vTaskDelay(pdMS_TO_TICKS(100));
#endif
}

void printBootBanner() {
  Serial.printf("[CoastCue] boot role=%s board=%s node_uid=%s boat_id=%s\n",
                IS_SHORE ? "shore" : "boat", COASTCUE_BOARD_LABEL,
                nodeUid.c_str(), BOAT_ID);
  Serial.printf("[CoastCue] transport=LoRa region=%s freq=%.1fMHz bw=%.1fkHz sf=%d cr=%d power=%ddBm\n",
                REGION_LABEL, static_cast<double>(COASTCUE_LORA_FREQUENCY_MHZ),
                static_cast<double>(COASTCUE_LORA_BANDWIDTH_KHZ),
                COASTCUE_LORA_SPREADING_FACTOR, COASTCUE_LORA_CODING_RATE,
                COASTCUE_LORA_TX_POWER_DBM);
  Serial.printf("[CoastCue] pmu=%s ready=%s gps_freshness_ms=%lu auth=hmac-sha256-128\n",
                COASTCUE_PMU_LABEL, pmuReady ? "yes" : "no",
                static_cast<unsigned long>(COASTCUE_GPS_FRESHNESS_MS));
#if defined(COASTCUE_GPS_DIAGNOSTICS)
  Serial.printf("[GPS-DIAG] enabled report_period_ms=%lu\n",
                static_cast<unsigned long>(ACTIVE_REPORT_PERIOD_MS));
#endif
}

void drawScreen() {
#if !defined(COASTCUE_ROLE_SHORE) && COASTCUE_OLED_ENABLED
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
  display.print(isFreshGpsFix() ? "GPS fresh" : "GPS waiting");
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
    if (!boatHardwareReady) setRed(true);
#if COASTCUE_BATTERY_ADC_ENABLED
    analogReadResolution(12);
    analogSetPinAttenuation(COASTCUE_PIN_BATTERY_ADC, ADC_11db);
#endif
    if (boatHardwareReady) {
#if defined(COASTCUE_GPS_DIAGNOSTICS)
      beginGpsDiagnostics();
#else
      GPS.begin(9600, SERIAL_8N1, COASTCUE_PIN_GPS_RX, COASTCUE_PIN_GPS_TX);
#endif
#if COASTCUE_OLED_ENABLED
      if (!display.begin(SSD1306_SWITCHCAPVCC, COASTCUE_OLED_ADDR)) {
        Serial.println("[WARN] OLED init failed; safety radio remains enabled");
      } else {
        displayReady = true;
      }
#endif
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
#if !defined(COASTCUE_GPS_DIAGNOSTICS)
  normalReportScheduler.scheduleFirstPeriodicReport(
      millis(), ACTIVE_REPORT_PERIOD_MS);
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
  printGpsDiagnostic();
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
#if defined(COASTCUE_GPS_DIAGNOSTICS)
  const bool normalReportDue = gpsDiagnosticTransmitDue(millis());
#else
  const bool normalReportDue = normalReportScheduler.normalReportDue(
      millis(), ACTIVE_REPORT_PERIOD_MS, isFreshGpsFix());
#endif
  if (loraReady && !loraTransmitInFlight && !boatReceiveWindowActive &&
      !boatStateReceiptPending && !emergencyScheduler.hasPending() &&
      normalReportDue) {
    if (sendNormalReport()) {
#if defined(COASTCUE_GPS_DIAGNOSTICS)
      gpsDiagnosticTransmitStarted(millis());
#endif
    }
  }
#endif
  serviceBuzz();
#if !defined(COASTCUE_ROLE_SHORE)
  drawScreen();
#endif
}
