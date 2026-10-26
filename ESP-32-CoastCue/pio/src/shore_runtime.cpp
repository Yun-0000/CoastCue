#if defined(COASTCUE_ROLE_SHORE)

#include "shore_runtime.h"

#ifndef COASTCUE_BENCH_SHORE_FAIL_FIRST_OPERATOR_TRANSITION
#define COASTCUE_BENCH_SHORE_FAIL_FIRST_OPERATOR_TRANSITION 0
#endif

#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <FS.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>

#include "coastcue_pins.h"
#include "incident_protocol.h"

namespace {
constexpr const char *JOURNAL_PATH = "/coastcue-events.ndjson";
constexpr const char *JOURNAL_TEMP_PATH = "/coastcue-events.tmp";
constexpr const char *JOURNAL_BACKUP_PATH = "/coastcue-events.bak";
constexpr const char *JOURNAL_EPOCH_PATH = "/coastcue-journal-epoch";
constexpr const char *JOURNAL_EPOCH_NVS_KEY = "journal_epoch";
constexpr const char *JOURNAL_EPOCH_PROVISIONAL_NVS_KEY = "journal_ep_tmp";
constexpr const char *VOLATILE_ID_NVS_KEY = "volatile_id";
constexpr const char *PENDING_EMERGENCY_NVS_KEY = "pend_emergency";
constexpr const char *PENDING_EMERGENCY_SLOT_PREFIX = "pend_emerg";
constexpr uint8_t EMERGENCY_CHECKPOINT_SLOTS = 8;
constexpr const char *CONFIRMED_INCIDENT_NVS_KEY = "radio_incident";
constexpr const char *CONFIRMED_STATE_SEQ_NVS_KEY = "radio_state_seq";
constexpr size_t MAX_EMERGENCY_CHECKPOINT_BYTES = 1400;
// At most 560 KiB of the 1,408 KiB data partition is copied at once. Keeping
// the replacement below 360 KiB means old + new + filesystem metadata fit.
constexpr uint32_t JOURNAL_HIGH_WATER_BYTES = 560 * 1024;
constexpr uint32_t JOURNAL_LOW_WATER_BYTES = 360 * 1024;
const coastcue::JournalWindow JOURNAL_WINDOW(JOURNAL_HIGH_WATER_BYTES,
                                            JOURNAL_LOW_WATER_BYTES);
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
constexpr uint32_t DOUBLE_CLICK_MS = 450;
constexpr uint32_t PAIRING_MS = 120000;
constexpr uint32_t BLE_CONNECTED_NOTICE_MS = 5000;
constexpr uint32_t BLE_DISCONNECTED_NOTICE_MS = 5000;
constexpr uint32_t STATUS_INTERVAL_MS = 1000;
constexpr uint32_t OLED_I2C_HZ = 100000;
constexpr uint32_t STORAGE_RECOVERY_RETRY_MS = 30000;
constexpr uint32_t BLE_AUTH_TIMEOUT_MS = 30000;
constexpr uint8_t EVENT_MESSAGE_TYPE = 1;
constexpr size_t EVENT_CHUNK_BYTES = 160;
static_assert(EVENT_CHUNK_BYTES == coastcue::kBleEventChunkMaxBytes,
              "host/protocol event chunk max must match firmware");
constexpr size_t MAX_PENDING_COMMANDS = 8;
constexpr size_t MAX_OPERATOR_COMMANDS = 4;
constexpr size_t MAX_GENERAL_COMMANDS = 6;
constexpr size_t MAX_OUTGOING_MESSAGES = 48;
constexpr uint32_t NOTIFY_GAP_MS = coastcue::kBleNotifyGapMs;
constexpr const char *FIRMWARE_VERSION = "1.0-mvp";

const char *const SERVICE_UUID = "7A1F0001-6B7C-4D8E-9F10-112233445566";
const char *const STATUS_UUID = "7A1F0002-6B7C-4D8E-9F10-112233445566";
const char *const EVENTS_UUID = "7A1F0003-6B7C-4D8E-9F10-112233445566";
const char *const COMMAND_UUID = "7A1F0004-6B7C-4D8E-9F10-112233445566";
const char *const DEVICE_INFO_UUID = "7A1F0005-6B7C-4D8E-9F10-112233445566";

ShoreRuntime *runtime = nullptr;
NimBLECharacteristic *statusCharacteristic = nullptr;
NimBLECharacteristic *eventsCharacteristic = nullptr;
NimBLECharacteristic *deviceInfoCharacteristic = nullptr;
Adafruit_SH1106G display(COASTCUE_OLED_WIDTH, COASTCUE_OLED_HEIGHT, &Wire,
                         COASTCUE_OLED_RESET, OLED_I2C_HZ, OLED_I2C_HZ);
bool displayReady = false;
#if COASTCUE_BENCH_SHORE_FAIL_FIRST_OPERATOR_TRANSITION
bool benchStorageRecoverySuppressed = false;
#endif

uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (-(static_cast<int32_t>(crc & 1))));
    }
  }
  return ~crc;
}

String jsonString(const JsonDocument &document) {
  String output;
  serializeJson(document, output);
  return output;
}

String base64(const String &input) {
  size_t encodedLength = 0;
  mbedtls_base64_encode(nullptr, 0, &encodedLength,
                        reinterpret_cast<const uint8_t *>(input.c_str()),
                        input.length());
  String encoded;
  encoded.reserve(encodedLength);
  uint8_t *buffer = reinterpret_cast<uint8_t *>(malloc(encodedLength + 1));
  if (buffer == nullptr) return "";
  const int result = mbedtls_base64_encode(
      buffer, encodedLength + 1, &encodedLength,
      reinterpret_cast<const uint8_t *>(input.c_str()), input.length());
  if (result == 0) {
    buffer[encodedLength] = '\0';
    encoded = String(reinterpret_cast<char *>(buffer));
  }
  free(buffer);
  return encoded;
}

String newJournalEpoch() {
  char value[25];
  snprintf(value, sizeof(value), "%08lx%08lx%08lx",
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  return String(value);
}

void trimTrailingCr(String &line) {
  if (line.endsWith("\r")) line.remove(line.length() - 1);
}

bool readJournalLine(File &file, String &line, size_t &bytesRead,
                     bool &newlineTerminated) {
  line = "";
  bytesRead = 0;
  newlineTerminated = false;
  while (file.available()) {
    const int value = file.read();
    if (value < 0) break;
    ++bytesRead;
    if (value == '\n') {
      newlineTerminated = true;
      break;
    }
    line += static_cast<char>(value);
  }
  return bytesRead != 0;
}

bool writeJournalLine(File &file, const String &line, size_t &bytesWritten) {
  const size_t payload = file.print(line);
  const size_t newline = file.write(static_cast<uint8_t>('\n'));
  bytesWritten = payload + newline;
  return payload == line.length() && newline == 1;
}

String extractNonce(const String &packet) {
  JsonDocument document;
  if (deserializeJson(document, packet) != DeserializationError::Ok ||
      !document["nonce"].is<const char *>()) {
    return "";
  }
  const String nonce = document["nonce"].as<const char *>();
  if (nonce.length() != 16) return "";
  for (size_t i = 0; i < nonce.length(); ++i) {
    const char value = nonce.charAt(i);
    if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return "";
  }
  return nonce;
}

String emergencyCheckpointKey(uint8_t slot) {
  return String(PENDING_EMERGENCY_SLOT_PREFIX) + String(slot);
}

uint32_t bleCommandRecordId(const JsonVariantConst &value) {
  if (value.is<uint64_t>()) {
    return coastcue::normalizeBleCommandRecordId(value.as<uint64_t>(), true);
  }
  if (value.is<int64_t>()) {
    const int64_t raw = value.as<int64_t>();
    return coastcue::normalizeBleCommandRecordId(
        raw > 0 ? static_cast<uint64_t>(raw) : 0, raw > 0);
  }
  return 0;
}

bool promoteAuthenticatedLinkIfSecure(NimBLEConnInfo &connectionInfo,
                                      const char *source) {
  if (runtime == nullptr) return false;
  const bool secureBond = coastcue::shouldPromoteBleAuthenticatedLink(
      connectionInfo.isEncrypted(), connectionInfo.isBonded(),
      NimBLEDevice::isBonded(connectionInfo.getAddress()),
      NimBLEDevice::isBonded(connectionInfo.getIdAddress()),
      static_cast<size_t>(NimBLEDevice::getNumBonds()));
  if (!secureBond) return runtime->bleAuthenticated();
  if (!runtime->bleAuthenticated()) {
    runtime->onBleAuthenticated();
    Serial.printf("[SHORE] BLE authenticated source=%s handle=%u\n", source,
                  static_cast<unsigned>(connectionInfo.getConnHandle()));
  }
  return true;
}

// A 64-bit content hash feeds the bounded in-memory replay filters. The normal
// negative path stays out of LittleFS; positive collisions are exact-confirmed
// against retained history and never admit a duplicate.
uint64_t replayIndexHash(const String &value) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t index = 0; index < value.length(); ++index) {
    hash ^= static_cast<uint8_t>(value.charAt(index));
    hash *= 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic,
               NimBLEConnInfo &connectionInfo) override {
    if (runtime == nullptr) return;
    // WRITE_ENC proves this callback ran over an encrypted link. On bonded
    // reconnects NimBLE may not emit another authentication-complete callback,
    // so recover the runtime-ready flag from the still-valid sole bond before
    // admitting Sync/Ack/Resolve.
    if (!promoteAuthenticatedLinkIfSecure(connectionInfo, "encrypted_write")) {
      Serial.printf(
          "[SHORE] encrypted command rejected before bond authentication handle=%u\n",
          static_cast<unsigned>(connectionInfo.getConnHandle()));
      return;
    }
    const NimBLEAttValue value = characteristic->getValue();
    Serial.printf("[SHORE] BLE command received handle=%u bytes=%u\n",
                  static_cast<unsigned>(connectionInfo.getConnHandle()),
                  static_cast<unsigned>(value.length()));
    runtime->handleCommand(String(
        reinterpret_cast<const char *>(value.data()), value.length()));
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connectionInfo) override {
    // iPhones commonly reconnect with a resolvable private address, which may
    // not map to the stored identity until after onConnect. With one bond we
    // provisionally allow the link, then enforce that identity/authentication
    // resolves to the sole encrypted bond below.
    const bool hasBond = NimBLEDevice::getNumBonds() > 0;
    const bool firstPairing = runtime != nullptr && runtime->pairingOpen() &&
                              NimBLEDevice::getNumBonds() == 0;
    if (runtime == nullptr || (!hasBond && !firstPairing)) {
      Serial.printf("[SHORE] local disconnect cause=no_bond handle=%u bonds=%d pairing=%s\n",
                    static_cast<unsigned>(connectionInfo.getConnHandle()),
                    NimBLEDevice::getNumBonds(), firstPairing ? "yes" : "no");
      server->disconnect(connectionInfo.getConnHandle());
      return;
    }
    runtime->onBleLinkConnected(connectionInfo.getConnHandle());
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connectionInfo,
                    int reason) override {
    (void)server;
    (void)connectionInfo;
    if (runtime != nullptr) runtime->onBleDisconnected(reason);
  }
  uint32_t onPassKeyDisplay() override {
    return runtime == nullptr ? 0 : runtime->pairingPasskey();
  }
  void onIdentity(NimBLEConnInfo &connectionInfo) override {
    if (NimBLEDevice::getNumBonds() == 0) return;
    if (promoteAuthenticatedLinkIfSecure(connectionInfo, "identity")) return;
    const bool knownBond = NimBLEDevice::isBonded(connectionInfo.getAddress()) ||
                           NimBLEDevice::isBonded(connectionInfo.getIdAddress());
    if (!knownBond && (runtime == nullptr || !runtime->bleAuthenticated())) {
      Serial.printf("[SHORE] local disconnect cause=unknown_identity handle=%u bonds=%d\n",
                    static_cast<unsigned>(connectionInfo.getConnHandle()),
                    NimBLEDevice::getNumBonds());
      NimBLEDevice::getServer()->disconnect(connectionInfo.getConnHandle());
    }
  }
  void onAuthenticationComplete(NimBLEConnInfo &connectionInfo) override {
    const bool secureBond = coastcue::shouldPromoteBleAuthenticatedLink(
        connectionInfo.isEncrypted(), connectionInfo.isBonded(),
        NimBLEDevice::isBonded(connectionInfo.getAddress()),
        NimBLEDevice::isBonded(connectionInfo.getIdAddress()),
        static_cast<size_t>(NimBLEDevice::getNumBonds()));
    if (!secureBond || NimBLEDevice::getNumBonds() != 1) {
      Serial.printf(
          "[SHORE] local disconnect cause=authentication encrypted=%s bonded=%s bonds=%d\n",
          connectionInfo.isEncrypted() ? "yes" : "no",
          connectionInfo.isBonded() ? "yes" : "no",
          NimBLEDevice::getNumBonds());
      if (connectionInfo.isBonded() && NimBLEDevice::getNumBonds() > 1) {
        NimBLEDevice::deleteBond(connectionInfo.getIdAddress());
      }
      NimBLEDevice::getServer()->disconnect(connectionInfo.getConnHandle());
      return;
    }
    promoteAuthenticatedLinkIfSecure(connectionInfo, "authentication_complete");
  }
};
}  // namespace

void ShoreRuntime::begin(const String &nodeUid,
                         const String &configuredBoatId) {
  nodeUid_ = nodeUid;
  configuredBoatId_ = configuredBoatId;
  runtime = this;
  queueMutex_ = xSemaphoreCreateMutex();
  commandQueue_ = xQueueCreate(MAX_GENERAL_COMMANDS, sizeof(CommandRequest));
  operatorCommandQueue_ =
      xQueueCreate(MAX_OPERATOR_COMMANDS, sizeof(CommandRequest));
  busyCommandQueue_ = xQueueCreate(MAX_PENDING_COMMANDS, sizeof(CommandRequest));
  if (commandQueue_ == nullptr || operatorCommandQueue_ == nullptr ||
      busyCommandQueue_ == nullptr) {
    Serial.println("[SHORE] command queue allocation failed; commands unavailable");
  }
  pinMode(COASTCUE_PIN_BUTTON, INPUT_PULLUP);
  pinMode(COASTCUE_PIN_BUZZER, OUTPUT);
  digitalWrite(COASTCUE_PIN_BUZZER, LOW);
  Wire.begin(COASTCUE_PIN_I2C_SDA, COASTCUE_PIN_I2C_SCL);
  displayReady = display.begin(COASTCUE_OLED_ADDR, true);

  Preferences preferences;
  preferences.begin("coastcue", false);
  nextRecordId_ = preferences.getUInt("next_id", 1);
  // ESP32 millis() restarts at zero after power loss, so a wall-clock base from
  // the previous boot would fabricate a timestamp. Time stays unknown until a
  // connected phone sets it during this boot.
  unixBaseMs_ = 0;
  unixBaseUptimeMs_ = 0;
  confirmedIncidentId_ = preferences.getString(CONFIRMED_INCIDENT_NVS_KEY, "");
  confirmedIncidentStateSeq_ = preferences.getUInt(CONFIRMED_STATE_SEQ_NVS_KEY, 0);
  journalEpoch_ = preferences.getString(JOURNAL_EPOCH_NVS_KEY, "");
  journalEpochProvisional_ =
      preferences.getBool(JOURNAL_EPOCH_PROVISIONAL_NVS_KEY, false);
  const bool fileSystemInitialized = preferences.getBool("fs_init", false);
  preferences.end();

  // Normal runtime never formats storage. A failed mount may still contain
  // recoverable history, so destructive initialization is an explicit,
  // guarded deployment step (`make init-shore-storage`).
  const bool mounted = LittleFS.begin(false);
  storageReady_ = mounted && initializeMountedStorage();
  if (!mounted) {
    Serial.println("[SHORE] LittleFS unavailable; journal disabled");
  } else if (!storageReady_) {
    Serial.println("[SHORE] mounted storage rejected; journal disabled");
  } else {
    if (!fileSystemInitialized) {
      preferences.begin("coastcue", false);
      preferences.putBool("fs_init", true);
      preferences.end();
    }
  }
  if (!coastcue::isValidJournalEpoch(journalEpoch_.c_str()) &&
      !ensureFallbackJournalIdentity()) {
    Serial.println("[SHORE] stable journal identity checkpoint failed");
  }
  initializeVolatileRecordIds();
  if (nextRecordId_ >= 0x80000000UL) {
    storageReady_ = false;
    Serial.println("[SHORE] persistent record ID space exhausted; persistence disabled");
  }
  recoverEmergencyCheckpoint();

  NimBLEDevice::init("CoastCue Shore");
  NimBLEDevice::setMTU(185);
  NimBLEDevice::setSecurityAuth(true, true, true);
  passkey_ = (esp_random() % 900000UL) + 100000UL;
  NimBLEDevice::setSecurityPasskey(passkey_);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  bleUnexpectedlyOffline_ = NimBLEDevice::getNumBonds() > 0;
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService *service = server->createService(SERVICE_UUID);
  statusCharacteristic = service->createCharacteristic(
      STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY |
                       NIMBLE_PROPERTY::READ_ENC);
  eventsCharacteristic = service->createCharacteristic(
      EVENTS_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  NimBLECharacteristic *command = service->createCharacteristic(
      COMMAND_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  command->setCallbacks(new CommandCallbacks());
  deviceInfoCharacteristic = service->createCharacteristic(
      DEVICE_INFO_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
  refreshDeviceInfo();
  service->start();
  NimBLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
  notifyStatus();
  restartAdvertisingIfAllowed();
  Serial.printf("[SHORE] local runtime ready display=%s journal=%s BLE pairing physical-gated\n",
                displayReady ? "yes" : "no", storageReady_ ? "yes" : "no");
}

bool ShoreRuntime::acceptNonce(const String &wirePacket) {
  const String nonce = extractNonce(wirePacket);
  if (nonce.length() != 16) return false;
  for (const String &recent : recentNonces_) if (recent == nonce) return false;
  const uint64_t hash = replayIndexHash(nonce);
  const auto decision = coastcue::decideReplayAdmission(
      journalNonceIndex_.complete(), journalNonceIndex_.contains(hash));
  if (decision == coastcue::ReplayAdmissionDecision::kReject ||
      (decision == coastcue::ReplayAdmissionDecision::kConfirmExact &&
       journalContainsNonce(nonce))) {
    return false;
  }
  recentNonces_[nextNonce_++ % 64] = nonce;
  return true;
}

bool ShoreRuntime::ingestAuthenticatedPacket(const String &wirePacket, float rssi,
                                             float snr) {
  JsonDocument packet;
  if (deserializeJson(packet, wirePacket) != DeserializationError::Ok) {
    return false;
  }

  const String kind = packet["kind"] | "";
  const String packetNodeUid = packet["node_uid"] | "";
  const String boatId = configuredBoatId_;
  lastRssi_ = rssi;
  lastSnr_ = snr;
  lastPacketAt_ = millis();
  hasRadioMetrics_ = true;
  if (!packetNodeUid.isEmpty()) lastNodeUid_ = packetNodeUid;

  if (kind == "state_receipt") {
    confirmIncidentDownlink(packet["incident_id"] | "",
                            packet["state_seq"] | 0U);
    notifyStatus();
    return false;
  }

  const String status = packet["status"] | "normal";
  const String incidentId =
      status == "help" || status == "sos" ? String(packet["i"] | "")
                                           : String();
  if (!incidentId.isEmpty()) {
    if (incidentId == activeIncidentId_) {
      // A retransmission refreshes link metrics but does not create another
      // journal record or iPhone alert. Storage recovery promotes the exact
      // original checkpoint rather than turning this new radio nonce into a
      // second semantic event.
      if (!activeIncidentDurable_) {
        tryRecoverStorage();
      }
      notifyStatus();
      return true;
    }
    if (journalContainsIncident(incidentId)) {
      notifyStatus();
      return false;
    }
  }

  appendRadioEvent(wirePacket, extractNonce(wirePacket), status, incidentId,
                   rssi, snr);
  return true;
}

bool ShoreRuntime::takePendingRadioDownlink(
    ShoreRadioDownlink &downlink) const {
  if (pendingRadioDownlinks_.empty()) return false;
  downlink = pendingRadioDownlinks_.front();
  return true;
}

void ShoreRuntime::bindActiveIncidentIdentity(const String &incidentId,
                                              const String &boatNodeUid,
                                              uint32_t stateSeq,
                                              bool receiptAvailable) {
  activeIncidentId_ = incidentId;
  activeBoatNodeUid_ = boatNodeUid;
  activeIncidentStateSeq_ = stateSeq;
  activeIncidentDurable_ = receiptAvailable;
  confirmedIncidentId_ = incidentId;
  confirmedIncidentStateSeq_ = 0;
  pendingRadioDownlinks_.clear();
  persistConfirmedIncidentState();
  if (receiptAvailable) {
    queueIncidentDownlink("receipt", stateSeq);
  }
}

void ShoreRuntime::clearActiveIncidentIdentity() {
  activeIncidentId_ = "";
  activeIncidentNonce_ = "";
  activeBoatNodeUid_ = "";
  activeIncidentStateSeq_ = 0;
  activeIncidentDurable_ = false;
  activeLiveEmergency_ = "";
  pendingRadioDownlinks_.clear();
}

void ShoreRuntime::queueIncidentDownlink(const String &state,
                                         uint32_t stateSeq) {
  if (activeIncidentId_.isEmpty() || activeBoatNodeUid_.isEmpty() ||
      stateSeq == 0) {
    return;
  }
  if (confirmedIncidentId_ == activeIncidentId_ &&
      confirmedIncidentStateSeq_ >= stateSeq) {
    return;
  }
  for (auto pending = pendingRadioDownlinks_.begin();
       pending != pendingRadioDownlinks_.end();) {
    if (pending->incidentId == activeIncidentId_ &&
        pending->stateSeq == stateSeq) {
      return;
    }
    // A newer operator state carries all earlier progress. Keep only the
    // highest unsatisfied revision so direct Resolve cannot sit behind an
    // unconfirmed Receipt or Acknowledged frame for another heartbeat cycle.
    if (pending->incidentId == activeIncidentId_ &&
        pending->stateSeq < stateSeq) {
      pending = pendingRadioDownlinks_.erase(pending);
    } else {
      ++pending;
    }
  }
  pendingRadioDownlinks_.push_back(
      {activeBoatNodeUid_, activeIncidentId_, state, stateSeq});
}

void ShoreRuntime::confirmIncidentDownlink(const String &incidentId,
                                           uint32_t stateSeq) {
  if (incidentId != activeIncidentId_ || stateSeq == 0 ||
      stateSeq > activeIncidentStateSeq_) {
    return;
  }
  bool matchesPendingRevision = false;
  for (const auto &pending : pendingRadioDownlinks_) {
    if (coastcue::isMatchingPendingStateReceipt(
            incidentId.c_str(), stateSeq, pending.incidentId.c_str(),
            pending.stateSeq)) {
      matchesPendingRevision = true;
      break;
    }
  }
  if (!matchesPendingRevision) return;
  confirmedIncidentId_ = incidentId;
  confirmedIncidentStateSeq_ =
      max(confirmedIncidentStateSeq_, stateSeq);
  while (!pendingRadioDownlinks_.empty() &&
         pendingRadioDownlinks_.front().incidentId == incidentId &&
         pendingRadioDownlinks_.front().stateSeq <= stateSeq) {
    pendingRadioDownlinks_.pop_front();
  }
  persistConfirmedIncidentState();
  Serial.printf("[LORA] state receipt confirmed incident=%s seq=%lu\n",
                incidentId.c_str(), static_cast<unsigned long>(stateSeq));
}

void ShoreRuntime::persistConfirmedIncidentState() {
  Preferences preferences;
  if (!preferences.begin("coastcue", false)) return;
  preferences.putString(CONFIRMED_INCIDENT_NVS_KEY, confirmedIncidentId_);
  preferences.putUInt(CONFIRMED_STATE_SEQ_NVS_KEY,
                      confirmedIncidentStateSeq_);
  preferences.end();
}

bool ShoreRuntime::appendJournal(const String &encoded) {
  if (!storageReady_) return false;
  File journal = LittleFS.open(JOURNAL_PATH, FILE_APPEND);
  if (!journal) {
    storageReady_ = false;
    Serial.println("[SHORE] journal open failed; persistence unavailable until recovery retry");
    return false;
  }
  size_t written = 0;
  const bool complete = writeJournalLine(journal, encoded, written);
  journal.flush();
  const bool writeError = journal.getWriteError() != 0;
  journal.close();
  if (!complete || writeError) {
    // A short write may leave a partial final line. This boot treats the
    // journal as unavailable and never assigns another persistent record ID.
    storageReady_ = false;
    Serial.println("[SHORE] journal append failed; persistence unavailable until recovery retry");
    return false;
  }
  ++journalCount_;
  journalBytes_ += written;
  return true;
}

bool ShoreRuntime::persistJournalEpochMirror(const String &epoch) {
  if (!coastcue::isValidJournalEpoch(epoch.c_str())) return false;
  Preferences preferences;
  if (!preferences.begin("coastcue", false)) return false;
  const size_t written =
      preferences.putString(JOURNAL_EPOCH_NVS_KEY, epoch);
  preferences.end();
  return written != 0;
}

bool ShoreRuntime::ensureFallbackJournalIdentity() {
  if (coastcue::isValidJournalEpoch(journalEpoch_.c_str())) return true;
  const String generated = newJournalEpoch();
  Preferences preferences;
  if (!preferences.begin("coastcue", false)) return false;
  const bool storedEpoch =
      preferences.putString(JOURNAL_EPOCH_NVS_KEY, generated) != 0;
  const bool storedProvisional =
      preferences.putBool(JOURNAL_EPOCH_PROVISIONAL_NVS_KEY, true) ==
      sizeof(bool);
  preferences.end();
  if (!storedEpoch || !storedProvisional) return false;
  journalEpoch_ = generated;
  journalEpochProvisional_ = true;
  return true;
}

void ShoreRuntime::clearProvisionalJournalIdentity() {
  if (!journalEpochProvisional_) return;
  Preferences preferences;
  if (!preferences.begin("coastcue", false)) return;
  if (!preferences.remove(JOURNAL_EPOCH_PROVISIONAL_NVS_KEY)) {
    preferences.end();
    Serial.println("[SHORE] provisional journal identity flag clear failed");
    return;
  }
  preferences.end();
  journalEpochProvisional_ = false;
}

void ShoreRuntime::refreshDeviceInfo(bool disconnectChangedIdentity) {
  if (deviceInfoCharacteristic == nullptr) return;
  deviceInfoCharacteristic->setValue(
      (String("{\"name\":\"CoastCue Shore\",\"node_uid\":\"") + nodeUid_ +
       "\",\"journal_epoch\":\"" + journalEpoch_ + "\",\"firmware\":\"" +
       FIRMWARE_VERSION + "\"}")
          .c_str());
  // A provisional identity must never continue an authenticated session after
  // the durable filesystem identity is adopted. Force Device Info/trust to be
  // evaluated again before any more commands or sync records are accepted.
  if (disconnectChangedIdentity && connectedHandle_ != 0xFFFF) {
    bleConnected_ = false;
    Serial.println("[SHORE] local disconnect cause=journal_identity_changed");
    NimBLEDevice::getServer()->disconnect(connectedHandle_);
  }
}

bool ShoreRuntime::prepareJournalIdentity() {
  String filesystemEpoch;
  File epoch = LittleFS.open(JOURNAL_EPOCH_PATH, FILE_READ);
  if (epoch) {
    filesystemEpoch = epoch.readStringUntil('\n');
    epoch.close();
    trimTrailingCr(filesystemEpoch);
  }
  File journal = LittleFS.open(JOURNAL_PATH, FILE_READ);
  const bool journalHasHistory = journal && journal.size() > 0;
  if (journal) journal.close();

  const coastcue::JournalIdentityDecision decision =
      coastcue::decideJournalIdentity(journalEpoch_.c_str(),
                                     filesystemEpoch.c_str(),
                                     journalHasHistory,
                                     journalEpochProvisional_);
  if (decision == coastcue::JournalIdentityDecision::kReject) {
    Serial.println(
        "[SHORE] journal identity mismatch or missing marker; history rejected");
    return false;
  }
  if (decision == coastcue::JournalIdentityDecision::kUseMatching) {
    journalEpoch_ = filesystemEpoch;
    clearProvisionalJournalIdentity();
    return true;
  }
  if (decision == coastcue::JournalIdentityDecision::kAdoptFilesystem) {
    const bool identityChanged = journalEpoch_ != filesystemEpoch;
    journalEpoch_ = filesystemEpoch;
    if (!persistJournalEpochMirror(filesystemEpoch)) {
      Serial.println("[SHORE] journal epoch NVS mirror write failed");
      return false;
    }
    clearProvisionalJournalIdentity();
    refreshDeviceInfo(identityChanged);
    return true;
  }
  if (decision == coastcue::JournalIdentityDecision::kCreateNew) {
    journalEpoch_ = newJournalEpoch();
    if (!persistJournalEpochMirror(journalEpoch_)) {
      Serial.println("[SHORE] journal epoch NVS mirror creation failed");
      journalEpoch_ = "";
      return false;
    }
  }

  // Seeding/replacing the marker is permitted only on an empty journal. The
  // pure decision above rejects missing or mismatched identity for history.
  LittleFS.remove(JOURNAL_EPOCH_PATH);
  epoch = LittleFS.open(JOURNAL_EPOCH_PATH, FILE_WRITE);
  size_t epochBytes = 0;
  if (!epoch || !writeJournalLine(epoch, journalEpoch_, epochBytes)) {
    if (epoch) epoch.close();
    Serial.println("[SHORE] journal epoch marker write failed");
    return false;
  }
  epoch.flush();
  const bool writeError = epoch.getWriteError() != 0;
  epoch.close();
  if (writeError) {
    Serial.println("[SHORE] journal epoch marker flush failed");
    return false;
  }
  clearProvisionalJournalIdentity();
  return true;
}

void ShoreRuntime::resetJournalDerivedState() {
  journalNonceIndex_.reset();
  journalIncidentIndex_.reset();
  journalCount_ = 0;
  journalBytes_ = 0;
  firstStoredRecordId_ = 0;
  truncatedBeforeRecordId_ = 0;
  syncHintAfter_ = 0;
  syncHintOffset_ = 0;
  alerts_ = coastcue::AlertReducer();
  activeRecordId_ = 0;
  activeSeverity_ = 0;
  alertState_ = "resolved";
  silenced_ = false;
  activeIncidentId_ = "";
  activeIncidentNonce_ = "";
  activeBoatNodeUid_ = "";
  activeIncidentStateSeq_ = 0;
  activeIncidentDurable_ = false;
  activeLiveEmergency_ = "";
  pendingRadioDownlinks_.clear();
  hasLastLocation_ = false;
  lastLatitude_ = 0.0f;
  lastLongitude_ = 0.0f;
  hasLastGpsAge_ = false;
  lastGpsAgeSeconds_ = 0;
  liveRecoveredCheckpointCount_ = 0;
  for (String &nonce : liveRecoveredCheckpointNonces_) nonce = "";
}

bool ShoreRuntime::initializeMountedStorage() {
  // Normalize only interrupted journal swaps. Never format or replace journal
  // history as part of automatic recovery.
  if (!LittleFS.exists(JOURNAL_PATH) && LittleFS.exists(JOURNAL_BACKUP_PATH)) {
    if (!LittleFS.rename(JOURNAL_BACKUP_PATH, JOURNAL_PATH)) return false;
  }
  if (LittleFS.exists(JOURNAL_PATH) && LittleFS.exists(JOURNAL_BACKUP_PATH)) {
    LittleFS.remove(JOURNAL_BACKUP_PATH);
  }
  if (LittleFS.exists(JOURNAL_TEMP_PATH)) LittleFS.remove(JOURNAL_TEMP_PATH);
  if (!recoverJournalTail()) {
    Serial.println("[SHORE] journal recovery failed");
    return false;
  }
  if (!prepareJournalIdentity()) return false;

  resetJournalDerivedState();
  storageReady_ = true;
  replayJournal();
  // A retained journal can exceed the fixed replay window. Compaction runs
  // only after complete replay rebuilt the reducer and active lineage.
  compactJournalIfNeeded();
  return storageReady_;
}

uint32_t ShoreRuntime::allocateVolatileRecordId() {
  const uint32_t id = nextVolatileRecordId_;
  ++nextVolatileRecordId_;
  if (nextVolatileRecordId_ < 0x80000000UL) nextVolatileRecordId_ = 0x80000000UL;
  Preferences preferences;
  if (preferences.begin("coastcue", false)) {
    if (preferences.putUInt(VOLATILE_ID_NVS_KEY, nextVolatileRecordId_) != sizeof(uint32_t)) {
      Serial.println("[SHORE] volatile ID checkpoint failed; next boot will randomize range");
    }
    preferences.end();
  }
  return id;
}

void ShoreRuntime::initializeVolatileRecordIds() {
  Preferences preferences;
  uint32_t saved = 0;
  if (preferences.begin("coastcue", false)) {
    saved = preferences.getUInt(VOLATILE_ID_NVS_KEY, 0);
    preferences.end();
  }
  // Even with a valid checkpoint, randomly jump within the high-bit range at
  // each boot. If a prior NVS write silently failed, the old checkpoint cannot
  // deterministically recreate IDs emitted by that earlier boot.
  const uint32_t randomJump = (esp_random() & 0x000FFFFFUL) + 1;
  const uint32_t base = saved >= 0x80000000UL
                            ? (saved & 0x7FFFFFFFUL)
                            : (esp_random() & 0x7FFFFFFFUL);
  nextVolatileRecordId_ = 0x80000000UL | ((base + randomJump) & 0x7FFFFFFFUL);
  if (nextVolatileRecordId_ == 0xFFFFFFFFUL) nextVolatileRecordId_ = 0x80000000UL;
  Preferences checkpoint;
  if (checkpoint.begin("coastcue", false)) {
    if (checkpoint.putUInt(VOLATILE_ID_NVS_KEY, nextVolatileRecordId_) != sizeof(uint32_t)) {
      Serial.println("[SHORE] volatile ID seed checkpoint failed; random fallback remains active");
    }
    checkpoint.end();
  }
}

bool ShoreRuntime::recoverJournalTail() {
  File source = LittleFS.open(JOURNAL_PATH, FILE_READ);
  if (!source) return true;

  coastcue::JournalRecoveryPolicy recovery;
  while (source.available()) {
    String line;
    size_t lineBytes = 0;
    bool newlineTerminated = false;
    if (!readJournalLine(source, line, lineBytes, newlineTerminated)) break;
    trimTrailingCr(line);
    JsonDocument record;
    const bool validJsonObject =
        deserializeJson(record, line) == DeserializationError::Ok &&
        record.is<JsonObject>();
    recovery.observeLine(lineBytes, newlineTerminated, validJsonObject);
  }
  source.close();
  if (!recovery.needsRecovery()) return true;

  source = LittleFS.open(JOURNAL_PATH, FILE_READ);
  if (!source) return false;
  LittleFS.remove(JOURNAL_TEMP_PATH);
  File target = LittleFS.open(JOURNAL_TEMP_PATH, FILE_WRITE);
  if (!target) {
    source.close();
    return false;
  }
  size_t remaining = recovery.retainedBytes();
  uint8_t buffer[128];
  bool copyFailed = false;
  while (remaining > 0) {
    const size_t wanted = min(sizeof(buffer), remaining);
    const size_t read = source.read(buffer, wanted);
    if (read != wanted || target.write(buffer, read) != read) {
      copyFailed = true;
      break;
    }
    remaining -= read;
  }
  target.flush();
  const bool writeError = target.getWriteError() != 0;
  source.close();
  target.close();
  if (copyFailed || writeError || remaining != 0) {
    LittleFS.remove(JOURNAL_TEMP_PATH);
    return false;
  }

  LittleFS.remove(JOURNAL_BACKUP_PATH);
  if (!LittleFS.rename(JOURNAL_PATH, JOURNAL_BACKUP_PATH)) {
    LittleFS.remove(JOURNAL_TEMP_PATH);
    return false;
  }
  if (!LittleFS.rename(JOURNAL_TEMP_PATH, JOURNAL_PATH)) {
    LittleFS.rename(JOURNAL_BACKUP_PATH, JOURNAL_PATH);
    return false;
  }
  LittleFS.remove(JOURNAL_BACKUP_PATH);
  return true;
}

void ShoreRuntime::replayJournal() {
  File journal = LittleFS.open(JOURNAL_PATH, FILE_READ);
  if (!journal) return;
  while (journal.available()) {
    String line = journal.readStringUntil('\n');
    const size_t originalBytes = line.length() + 1;
    trimTrailingCr(line);
    journalBytes_ += originalBytes;
    JsonDocument record;
    if (deserializeJson(record, line) != DeserializationError::Ok) continue;
    const uint32_t id = record["record_id"] | 0;
    if (id == 0) continue;
    ++journalCount_;
    if (firstStoredRecordId_ == 0 || id < firstStoredRecordId_) firstStoredRecordId_ = id;
    if (id >= nextRecordId_) {
      nextRecordId_ = id == 0xFFFFFFFFUL ? 0x80000000UL : id + 1;
    }
    const String nonce = record["nonce"] | "";
    if (nonce.length() == 16) {
      journalNonceIndex_.add(replayIndexHash(nonce));
    }
    const String kind = record["kind"] | "";
    if (kind == "event") {
      const String boatId = record["boat_id"] | "";
      const String packetNodeUid = record["node_uid"] | "";
      const String eventNonce = record["nonce"] | "";
      const String incidentId = record["incident_id"] | "";
      if (incidentId.length() == 16) {
        journalIncidentIndex_.add(replayIndexHash(incidentId));
      }
      const String status = record["status"] | "normal";
      const bool gpsValid = record["latitude"].is<float>() &&
                            record["longitude"].is<float>();
      if (gpsValid) {
        const float latitude = record["latitude"] | 0.0f;
        const float longitude = record["longitude"] | 0.0f;
        if (latitude >= -90.0f && latitude <= 90.0f &&
            longitude >= -180.0f && longitude <= 180.0f) {
          hasLastLocation_ = true;
          lastLatitude_ = latitude;
          lastLongitude_ = longitude;
          hasLastGpsAge_ = record["gps_age_s"].is<uint32_t>();
          if (hasLastGpsAge_) {
            lastGpsAgeSeconds_ = record["gps_age_s"].as<uint32_t>();
          }
        }
      }
      const auto severity = status == "sos" ? coastcue::AlertReducer::kSos
                            : status == "help" ? coastcue::AlertReducer::kHelp
                                               : coastcue::AlertReducer::kNone;
      alerts_.replayEvent(id, severity);
      if (severity != coastcue::AlertReducer::kNone) {
        if (alerts_.activeRecordId() == id) {
          if (!incidentId.isEmpty()) {
            activeIncidentId_ = incidentId;
            activeIncidentNonce_ = eventNonce;
            activeBoatNodeUid_ = packetNodeUid;
            activeIncidentStateSeq_ = record["radio_state_seq"] | 1U;
            activeIncidentDurable_ = true;
            pendingRadioDownlinks_.clear();
            if (confirmedIncidentId_ != activeIncidentId_) {
              confirmedIncidentStateSeq_ = 0;
            }
            queueIncidentDownlink("receipt", activeIncidentStateSeq_);
          } else {
            clearActiveIncidentIdentity();
          }
        }
      }
    } else if (kind == "transition") {
      const uint32_t targetRecordId = record["target_record_id"] | 0;
      const String state = record["state"] | "";
      alerts_.replayTransition(targetRecordId, state.c_str());
      const String incidentId = record["incident_id"] | "";
      const uint32_t stateSeq = record["radio_state_seq"] | 0U;
      if (targetRecordId == alerts_.activeRecordId() &&
          incidentId == activeIncidentId_ && stateSeq > 0 &&
          (state == "acknowledged" || state == "resolved")) {
        activeIncidentStateSeq_ = max(activeIncidentStateSeq_, stateSeq);
        queueIncidentDownlink(state, stateSeq);
      }
    }
  }
  journal.close();
  activeRecordId_ = alerts_.activeRecordId();
  activeSeverity_ = alerts_.activeSeverity();
  alertState_ = alerts_.state();
  silenced_ = alertState_ == "acknowledged" || alertState_ == "resolved";
}

bool ShoreRuntime::rebuildJournalReplayIndexes() {
  // This runs only after a successful atomic journal replacement. Never use a
  // partial rebuilt index: malformed or unreadable retained history must keep
  // replay admission fail-closed.
  journalNonceIndex_.reset();
  journalIncidentIndex_.reset();
  File journal = LittleFS.open(JOURNAL_PATH, FILE_READ);
  if (!journal) {
    journalNonceIndex_.failClosed();
    journalIncidentIndex_.failClosed();
    return false;
  }
  bool valid = true;
  while (journal.available()) {
    String line = journal.readStringUntil('\n');
    trimTrailingCr(line);
    JsonDocument record;
    if (deserializeJson(record, line) != DeserializationError::Ok ||
        !record.is<JsonObject>()) {
      valid = false;
      break;
    }
    const String nonce = record["nonce"] | "";
    if (nonce.length() == 16) journalNonceIndex_.add(replayIndexHash(nonce));
    const String incidentId = record["incident_id"] | "";
    if (incidentId.length() == 16) {
      journalIncidentIndex_.add(replayIndexHash(incidentId));
    }
  }
  journal.close();
  if (!valid) {
    journalNonceIndex_.failClosed();
    journalIncidentIndex_.failClosed();
  }
  return valid && journalNonceIndex_.complete() &&
         journalIncidentIndex_.complete();
}

void ShoreRuntime::appendRadioEvent(const String &wirePacket, const String &nonce,
                                    const String &status,
                                    const String &incidentId, float rssi,
                                    float snr) {
  if (!storageReady_) tryRecoverStorage();
  if (storageReady_ && nextRecordId_ >= 0x80000000UL) {
    storageReady_ = false;
    Serial.println("[SHORE] persistent record ID space exhausted; live-only mode");
  }
  uint32_t recordId = storageReady_ ? nextRecordId_ : allocateVolatileRecordId();
  JsonDocument packet;
  const bool packetParsed =
      deserializeJson(packet, wirePacket) == DeserializationError::Ok;
  JsonDocument event;
  event["kind"] = "event";
  event["record_id"] = recordId;
  event["uptime_ms"] = millis();
  const uint64_t unixMs = currentUnixMs();
  if (unixMs != 0) event["unix_ms"] = unixMs;
  event["time_quality"] = unixMs != 0 ? "phone_set" : "uptime_only";
  event["rssi"] = rssi;
  event["snr"] = snr;
  event["packet_b64"] = base64(wirePacket);
  event["nonce"] = nonce;
  event["status"] = status;
  if (!incidentId.isEmpty()) {
    event["incident_id"] = incidentId;
    event["radio_state_seq"] = 1;
  }
  const auto severity = status == "sos" ? coastcue::AlertReducer::kSos
                        : status == "help" ? coastcue::AlertReducer::kHelp
                                           : coastcue::AlertReducer::kNone;
  coastcue::AlertReducer preview = alerts_;
  const auto disposition = preview.ingest(recordId, severity);
  if (severity != coastcue::AlertReducer::kNone) {
    if (disposition.kind == coastcue::AlertReducer::kSuperseded) {
      event["alert_state"] = "open";
      event["supersedes_record_id"] = disposition.previousRecordId;
    } else if (disposition.kind == coastcue::AlertReducer::kAbsorbed) {
      event["alert_state"] = "resolved";
      event["source"] = "absorbed";
    } else {
      event["alert_state"] = "open";
    }
  }
  String packetNodeUid;
  if (packetParsed) {
    packetNodeUid = String(packet["node_uid"] | "");
    if (!packetNodeUid.isEmpty()) {
      event["node_uid"] = packetNodeUid;
      lastNodeUid_ = packetNodeUid;
    }
    const String boatId = configuredBoatId_;
    if (!boatId.isEmpty()) {
      event["boat_id"] = boatId;
    }
    if (!packet["battery_percent"].isNull()) {
      event["battery_percent"] = packet["battery_percent"];
    }
    const bool gpsValid = packet["gps"]["valid"] | false;
    if (gpsValid && !packet["gps"]["latitude"].isNull() &&
        !packet["gps"]["longitude"].isNull()) {
      const float latitude = packet["gps"]["latitude"] | 0.0f;
      const float longitude = packet["gps"]["longitude"] | 0.0f;
      event["latitude"] = latitude;
      event["longitude"] = longitude;
      if (packet["gps"]["age_s"].is<uint32_t>()) {
        event["gps_age_s"] = packet["gps"]["age_s"];
      }
      if (latitude >= -90.0f && latitude <= 90.0f &&
          longitude >= -180.0f && longitude <= 180.0f) {
        hasLastLocation_ = true;
        lastLatitude_ = latitude;
        lastLongitude_ = longitude;
        hasLastGpsAge_ = packet["gps"]["age_s"].is<uint32_t>();
        if (hasLastGpsAge_) {
          lastGpsAgeSeconds_ = packet["gps"]["age_s"].as<uint32_t>();
        }
      }
    }
  }
  lastRssi_ = rssi;
  lastSnr_ = snr;
  lastPacketAt_ = millis();
  hasRadioMetrics_ = true;
  const bool attemptedDurableAppend = storageReady_;
  bool checkpointWritten = false;
  String checkpointEncoded;
  if (coastcue::shouldCheckpointVolatileEmergency(status.c_str(), false)) {
    JsonDocument checkpoint = event;
    checkpoint["durable"] = false;
    checkpoint["checkpoint"] = true;
    checkpointEncoded = jsonString(checkpoint);
    checkpointWritten = checkpointEmergency(checkpointEncoded);
  }
  event["durable"] = attemptedDurableAppend;
  const bool persisted = appendJournal(jsonString(event));
  if (persisted && checkpointWritten) {
    clearEmergencyCheckpointIfMatches(nonce);
  }
  if (!persisted && attemptedDurableAppend) {
    recordId = allocateVolatileRecordId();
    event["record_id"] = recordId;
  }
  event["durable"] = persisted;
  const String encoded = jsonString(event);
  // Live event always precedes a derived transition on the BLE queue.
  notifyEvent(encoded);
  if (persisted) {
    if (nonce.length() == 16) {
      journalNonceIndex_.add(replayIndexHash(nonce));
    }
    if (incidentId.length() == 16) {
      journalIncidentIndex_.add(replayIndexHash(incidentId));
    }
    if (firstStoredRecordId_ == 0) firstStoredRecordId_ = recordId;
    ++nextRecordId_;
    Preferences preferences;
    preferences.begin("coastcue", false);
    preferences.putUInt("next_id", nextRecordId_);
    preferences.end();
    compactJournalIfNeeded();
  }
  if (status == "help" || status == "sos") {
    applyEmergency(recordId, status, persisted);
    if (coastcue::shouldBindIncidentIdentity(
            true, !incidentId.isEmpty(), !packetNodeUid.isEmpty(),
            activeRecordId_ == recordId)) {
      bindActiveIncidentIdentity(incidentId, packetNodeUid, 1, persisted);
      activeIncidentNonce_ = nonce;
      activeLiveEmergency_ = persisted ? String() : checkpointEncoded;
    } else if (incidentId.isEmpty() && activeRecordId_ == recordId) {
      clearActiveIncidentIdentity();
    }
  }
  notifyStatus();
}

bool ShoreRuntime::appendTransition(uint32_t targetRecordId, const char *state,
                                    const char *source) {
  if (storageReady_ && nextRecordId_ >= 0x80000000UL) {
    storageReady_ = false;
    Serial.println("[SHORE] persistent record ID space exhausted; live-only mode");
  }
  uint32_t recordId = storageReady_ ? nextRecordId_ : allocateVolatileRecordId();
  JsonDocument transition;
  transition["kind"] = "transition";
  transition["record_id"] = recordId;
  transition["target_record_id"] = targetRecordId;
  transition["state"] = state;
  transition["source"] = source;
  const bool operatorRadioTransition =
      !activeIncidentId_.isEmpty() && targetRecordId == activeRecordId_ &&
      (strcmp(state, "acknowledged") == 0 ||
       strcmp(state, "resolved") == 0) &&
      (strcmp(source, "ble") == 0 || strcmp(source, "button") == 0);
  const uint32_t radioStateSeq =
      operatorRadioTransition ? activeIncidentStateSeq_ + 1 : 0;
#if COASTCUE_BENCH_SHORE_FAIL_FIRST_OPERATOR_TRANSITION
  static bool operatorTransitionFaultInjected = false;
  if (operatorRadioTransition && !operatorTransitionFaultInjected) {
    operatorTransitionFaultInjected = true;
    storageReady_ = false;
    benchStorageRecoverySuppressed = true;
    Serial.println(
        "[BENCH-FAULT] injected shore operator-transition append failure; "
        "storage recovery suppressed until reboot");
    return false;
  }
#endif
  if (operatorRadioTransition) {
    transition["incident_id"] = activeIncidentId_;
    transition["radio_state_seq"] = radioStateSeq;
  }
  transition["uptime_ms"] = millis();
  const uint64_t unixMs = currentUnixMs();
  if (unixMs != 0) transition["unix_ms"] = unixMs;
  transition["time_quality"] = unixMs != 0 ? "phone_set" : "uptime_only";
  const bool attemptedDurableAppend = storageReady_;
  transition["durable"] = attemptedDurableAppend;
  const bool persisted = appendJournal(jsonString(transition));
  if (!persisted && attemptedDurableAppend) {
    transition["record_id"] = allocateVolatileRecordId();
  }
  transition["durable"] = persisted;
  if (persisted) {
    notifyEvent(jsonString(transition));
  }
  if (!persisted) return false;
  if (firstStoredRecordId_ == 0) firstStoredRecordId_ = recordId;
  ++nextRecordId_;
  Preferences preferences;
  preferences.begin("coastcue", false);
  preferences.putUInt("next_id", nextRecordId_);
  preferences.end();
  compactJournalIfNeeded();
  if (operatorRadioTransition) {
    activeIncidentStateSeq_ = radioStateSeq;
    queueIncidentDownlink(String(state), radioStateSeq);
  }
  return true;
}

void ShoreRuntime::applyEmergency(uint32_t recordId, const String &status,
                                  bool eventPersisted) {
  const auto severity = status == "sos" ? coastcue::AlertReducer::kSos
                                         : coastcue::AlertReducer::kHelp;
  const auto action = alerts_.ingest(recordId, severity);
  // Every event has a deterministic terminal state. A newer same/higher
  // severity supersedes the old one; Help under active SOS resolves itself.
  if (action.kind == coastcue::AlertReducer::kSuperseded && !eventPersisted) {
    Serial.printf("[SHORE] live-only supersede event=%lu; checkpoint retains recovery intent\n",
                  static_cast<unsigned long>(recordId));
  } else if (action.kind == coastcue::AlertReducer::kAbsorbed &&
             !eventPersisted) {
    Serial.printf("[SHORE] live-only absorbed event=%lu; checkpoint retains recovery intent\n",
                  static_cast<unsigned long>(recordId));
  } else if (action.kind == coastcue::AlertReducer::kSuperseded) {
    if (!appendTransition(action.previousRecordId, "superseded", "superseded")) {
      // The already-durable replacement event contains supersedes_record_id,
      // so replay deterministically reaches the same reducer state even when
      // its derived transition row cannot be written.
      Serial.printf("[SHORE] supersede transition append failed; replacement event=%lu remains authoritative\n",
                    static_cast<unsigned long>(recordId));
    }
  } else if (action.kind == coastcue::AlertReducer::kAbsorbed) {
    if (!appendTransition(action.previousRecordId, "resolved", "absorbed")) {
      // The durable absorbed event carries alert_state=resolved/source=absorbed
      // and is therefore the authoritative replay record.
      Serial.printf("[SHORE] absorbed transition append failed; event=%lu remains authoritative\n",
                    static_cast<unsigned long>(recordId));
    }
  }
  activeRecordId_ = alerts_.activeRecordId();
  activeSeverity_ = alerts_.activeSeverity();
  alertState_ = alerts_.state();
  if (action.kind == coastcue::AlertReducer::kOpened ||
      action.kind == coastcue::AlertReducer::kSuperseded) {
    silenced_ = false;
  }
}

void ShoreRuntime::acknowledge(uint32_t recordId, const String &commandId,
                               const char *source) {
  bool valid = alerts_.canApplyOperatorAction(recordId);
  bool persistenceFailed = false;
  const bool changed = valid && alertState_ != "acknowledged";
  if (valid) {
    const bool durable =
        !changed || appendTransition(activeRecordId_, "acknowledged", source);
    if (coastcue::mayApplyOperatorTransition(changed, durable)) {
      if (changed) alerts_.replayTransition(activeRecordId_, "acknowledged");
      alertState_ = alerts_.state();
      silenced_ = true;
      digitalWrite(COASTCUE_PIN_BUZZER, LOW);
      Serial.printf("[SHORE] operator acknowledged record=%lu source=%s\n",
                    static_cast<unsigned long>(recordId == 0 ? activeRecordId_
                                                             : recordId),
                    source != nullptr ? source : "");
    } else {
      valid = false;
      persistenceFailed = true;
    }
  }
  JsonDocument result;
  result["kind"] = "command_result";
  if (!commandId.isEmpty()) result["command_id"] = commandId;
  result["op"] = "ack";
  result["record_id"] = recordId == 0 ? activeRecordId_ : recordId;
  result["source"] = source;
  result["ok"] = valid;
  result["durable"] = storageReady_;
  if (!valid) {
    result["error"] =
        persistenceFailed ? "persistence_failed" : "alert_not_active";
  }
  const bool resultQueued = notifyOperatorResult(jsonString(result));
  Serial.printf(
      "[SHORE] BLE operator result op=ack ok=%s error=%s requested=%lu active=%lu reducer=%lu state=%s reducer_state=%s storage=%s queued=%s\n",
      valid ? "true" : "false",
      valid ? "" : (persistenceFailed ? "persistence_failed"
                                           : "alert_not_active"),
      static_cast<unsigned long>(recordId),
      static_cast<unsigned long>(activeRecordId_),
      static_cast<unsigned long>(alerts_.activeRecordId()), alertState_.c_str(),
      alerts_.state(), storageReady_ ? "ready" : "failed",
      resultQueued ? "true" : "false");
  if (valid) notifyStatus();
}

void ShoreRuntime::resolve(uint32_t recordId, const String &commandId,
                           const char *source) {
  bool valid = alerts_.canApplyOperatorAction(recordId);
  bool persistenceFailed = false;
  const bool changed = valid && alertState_ != "resolved";
  if (changed) {
    const bool durable = appendTransition(activeRecordId_, "resolved", source);
    if (coastcue::mayApplyOperatorTransition(changed, durable)) {
      alerts_.replayTransition(activeRecordId_, "resolved");
      alertState_ = alerts_.state();
      activeSeverity_ = alerts_.activeSeverity();
      silenced_ = true;
      digitalWrite(COASTCUE_PIN_BUZZER, LOW);
      Serial.printf("[SHORE] operator resolved record=%lu source=%s\n",
                    static_cast<unsigned long>(recordId == 0 ? activeRecordId_
                                                             : recordId),
                    source != nullptr ? source : "");
    } else {
      valid = false;
      persistenceFailed = true;
    }
  }
  JsonDocument result;
  result["kind"] = "command_result";
  if (!commandId.isEmpty()) result["command_id"] = commandId;
  result["op"] = "resolve";
  result["record_id"] = recordId == 0 ? activeRecordId_ : recordId;
  result["source"] = source;
  result["ok"] = valid;
  result["durable"] = storageReady_;
  if (!valid) {
    result["error"] =
        persistenceFailed ? "persistence_failed" : "alert_not_active";
  }
  const bool resultQueued = notifyOperatorResult(jsonString(result));
  Serial.printf(
      "[SHORE] BLE operator result op=resolve ok=%s error=%s requested=%lu active=%lu reducer=%lu state=%s reducer_state=%s storage=%s queued=%s\n",
      valid ? "true" : "false",
      valid ? "" : (persistenceFailed ? "persistence_failed"
                                           : "alert_not_active"),
      static_cast<unsigned long>(recordId),
      static_cast<unsigned long>(activeRecordId_),
      static_cast<unsigned long>(alerts_.activeRecordId()), alertState_.c_str(),
      alerts_.state(), storageReady_ ? "ready" : "failed",
      resultQueued ? "true" : "false");
  if (valid) notifyStatus();
}

void ShoreRuntime::handleCommand(const String &command) {
  if (commandQueue_ == nullptr || operatorCommandQueue_ == nullptr ||
      busyCommandQueue_ == nullptr) {
    return;
  }
  CommandRequest request = {};
  const size_t copied = min(command.length(), sizeof(request.value) - 1);
  memcpy(request.value, command.c_str(), copied);
  request.value[copied] = '\0';
  const bool isOperator =
      coastcue::isOperatorSafetyCommandJson(request.value);
  const auto targetKind = coastcue::decideCommandEnqueueTarget(
      isOperator, uxQueueSpacesAvailable(commandQueue_) > 0,
      uxQueueSpacesAvailable(operatorCommandQueue_) > 0,
      copied != command.length());
  QueueHandle_t target = busyCommandQueue_;
  if (targetKind == coastcue::CommandEnqueueTarget::kGeneral) {
    target = commandQueue_;
  } else if (targetKind == coastcue::CommandEnqueueTarget::kOperator) {
    target = operatorCommandQueue_;
  }
  if (xQueueSend(target, &request, 0) != pdTRUE && target != busyCommandQueue_) {
    xQueueSend(busyCommandQueue_, &request, 0);
  }
}

void ShoreRuntime::executeCommand(const String &command) {
  JsonDocument input;
  JsonDocument output;
  output["kind"] = "command_result";
  if (deserializeJson(input, command) != DeserializationError::Ok) {
    char commandIdBuf[65] = {};
    char opBuf[16] = {};
    if (coastcue::copyJsonStringField(command.c_str(), "command_id",
                                      commandIdBuf, sizeof(commandIdBuf))) {
      output["command_id"] = commandIdBuf;
    }
    if (coastcue::copyJsonStringField(command.c_str(), "op", opBuf,
                                      sizeof(opBuf))) {
      output["op"] = opBuf;
    }
    output["ok"] = false;
    output["error"] = "invalid_json";
    const String encoded = jsonString(output);
    if (coastcue::isOperatorSafetyCommand(opBuf)) {
      notifyOperatorResult(encoded);
    } else {
      notifyEvent(encoded);
    }
    return;
  }
  const String op = input["op"] | "";
  if (coastcue::isOperatorSafetyCommand(op.c_str())) {
    const uint32_t requestedRecordId = bleCommandRecordId(input["record_id"]);
    Serial.printf(
        "[SHORE] BLE operator command op=%s bytes=%u requested=%lu active=%lu reducer=%lu state=%s reducer_state=%s storage=%s\n",
        op.c_str(), static_cast<unsigned>(command.length()),
        static_cast<unsigned long>(requestedRecordId),
        static_cast<unsigned long>(activeRecordId_),
        static_cast<unsigned long>(alerts_.activeRecordId()), alertState_.c_str(),
        alerts_.state(),
        storageReady_ ? "ready" : "failed");
  }
  String commandId;
  if (coastcue::commandRequiresCommandId(op.c_str())) {
    // ArduinoJson 7.x selects a null-default overload for `variant | nullptr`
    // and returns nullptr even when the JSON value is a valid string. Extract
    // into owned storage from the already validated JSON instead.
    char commandIdValue[65] = {};
    if (!coastcue::copyJsonStringField(command.c_str(), "command_id",
                                       commandIdValue,
                                       sizeof(commandIdValue)) ||
        !coastcue::isValidCommandId(commandIdValue)) {
      output["ok"] = false;
      output["error"] = "missing_command_id";
      output["op"] = op;
      notifyOperatorResult(jsonString(output));
      return;
    }
    commandId = commandIdValue;
    output["command_id"] = commandId;
  }
  if (op == "sync") {
    const int requested = input["limit"] | 20;
    const String syncId = input["sync_id"] | "";
    const uint64_t unixMs = input["unix_ms"] | 0ULL;
    if (unixMs > 0) {
      unixBaseMs_ = unixMs;
      unixBaseUptimeMs_ = esp_timer_get_time() / 1000ULL;
    }
    if (syncId.isEmpty()) {
      output["ok"] = false;
      output["error"] = "missing_sync_id";
      notifyEvent(jsonString(output));
    } else if (!beginSync(input["after"] | 0,
                          static_cast<uint8_t>(max(1, min(requested, 20))), syncId,
                          connectionGeneration_)) {
      notifyEvent(String(coastcue::encodeSyncFailureResult(
                              {syncId.c_str(), "busy"})
                              .c_str()));
    }
    return;
  }
  if (op == "ack") {
    acknowledge(bleCommandRecordId(input["record_id"]), commandId, "ble");
    return;
  }
  if (op == "resolve") {
    resolve(bleCommandRecordId(input["record_id"]), commandId, "ble");
    return;
  }
  if (op == "time") {
    const uint64_t unixMs = input["unix_ms"] | 0ULL;
    if (unixMs > 0) {
      unixBaseMs_ = unixMs;
      unixBaseUptimeMs_ = esp_timer_get_time() / 1000ULL;
      output["ok"] = true;
      output["op"] = "time";
      notifyStatus();
    } else {
      output["ok"] = false;
      output["error"] = "invalid_time";
    }
    notifyEvent(jsonString(output));
    return;
  }
  output["ok"] = false;
  output["error"] = "unsupported_op";
  notifyEvent(jsonString(output));
}

void ShoreRuntime::processNextCommand() {
  CommandRequest command = {};
  while (operatorCommandQueue_ != nullptr &&
         xQueueReceive(operatorCommandQueue_, &command, 0) == pdTRUE) {
    executeCommand(String(command.value));
  }
  if (busyCommandQueue_ != nullptr &&
      xQueueReceive(busyCommandQueue_, &command, 0) == pdTRUE) {
    notifyBusyCommand(command);
    return;
  }
  if (commandQueue_ != nullptr &&
      xQueueReceive(commandQueue_, &command, 0) == pdTRUE) {
    executeCommand(String(command.value));
  }
}

void ShoreRuntime::notifyBusyCommand(const CommandRequest &command) {
  coastcue::BusyCommandResult result{"", "", 0, false};
  JsonDocument input;
  if (deserializeJson(input, command.value) == DeserializationError::Ok &&
      input.is<JsonObjectConst>()) {
    const JsonObjectConst object = input.as<JsonObjectConst>();
    if (object["command_id"].is<const char *>()) {
      result.commandId = object["command_id"].as<const char *>();
    }
    if (object["op"].is<const char *>()) {
      result.operation = object["op"].as<const char *>();
    }
    const JsonVariantConst recordId = object["record_id"];
    if (recordId.is<int64_t>()) {
      result.recordId = recordId.as<int64_t>();
      result.hasRecordId = true;
    } else if (recordId.is<uint64_t>()) {
      const uint64_t unsignedId = recordId.as<uint64_t>();
      if (unsignedId <= static_cast<uint64_t>(INT64_MAX)) {
        result.recordId = static_cast<int64_t>(unsignedId);
        result.hasRecordId = true;
      }
    }
  }
  notifyEvent(String(coastcue::encodeBusyCommandResult(result).c_str()));
}

bool ShoreRuntime::beginSync(uint32_t after, uint8_t limit, const String &syncId,
                             uint32_t connectionGeneration) {
  if (!bleConnected_ || syncId.isEmpty()) return false;
  if (pendingSync_.active) return false;
  pendingSync_.active = true;
  pendingSync_.syncId = syncId;
  pendingSync_.connectionGeneration = connectionGeneration;
  pendingSync_.scanner.begin(after, limit);
  pendingSync_.fileOffset = coastcue::syncJournalStartOffset(
      after, syncHintAfter_, syncHintOffset_);
  Serial.printf("[SHORE] sync begin after=%lu limit=%u generation=%lu\n",
                static_cast<unsigned long>(after), static_cast<unsigned>(limit),
                static_cast<unsigned long>(connectionGeneration));
  return true;
}

void ShoreRuntime::clearPendingSync() {
  pendingSync_.active = false;
  pendingSync_.syncId = "";
  pendingSync_.connectionGeneration = 0;
  pendingSync_.fileOffset = 0;
  pendingSync_.scanner.begin(0, 1);
}

void ShoreRuntime::failSync(const char *error) {
  if (!pendingSync_.active) return;
  const String syncId = pendingSync_.syncId;
  const uint32_t generation = pendingSync_.connectionGeneration;
  clearPendingSync();
  if (syncId.isEmpty()) return;
  if (queueMutex_ != nullptr &&
      xSemaphoreTake(queueMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    for (auto it = outgoingMessages_.begin(); it != outgoingMessages_.end();) {
      if (it->connectionGeneration == generation &&
          it->json.indexOf(syncId) >= 0) {
        it = outgoingMessages_.erase(it);
      } else {
        ++it;
      }
    }
    if (outgoingMessages_.size() < MAX_OUTGOING_MESSAGES) {
      outgoingMessages_.push_back(
          {String(coastcue::encodeSyncFailureResult({syncId.c_str(), error})
                      .c_str()),
           generation});
    }
    xSemaphoreGive(queueMutex_);
    return;
  }
  enqueueOutgoing(
      String(coastcue::encodeSyncFailureResult({syncId.c_str(), error}).c_str()),
      generation);
}

void ShoreRuntime::finishSyncPage() {
  if (!pendingSync_.active) return;
  const coastcue::SyncPageScanner &scanner = pendingSync_.scanner;
  JsonDocument completion;
  completion["kind"] = "sync_complete";
  completion["last_record_id"] = scanner.last();
  completion["first_record_id"] = scanner.firstReturned();
  completion["count"] = scanner.sent();
  completion["has_more"] = scanner.hasMore();
  completion["truncated"] = scanner.hasGap() ||
      (firstStoredRecordId_ != 0 &&
       firstStoredRecordId_ > scanner.after() + 1);
  if (truncatedBeforeRecordId_ != 0) {
    completion["truncated_before_record_id"] = truncatedBeforeRecordId_;
  }
  if (scanner.missingRangeBoundCount() > 0) {
    JsonArray ranges = completion["missing_record_id_ranges"].to<JsonArray>();
    for (size_t i = 0; i + 1 < scanner.missingRangeBoundCount(); i += 2) {
      JsonArray range = ranges.add<JsonArray>();
      range.add(scanner.missingRangeBounds()[i]);
      range.add(scanner.missingRangeBounds()[i + 1]);
    }
  }
  completion["storage_ready"] = storageReady_;
  completion["sync_id"] = pendingSync_.syncId;
  const uint32_t generation = pendingSync_.connectionGeneration;
  if (!enqueueOutgoing(jsonString(completion), generation)) {
    Serial.println("[SHORE] sync complete enqueue failed");
    failSync("busy");
    return;
  }
  Serial.printf(
      "[SHORE] sync page queued count=%u last=%lu more=%s generation=%lu\n",
      static_cast<unsigned>(scanner.sent()),
      static_cast<unsigned long>(scanner.last()),
      scanner.hasMore() ? "yes" : "no",
      static_cast<unsigned long>(generation));
  syncHintAfter_ = scanner.last();
  syncHintOffset_ = pendingSync_.fileOffset;
  clearPendingSync();
}

void ShoreRuntime::serviceSyncScan() {
  if (!pendingSync_.active) return;
  if (pendingSync_.connectionGeneration != connectionGeneration_ ||
      !bleConnected_) {
    clearPendingSync();
    return;
  }
  if (!storageReady_) {
    finishSyncPage();
    return;
  }

  File journal = LittleFS.open(JOURNAL_PATH, FILE_READ);
  if (!journal) {
    finishSyncPage();
    return;
  }
  if (pendingSync_.fileOffset > 0 &&
      !journal.seek(pendingSync_.fileOffset)) {
    journal.close();
    failSync("storage");
    return;
  }

  size_t lines = 0;
  bool pageDone = false;
  while (journal.available()) {
    const size_t lineStart = journal.position();
    String line = journal.readStringUntil('\n');
    trimTrailingCr(line);
    pendingSync_.fileOffset = journal.position();
    ++lines;

    JsonDocument event;
    if (deserializeJson(event, line) == DeserializationError::Ok) {
      const uint32_t id = event["record_id"] | 0;
      const coastcue::SyncRecordDisposition disposition =
          pendingSync_.scanner.observe(id);
      if (disposition == coastcue::SyncRecordDisposition::kAccept) {
        event["sync_id"] = pendingSync_.syncId;
        if (!enqueueOutgoing(jsonString(event),
                             pendingSync_.connectionGeneration)) {
          journal.close();
          failSync("busy");
          return;
        }
        syncHintAfter_ = id;
        syncHintOffset_ = pendingSync_.fileOffset;
      } else if (disposition == coastcue::SyncRecordDisposition::kPageFull) {
        // Rewind so the overflowing record remains available for the next page.
        pendingSync_.fileOffset = lineStart;
        pageDone = true;
        break;
      }
    }

    if (pendingSync_.scanner.shouldYield(lines)) break;
  }
  if (!journal.available()) pageDone = true;
  journal.close();
  if (pageDone) finishSyncPage();
}

bool ShoreRuntime::enqueueOutgoing(const String &json,
                                   uint32_t connectionGeneration) {
  if (!bleConnected_ || queueMutex_ == nullptr ||
      xSemaphoreTake(queueMutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
    return false;
  }
  const bool validLink =
      connectionGeneration == connectionGeneration_ && bleConnected_;
  if (!validLink) {
    xSemaphoreGive(queueMutex_);
    return false;
  }

  const coastcue::OutgoingKind kind =
      coastcue::classifyOutgoingJson(json.c_str());
  const coastcue::OutgoingAdmission admission =
      coastcue::decideOutgoingAdmission(kind, outgoingMessages_.size(),
                                        MAX_OUTGOING_MESSAGES);
  bool queued = false;
  bool droppedSync = false;

  if (admission == coastcue::OutgoingAdmission::kEnqueueFront) {
    while (outgoingMessages_.size() >= MAX_OUTGOING_MESSAGES) {
      bool dropped = false;
      for (auto it = outgoingMessages_.begin(); it != outgoingMessages_.end();
           ++it) {
        const coastcue::OutgoingKind queuedKind =
            coastcue::classifyOutgoingJson(it->json.c_str());
        if (coastcue::mayDropOutgoingForOperator(queuedKind)) {
          it = outgoingMessages_.erase(it);
          dropped = true;
          droppedSync = true;
          break;
        }
      }
      if (dropped) continue;
      for (auto it = outgoingMessages_.end();
           it != outgoingMessages_.begin();) {
        --it;
        const coastcue::OutgoingKind queuedKind =
            coastcue::classifyOutgoingJson(it->json.c_str());
        if (queuedKind != coastcue::OutgoingKind::kOperatorResult) {
          if (queuedKind == coastcue::OutgoingKind::kSyncPayload) {
            droppedSync = true;
          }
          outgoingMessages_.erase(it);
          dropped = true;
          break;
        }
      }
      if (!dropped) break;
    }
    if (outgoingMessages_.size() < MAX_OUTGOING_MESSAGES) {
      if (droppedSync && pendingSync_.active &&
          pendingSync_.connectionGeneration == connectionGeneration) {
        const String syncId = pendingSync_.syncId;
        clearPendingSync();
        for (auto it = outgoingMessages_.begin();
             it != outgoingMessages_.end();) {
          if (it->connectionGeneration == connectionGeneration &&
              coastcue::classifyOutgoingJson(it->json.c_str()) ==
                  coastcue::OutgoingKind::kSyncPayload) {
            it = outgoingMessages_.erase(it);
          } else {
            ++it;
          }
        }
        if (!syncId.isEmpty() &&
            outgoingMessages_.size() + 1 < MAX_OUTGOING_MESSAGES) {
          outgoingMessages_.push_back(
              {String(coastcue::encodeSyncFailureResult(
                          {syncId.c_str(), "busy"})
                          .c_str()),
               connectionGeneration});
        }
      }
      outgoingMessages_.push_front({json, connectionGeneration});
      queued = true;
    }
  } else if (admission == coastcue::OutgoingAdmission::kEnqueueTail &&
             outgoingMessages_.size() < MAX_OUTGOING_MESSAGES) {
    outgoingMessages_.push_back({json, connectionGeneration});
    queued = true;
  }

  xSemaphoreGive(queueMutex_);
  return queued;
}

bool ShoreRuntime::notifyEvent(const String &json) {
  if (!bleConnected_ || eventsCharacteristic == nullptr) return false;
  return enqueueOutgoing(json, connectionGeneration_);
}

bool ShoreRuntime::notifyOperatorResult(const String &json) {
  if (!bleConnected_ || eventsCharacteristic == nullptr) return false;
  if (enqueueOutgoing(json, connectionGeneration_)) return true;

  // Pathological saturation: still emit a correlated failure the phone can
  // match by command_id instead of timing out as if nothing happened.
  JsonDocument parsed;
  String sourceOwned = "ble";
  coastcue::OperatorNotifyFailureResult failure{"", "", 0, false,
                                                sourceOwned.c_str()};
  if (deserializeJson(parsed, json) == DeserializationError::Ok) {
    if (parsed["command_id"].is<const char *>()) {
      failure.commandId = parsed["command_id"].as<const char *>();
    }
    if (parsed["op"].is<const char *>()) {
      failure.operation = parsed["op"].as<const char *>();
    }
    if (parsed["source"].is<const char *>()) {
      sourceOwned = parsed["source"].as<const char *>();
      failure.source = sourceOwned.c_str();
    }
    const JsonVariantConst recordId = parsed["record_id"];
    if (recordId.is<int64_t>()) {
      failure.recordId = recordId.as<int64_t>();
      failure.hasRecordId = true;
    }
  }
  const String signal(coastcue::encodeOperatorNotifyFailureResult(failure)
                          .c_str());
  if (enqueueOutgoing(signal, connectionGeneration_)) return false;
  Serial.println(
      "[SHORE] operator command_result notify saturated; phone must use Status");
  return false;
}

size_t ShoreRuntime::currentAttPayloadBytes() const {
  uint16_t mtu = 23;
  if (connectedHandle_ != 0xFFFF) {
    NimBLEServer *server = NimBLEDevice::getServer();
    if (server != nullptr) {
      const uint16_t peerMtu = server->getPeerMTU(connectedHandle_);
      if (peerMtu >= 23) mtu = peerMtu;
    }
  }
  return coastcue::bleAttPayloadBytes(mtu);
}

void ShoreRuntime::abandonActiveNotify(const char *error) {
  activeMessage_ = "";
  activeFragmentIndex_ = 0;
  activeFragmentCount_ = 0;
  notifyFailCount_ = 0;
  if (pendingSync_.active) {
    const String syncId = pendingSync_.syncId;
    const uint32_t generation = pendingSync_.connectionGeneration;
    clearPendingSync();
    if (queueMutex_ != nullptr) {
      // Drop any remaining queued sync payloads for this generation; the
      // failure result is the terminal outcome for this page.
      for (auto it = outgoingMessages_.begin(); it != outgoingMessages_.end();) {
        if (it->connectionGeneration == generation &&
            it->json.indexOf("\"sync_id\"") >= 0) {
          it = outgoingMessages_.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (!syncId.isEmpty()) {
      outgoingMessages_.push_back(
          {String(coastcue::encodeSyncFailureResult({syncId.c_str(), error})
                      .c_str()),
           generation});
    }
  }
}

void ShoreRuntime::serviceNotifications() {
  if (!bleConnected_ || eventsCharacteristic == nullptr || queueMutex_ == nullptr ||
      static_cast<int32_t>(millis() - nextNotifyAt_) < 0) {
    return;
  }
  if (xSemaphoreTake(queueMutex_, pdMS_TO_TICKS(5)) != pdTRUE) return;
  if (activeMessage_.isEmpty() && !outgoingMessages_.empty()) {
    const OutgoingMessage next = outgoingMessages_.front();
    outgoingMessages_.pop_front();
    if (next.connectionGeneration != connectionGeneration_) {
      xSemaphoreGive(queueMutex_);
      return;
    }
    activeMessage_ = next.json;
    activeMessageGeneration_ = next.connectionGeneration;
    activeMessageId_ = nextMessageId_++;
    activeMessageCrc_ = crc32(
        reinterpret_cast<const uint8_t *>(activeMessage_.c_str()),
        activeMessage_.length());
    activeFragmentIndex_ = 0;
    const size_t attPayload = currentAttPayloadBytes();
    activeUnframedLimit_ = coastcue::bleEventUnframedLimit(attPayload);
    activeChunkBytes_ = coastcue::bleEventChunkBytes(attPayload);
    if (activeChunkBytes_ == 0 &&
        activeMessage_.length() > activeUnframedLimit_) {
      abandonActiveNotify("notify_failed");
      nextNotifyAt_ = millis() + NOTIFY_GAP_MS;
      xSemaphoreGive(queueMutex_);
      return;
    }
    if (activeMessage_.length() <= activeUnframedLimit_) {
      activeFragmentCount_ = 1;
    } else {
      activeFragmentCount_ = static_cast<uint16_t>(
          (activeMessage_.length() + activeChunkBytes_ - 1) / activeChunkBytes_);
    }
    notifyFailCount_ = 0;
  }
  if (activeMessage_.isEmpty() || activeMessageGeneration_ != connectionGeneration_) {
    activeMessage_ = "";
    xSemaphoreGive(queueMutex_);
    return;
  }

  bool notified = false;
  if (activeMessage_.length() <= activeUnframedLimit_) {
    notified = eventsCharacteristic->notify(
        reinterpret_cast<const uint8_t *>(activeMessage_.c_str()),
        activeMessage_.length());
  } else {
    const size_t offset = activeFragmentIndex_ * activeChunkBytes_;
    const size_t size = min(activeChunkBytes_, activeMessage_.length() - offset);
    uint8_t frame[16 + EVENT_CHUNK_BYTES];
    frame[0] = 'S';
    frame[1] = 'M';
    frame[2] = 1;
    frame[3] = EVENT_MESSAGE_TYPE;
    frame[4] = activeMessageId_ & 0xFF;
    frame[5] = (activeMessageId_ >> 8) & 0xFF;
    frame[6] = (activeMessageId_ >> 16) & 0xFF;
    frame[7] = (activeMessageId_ >> 24) & 0xFF;
    frame[8] = activeFragmentIndex_ & 0xFF;
    frame[9] = (activeFragmentIndex_ >> 8) & 0xFF;
    frame[10] = activeFragmentCount_ & 0xFF;
    frame[11] = (activeFragmentCount_ >> 8) & 0xFF;
    frame[12] = activeMessageCrc_ & 0xFF;
    frame[13] = (activeMessageCrc_ >> 8) & 0xFF;
    frame[14] = (activeMessageCrc_ >> 16) & 0xFF;
    frame[15] = (activeMessageCrc_ >> 24) & 0xFF;
    memcpy(frame + 16, activeMessage_.c_str() + offset, size);
    notified = eventsCharacteristic->notify(frame, size + 16);
  }

  const coastcue::NotifyDeliveryDecision decision =
      coastcue::decideNotifyDelivery(
          notified, notifyFailCount_,
          coastcue::classifyOutgoingJson(activeMessage_.c_str()) ==
              coastcue::OutgoingKind::kOperatorResult);
  notifyFailCount_ = decision.nextFailCount;
  if (decision.action == coastcue::NotifyDeliveryAction::kAdvance) {
    if (activeMessage_.length() <= activeUnframedLimit_) {
      activeMessage_ = "";
    } else if (++activeFragmentIndex_ >= activeFragmentCount_) {
      activeMessage_ = "";
    }
    nextNotifyAt_ = millis() + decision.delayMs;
  } else if (decision.action == coastcue::NotifyDeliveryAction::kAbandon) {
    abandonActiveNotify("notify_failed");
    nextNotifyAt_ = millis() + NOTIFY_GAP_MS;
  } else {
    nextNotifyAt_ = millis() + decision.delayMs;
  }
  xSemaphoreGive(queueMutex_);
}

void ShoreRuntime::notifyStatus() {
  if (statusCharacteristic == nullptr) return;
  const uint32_t packetAge = hasRadioMetrics_ ? millis() - lastPacketAt_ : UINT32_MAX;
  const bool radioFresh = coastcue::isRadioFresh(
      hasRadioMetrics_, millis(), lastPacketAt_, 900000UL);
  const coastcue::StatusSnapshot snapshot{
      storageReady_,
      journalCount_,
      lastNodeUid_.c_str(),
      hasRadioMetrics_,
      lastRssi_,
      lastSnr_,
      packetAge,
      radioFresh ? "recent" : "stale",
      alertState_.c_str(),
      activeRecordId_,
      activeSeverity_ == 2 ? "sos" : activeSeverity_ == 1 ? "help" : "none",
      pairingOpen()};
  const std::string encoded = coastcue::encodeStatusPayload(snapshot);
  statusCharacteristic->setValue(
      reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
  if (bleConnected_) statusCharacteristic->notify();
}

void ShoreRuntime::serviceButton() {
  const uint32_t now = millis();
  const bool raw = digitalRead(COASTCUE_PIN_BUTTON);
  if (raw != buttonRaw_) { buttonRaw_ = raw; buttonChangedAt_ = now; }
  if (raw != buttonStable_ && now - buttonChangedAt_ >= BUTTON_DEBOUNCE_MS) {
    buttonStable_ = raw;
    if (raw == LOW) {
      buttonPressedAt_ = now;
    } else {
      const uint32_t held = now - buttonPressedAt_;
      if (held >= 10000) clearBond();
      else if (held >= 3000) openPairing();
      else if (coastcue::shouldMuteBuzzerLocally(
                   storageReady_, activeSeverity_ != 0,
                   alertState_ == "open", held)) {
        silenced_ = true;
        digitalWrite(COASTCUE_PIN_BUZZER, LOW);
        Serial.println(
            "[SHORE] local buzzer muted; Incident remains open (FS fault)");
        notifyStatus();
      } else if (held < 1000) {
        if (singlePendingAt_ != 0 && now - singlePendingAt_ <= DOUBLE_CLICK_MS) {
          singlePendingAt_ = 0;
          resolve(activeRecordId_, "", "button");
        } else singlePendingAt_ = now;
      }
    }
  }
  if (singlePendingAt_ != 0 && now - singlePendingAt_ > DOUBLE_CLICK_MS) {
    singlePendingAt_ = 0;
    acknowledge(activeRecordId_, "", "button");
  }
}

void ShoreRuntime::serviceBuzzer() {
  const uint32_t now = millis();
  if (!coastcue::shouldSoundAttentionBuzzer(alertState_.c_str(), activeSeverity_,
                                            silenced_)) {
    digitalWrite(COASTCUE_PIN_BUZZER, LOW);
    return;
  }
  if (static_cast<int32_t>(now - nextBuzzAt_) < 0) return;
  const uint32_t phase = now % (activeSeverity_ == 2 ? 800 : 1800);
  const bool on = activeSeverity_ == 2 ? (phase < 120 || (phase > 240 && phase < 360))
                                      : phase < 160;
  digitalWrite(COASTCUE_PIN_BUZZER, on ? HIGH : LOW);
  nextBuzzAt_ = now + 40;
}

void ShoreRuntime::onBleLinkConnected(uint16_t connectionHandle) {
  ++connectionGeneration_;
  connectedHandle_ = connectionHandle;
  bleConnected_ = false;
  linkConnectedAt_ = millis();
  clearPendingSync();
  notifyFailCount_ = 0;
  if (commandQueue_ != nullptr) xQueueReset(commandQueue_);
  if (operatorCommandQueue_ != nullptr) xQueueReset(operatorCommandQueue_);
  if (busyCommandQueue_ != nullptr) xQueueReset(busyCommandQueue_);
  if (queueMutex_ != nullptr &&
      xSemaphoreTake(queueMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    outgoingMessages_.clear();
    activeMessage_ = "";
    activeMessageGeneration_ = 0;
    activeFragmentIndex_ = 0;
    activeFragmentCount_ = 0;
    xSemaphoreGive(queueMutex_);
  }
}

void ShoreRuntime::onBleAuthenticated() {
  if (bleConnected_) return;
  bleConnected_ = true;
  linkConnectedAt_ = 0;
  pairingUntil_ = 0;
  bleUnexpectedlyOffline_ = false;
  bleDisconnectedNoticeUntil_ = 0;
  bleConnectedNoticeUntil_ = millis() + BLE_CONNECTED_NOTICE_MS;
}

void ShoreRuntime::onBleDisconnected(int reason) {
  const bool wasAuthenticated = bleConnected_;
  const bool peerRequestedDisconnect =
      reason == BLE_ERR_REM_USER_CONN_TERM ||
      reason == BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM);
  ++connectionGeneration_;
  bleConnected_ = false;
  bleConnectedNoticeUntil_ = 0;
  clearPendingSync();
  notifyFailCount_ = 0;
  if (wasAuthenticated) {
    bleUnexpectedlyOffline_ = !peerRequestedDisconnect;
    bleDisconnectedNoticeUntil_ =
        peerRequestedDisconnect ? millis() + BLE_DISCONNECTED_NOTICE_MS : 0;
  }
  connectedHandle_ = 0xFFFF;
  linkConnectedAt_ = 0;
  if (commandQueue_ != nullptr) xQueueReset(commandQueue_);
  if (operatorCommandQueue_ != nullptr) xQueueReset(operatorCommandQueue_);
  if (busyCommandQueue_ != nullptr) xQueueReset(busyCommandQueue_);
  if (queueMutex_ != nullptr &&
      xSemaphoreTake(queueMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    outgoingMessages_.clear();
    activeMessage_ = "";
    activeMessageGeneration_ = 0;
    activeFragmentIndex_ = 0;
    activeFragmentCount_ = 0;
    xSemaphoreGive(queueMutex_);
  }
  Serial.printf("[SHORE] BLE disconnected reason=%d state=%s\n", reason,
                peerRequestedDisconnect ? "user" : "offline");
  restartAdvertisingIfAllowed();
}

void ShoreRuntime::restartAdvertisingIfAllowed() {
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  if (connectedHandle_ != 0xFFFF) return;
  if (pairingOpen() || NimBLEDevice::getNumBonds() > 0) {
    if (!advertising->isAdvertising()) advertising->start();
  } else if (advertising->isAdvertising()) {
    advertising->stop();
  }
}

void ShoreRuntime::openPairing() {
  if (journalEpochProvisional_) {
    Serial.println(
        "[SHORE] pairing unavailable until the journal identity recovers");
    return;
  }
  if (NimBLEDevice::getNumBonds() > 0) {
    Serial.println("[SHORE] iPhone bond already exists; hold 10 seconds to reset it");
    restartAdvertisingIfAllowed();
    return;
  }
  bleUnexpectedlyOffline_ = false;
  bleDisconnectedNoticeUntil_ = 0;
  pairingUntil_ = millis() + PAIRING_MS;
  restartAdvertisingIfAllowed();
  Serial.println("[SHORE] BLE pairing open for 120 seconds; read passkey on display");
  notifyStatus();
}

void ShoreRuntime::clearBond() {
  const uint16_t handle = connectedHandle_;
  bleConnected_ = false;
  bleConnectedNoticeUntil_ = 0;
  bleDisconnectedNoticeUntil_ = 0;
  bleUnexpectedlyOffline_ = false;
  connectedHandle_ = 0xFFFF;
  linkConnectedAt_ = 0;
  if (handle != 0xFFFF) NimBLEDevice::getServer()->disconnect(handle);
  NimBLEDevice::deleteAllBonds();
  Serial.println("[SHORE] BLE bonds cleared by physical hold");
  openPairing();
}

void ShoreRuntime::drawScreen() {
  if (!displayReady || millis() - lastDisplayAt_ < 250) return;
  lastDisplayAt_ = millis();
  const bool hasActiveIncident =
      activeSeverity_ != 0 && alertState_ != "resolved";
  const bool showBleConnectedNotice =
      bleConnected_ && bleConnectedNoticeUntil_ != 0 &&
      static_cast<int32_t>(bleConnectedNoticeUntil_ - millis()) > 0;
  const bool showBleDisconnectedNotice =
      !bleConnected_ && bleDisconnectedNoticeUntil_ != 0 &&
      static_cast<int32_t>(bleDisconnectedNoticeUntil_ - millis()) > 0;
  const bool showBleOffline = !bleConnected_ && bleUnexpectedlyOffline_ &&
                              NimBLEDevice::getNumBonds() > 0;
  const char *severityLabel = activeSeverity_ == 2 ? "SOS" : "HELP";
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("CoastCue shore");
  if (!storageReady_) display.print(" FS!");
  display.setCursor(0, 12);
  if (!hasActiveIncident) {
    display.print("Ready");
  } else {
    display.print(severityLabel);
    if (alertState_ == "acknowledged") {
      display.print(" acknowledged");
    } else if (activeIncidentDurable_) {
      display.print(" received");
    } else {
      display.print(" local only");
    }
  }
  display.setCursor(0, 24);
  if (pairingOpen()) {
    display.print("Confirm on iPhone");
  } else if (hasActiveIncident && alertState_ == "acknowledged") {
    display.print(coastcue::operatorIncidentPrompt(true, bleConnected_));
  } else if (hasActiveIncident && silenced_) {
    display.print("Buzzer muted");
  } else if (hasActiveIncident) {
    display.print(coastcue::operatorIncidentPrompt(false, bleConnected_));
  } else if (showBleConnectedNotice) {
    display.print("iPhone connected");
  } else if (showBleDisconnectedNotice) {
    display.print("iPhone disconnected");
  } else if (showBleOffline) {
    display.print("iPhone offline");
  } else {
    display.print("Radio listening");
  }
  // Pairing is a deliberate physical action and the six-digit passkey must be
  // visible even while an Incident remains active. The human-readable alert
  // state stays above while these two rows temporarily show pairing.
  if (pairingOpen()) {
    display.setCursor(0, 40);
    display.printf("PAIR %06lu", static_cast<unsigned long>(passkey_));
    display.setCursor(0, 52);
    display.printf("%lus", static_cast<unsigned long>((pairingUntil_ - millis()) / 1000));
  } else if (hasActiveIncident && hasLastLocation_) {
    display.setCursor(0, 40);
    display.printf("%.5f,%.5f", static_cast<double>(lastLatitude_),
                   static_cast<double>(lastLongitude_));
    display.setCursor(0, 52);
    if (hasLastGpsAge_) {
      display.printf("GPS age %lus",
                     static_cast<unsigned long>(lastGpsAgeSeconds_));
    } else {
      display.print("GPS age unknown");
    }
  } else if (hasActiveIncident) {
    display.setCursor(0, 40);
    display.print("GPS unavailable");
    display.setCursor(0, 52);
    display.print("no last position");
  } else if (NimBLEDevice::getNumBonds() == 0) {
    display.setCursor(0, 40);
    display.print("BLE hold 3s pair");
    display.setCursor(0, 52);
    display.print("hold 10s clear");
  }
  display.display();
}

void ShoreRuntime::compactJournalIfNeeded() {
  const bool replayIndexIncomplete = !journalNonceIndex_.complete() ||
                                     !journalIncidentIndex_.complete();
  if (!storageReady_) return;
  if (!JOURNAL_WINDOW.shouldCompact(journalBytes_) && !replayIndexIncomplete) return;
  const size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (!JOURNAL_WINDOW.canStageReplacement(freeBytes, 4096)) {
    Serial.println("[SHORE] journal compaction deferred: insufficient temporary space");
    return;
  }
  File source = LittleFS.open(JOURNAL_PATH, FILE_READ);
  if (!source) return;
  uint32_t sourceBytes = 0;
  while (source.available()) {
    String line = source.readStringUntil('\n');
    sourceBytes += line.length() + 1;
  }
  const auto retention = JOURNAL_WINDOW.plan(sourceBytes);
  if (!retention.needsCompaction && !replayIndexIncomplete) {
    source.close();
    journalBytes_ = sourceBytes;
    return;
  }
  source.seek(0);
  // Retain a contiguous tail below low-water. The active event/transition
  // lineage is included even when it predates the tail.
  uint32_t skipBytes = retention.bytesToDiscard;
  uint32_t keepFromId = 0;
  while (source.available() && skipBytes > 0) {
    String line = source.readStringUntil('\n');
    const uint32_t lineBytes = line.length() + 1;
    trimTrailingCr(line);
    JsonDocument record;
    const bool parsed = deserializeJson(record, line) == DeserializationError::Ok;
    const uint32_t id = parsed ? (record["record_id"] | 0) : 0;
    if (skipBytes > lineBytes) {
      skipBytes -= lineBytes;
    } else {
      skipBytes = 0;
    }
    keepFromId = id + 1;
  }
  source.seek(0);
  LittleFS.remove(JOURNAL_TEMP_PATH);
  File target = LittleFS.open(JOURNAL_TEMP_PATH, FILE_WRITE);
  if (!target) {
    source.close();
    return;
  }
  uint32_t retained = 0;
  uint32_t retainedBytes = 0;
  uint32_t retainedFirst = 0;
  bool writeFailed = false;
  while (source.available()) {
    String line = source.readStringUntil('\n');
    trimTrailingCr(line);
    JsonDocument event;
    const bool parsed = deserializeJson(event, line) == DeserializationError::Ok;
    const uint32_t id = parsed ? (event["record_id"] | 0) : 0;
    const bool activeLine = id == activeRecordId_ ||
                            (event["target_record_id"] | 0) == activeRecordId_;
    if (id != 0 && id < keepFromId && !activeLine) continue;
    size_t written = 0;
    if (!writeJournalLine(target, line, written)) {
      writeFailed = true;
      break;
    }
    ++retained;
    retainedBytes += written;
    if (retainedFirst == 0 && id != 0) retainedFirst = id;
  }
  target.flush();
  source.close();
  target.close();
  if (writeFailed) {
    LittleFS.remove(JOURNAL_TEMP_PATH);
    return;
  }
  LittleFS.remove(JOURNAL_BACKUP_PATH);
  if (!LittleFS.rename(JOURNAL_PATH, JOURNAL_BACKUP_PATH)) {
    LittleFS.remove(JOURNAL_TEMP_PATH);
    return;
  }
  if (!LittleFS.rename(JOURNAL_TEMP_PATH, JOURNAL_PATH)) {
    LittleFS.rename(JOURNAL_BACKUP_PATH, JOURNAL_PATH);
    return;
  }
  LittleFS.remove(JOURNAL_BACKUP_PATH);
  if (!rebuildJournalReplayIndexes()) {
    // The compacted journal remains the durable source of truth, but replay
    // admission cannot safely use a partial index.  Expose this as a storage
    // outage and let the normal recovery loop remount/replay/rebuild instead
    // of looking healthy while silently rejecting every radio uplink.
    storageReady_ = false;
    Serial.println(
        "[SHORE] compacted journal replay index rebuild failed; storage "
        "recovery required and radio admission paused");
  }
  journalCount_ = retained;
  journalBytes_ = retainedBytes;
  firstStoredRecordId_ = retainedFirst;
  truncatedBeforeRecordId_ = keepFromId;
  syncHintAfter_ = 0;
  syncHintOffset_ = 0;
}

uint64_t ShoreRuntime::currentUnixMs() const {
  if (unixBaseMs_ == 0) return 0;
  return unixBaseMs_ + (esp_timer_get_time() / 1000ULL - unixBaseUptimeMs_);
}

bool ShoreRuntime::pairingOpen() const {
  return pairingUntil_ != 0 && static_cast<int32_t>(pairingUntil_ - millis()) > 0;
}

void ShoreRuntime::service() {
  if (!storageReady_) tryRecoverStorage();
  serviceButton();
  serviceBuzzer();
  drawScreen();
  processNextCommand();
  serviceSyncScan();
  serviceNotifications();
  serviceSyncScan();
  serviceNotifications();
  if (connectedHandle_ != 0xFFFF && !bleConnected_ &&
      millis() - linkConnectedAt_ >= BLE_AUTH_TIMEOUT_MS) {
    const uint16_t handle = connectedHandle_;
    // NimBLE callbacks run outside this loop. Re-check immediately before the
    // destructive action so an authentication callback racing this service
    // iteration cannot be followed by an obsolete local disconnect.
    if (!bleConnected_ && handle == connectedHandle_) {
      linkConnectedAt_ = millis();
      Serial.printf(
          "[SHORE] local disconnect cause=authentication_timeout handle=%u\n",
          static_cast<unsigned>(handle));
      NimBLEDevice::getServer()->disconnect(handle);
    }
  }
  if (pairingUntil_ != 0 && static_cast<int32_t>(pairingUntil_ - millis()) <= 0) {
    pairingUntil_ = 0;
    restartAdvertisingIfAllowed();
    notifyStatus();
  }
  if (millis() - lastStatusAt_ >= STATUS_INTERVAL_MS) {
    lastStatusAt_ = millis();
    notifyStatus();
  }
}

bool ShoreRuntime::checkpointEmergency(const String &encoded) {
  if (encoded.isEmpty() || encoded.length() > MAX_EMERGENCY_CHECKPOINT_BYTES) {
    Serial.println("[SHORE] emergency checkpoint skipped: payload too large");
    return false;
  }
  JsonDocument incoming;
  if (deserializeJson(incoming, encoded) != DeserializationError::Ok) return false;
  const String nonce = incoming["nonce"] | "";
  if (nonce.length() != 16) return false;
  Preferences preferences;
  if (!preferences.begin("coastcue", false)) {
    Serial.println("[SHORE] emergency checkpoint unavailable: NVS open failed");
    return false;
  }
  String freeKey;
  for (uint8_t slot = 0; slot < EMERGENCY_CHECKPOINT_SLOTS; ++slot) {
    const String key = emergencyCheckpointKey(slot);
    const String pending = preferences.getString(key.c_str(), "");
    if (pending.isEmpty() && freeKey.isEmpty()) {
      freeKey = key;
      continue;
    }
    JsonDocument stored;
    if (deserializeJson(stored, pending) == DeserializationError::Ok &&
        (String(stored["nonce"] | "") == nonce ||
         (!String(incoming["incident_id"] | "").isEmpty() &&
          String(stored["incident_id"] | "") ==
              String(incoming["incident_id"] | "")))) {
      preferences.end();
      return true;
    }
  }
  if (freeKey.isEmpty()) {
    preferences.end();
    Serial.println("[SHORE] emergency checkpoint queue full; live alert retained");
    return false;
  }
  const size_t written = preferences.putString(freeKey.c_str(), encoded);
  preferences.end();
  if (written == 0) {
    Serial.println("[SHORE] emergency checkpoint write failed");
    return false;
  }
  return true;
}

bool ShoreRuntime::clearEmergencyCheckpointIfMatches(const String &nonce) {
  Preferences preferences;
  if (!preferences.begin("coastcue", false)) return false;
  bool removed = true;
  bool matched = false;
  for (int slot = -1; slot < EMERGENCY_CHECKPOINT_SLOTS; ++slot) {
    const String key = slot < 0 ? String(PENDING_EMERGENCY_NVS_KEY)
                                : emergencyCheckpointKey(slot);
    const String pending = preferences.getString(key.c_str(), "");
    if (pending.isEmpty()) continue;
    JsonDocument checkpoint;
    if (deserializeJson(checkpoint, pending) != DeserializationError::Ok) {
      removed = preferences.remove(key.c_str()) && removed;
      Serial.println("[SHORE] invalid emergency checkpoint discarded");
      continue;
    }
    if (String(checkpoint["nonce"] | "") == nonce) {
      matched = true;
      removed = preferences.remove(key.c_str()) && removed;
    }
  }
  preferences.end();
  if (matched && !removed) Serial.println("[SHORE] emergency checkpoint clear failed");
  return !matched || removed;
}

bool ShoreRuntime::journalContainsExactOrUnreadable(const char *field,
                                                    const String &value) {
  File journal = LittleFS.open(JOURNAL_PATH, FILE_READ);
  if (!journal) return coastcue::shouldRejectExactReplayLookup(false, false);
  const size_t expectedBytes = journal.size();
  if (expectedBytes != journalBytes_) {
    journal.close();
    return coastcue::shouldRejectExactReplayLookup(false, false);
  }
  size_t scannedBytes = 0;
  bool valid = true;
  while (journal.available()) {
    String line;
    size_t lineBytes = 0;
    bool newlineTerminated = false;
    if (!readJournalLine(journal, line, lineBytes, newlineTerminated) ||
        !newlineTerminated) {
      valid = false;
      break;
    }
    scannedBytes += lineBytes;
    trimTrailingCr(line);
    JsonDocument record;
    if (deserializeJson(record, line) != DeserializationError::Ok ||
        !record.is<JsonObject>()) {
      valid = false;
      break;
    }
    if (record[field].is<const char *>() &&
        value == record[field].as<const char *>()) {
      journal.close();
      return coastcue::shouldRejectExactReplayLookup(true, valid);
    }
  }
  if (scannedBytes != expectedBytes) valid = false;
  journal.close();
  // A failed confirmation cannot safely turn a filter positive into admission.
  return coastcue::shouldRejectExactReplayLookup(false, valid);
}

bool ShoreRuntime::journalContainsNonce(const String &nonce) {
  if (nonce.length() != 16) return false;
  const auto decision = coastcue::decideReplayAdmission(
      journalNonceIndex_.complete(),
      journalNonceIndex_.contains(replayIndexHash(nonce)));
  if (decision == coastcue::ReplayAdmissionDecision::kReject) return true;
  return decision == coastcue::ReplayAdmissionDecision::kConfirmExact &&
         journalContainsExactOrUnreadable("nonce", nonce);
}

bool ShoreRuntime::journalContainsIncident(const String &incidentId) {
  if (incidentId.length() != 16) return false;
  const auto decision = coastcue::decideReplayAdmission(
      journalIncidentIndex_.complete(),
      journalIncidentIndex_.contains(replayIndexHash(incidentId)));
  if (decision == coastcue::ReplayAdmissionDecision::kReject) return true;
  return decision == coastcue::ReplayAdmissionDecision::kConfirmExact &&
         journalContainsExactOrUnreadable("incident_id", incidentId);
}

void ShoreRuntime::captureLiveOnlyIncidentRecovery() {
  if (activeIncidentDurable_ || activeRecordId_ == 0 ||
      activeIncidentId_.isEmpty() || activeIncidentNonce_.isEmpty() ||
      activeLiveEmergency_.isEmpty()) {
    return;
  }
  liveOnlyRecovery_.pending = true;
  liveOnlyRecovery_.alerts = alerts_;
  liveOnlyRecovery_.recordId = activeRecordId_;
  liveOnlyRecovery_.severity = activeSeverity_;
  liveOnlyRecovery_.alertState = alertState_;
  liveOnlyRecovery_.silenced = silenced_;
  liveOnlyRecovery_.incidentId = activeIncidentId_;
  liveOnlyRecovery_.nonce = activeIncidentNonce_;
  liveOnlyRecovery_.boatNodeUid = activeBoatNodeUid_;
  liveOnlyRecovery_.stateSeq = activeIncidentStateSeq_;
  liveOnlyRecovery_.encodedEvent = activeLiveEmergency_;
}

void ShoreRuntime::restoreLiveOnlyIncidentRecovery() {
  if (!liveOnlyRecovery_.pending) return;
  alerts_ = liveOnlyRecovery_.alerts;
  activeRecordId_ = liveOnlyRecovery_.recordId;
  activeSeverity_ = liveOnlyRecovery_.severity;
  alertState_ = liveOnlyRecovery_.alertState;
  silenced_ = liveOnlyRecovery_.silenced;
  activeIncidentId_ = liveOnlyRecovery_.incidentId;
  activeIncidentNonce_ = liveOnlyRecovery_.nonce;
  activeBoatNodeUid_ = liveOnlyRecovery_.boatNodeUid;
  activeIncidentStateSeq_ = liveOnlyRecovery_.stateSeq;
  activeIncidentDurable_ = false;
  activeLiveEmergency_ = liveOnlyRecovery_.encodedEvent;
  pendingRadioDownlinks_.clear();
}

bool ShoreRuntime::tryRecoverStorage() {
#if COASTCUE_BENCH_SHORE_FAIL_FIRST_OPERATOR_TRANSITION
  if (benchStorageRecoverySuppressed) return false;
#endif
  if (storageReady_) return true;
  const uint32_t now = millis();
  if (nextStorageRecoveryAt_ != 0 &&
      static_cast<int32_t>(now - nextStorageRecoveryAt_) < 0) {
    return false;
  }
  nextStorageRecoveryAt_ = now + STORAGE_RECOVERY_RETRY_MS;
  if (!LittleFS.begin(false)) return false;
  captureLiveOnlyIncidentRecovery();
  if (!initializeMountedStorage()) {
    storageReady_ = false;
    restoreLiveOnlyIncidentRecovery();
    Serial.println("[SHORE] LittleFS remounted but storage initialization failed");
    return false;
  }
  Preferences preferences;
  if (preferences.begin("coastcue", false)) {
    preferences.putBool("fs_init", true);
    preferences.end();
  }
  Serial.println("[SHORE] LittleFS storage recovered; pending emergencies will promote when durable");
  // Journal replay is authoritative. NVS emergency checkpoints are then
  // promoted in arrival order, exactly as on a successful initial mount.
  recoverEmergencyCheckpoint();
  // A Preferences failure may have left the exact checkpoint only in RAM.
  // It remains sufficient for an in-boot promotion after LittleFS remounts.
  if (liveOnlyRecovery_.pending) {
    recoverEmergencyEvent(liveOnlyRecovery_.encodedEvent);
  }
  notifyStatus();
  return true;
}

void ShoreRuntime::recoverEmergencyCheckpoint() {
  Preferences preferences;
  if (!preferences.begin("coastcue", false)) return;
  String pending;
  for (int slot = -1; slot < EMERGENCY_CHECKPOINT_SLOTS; ++slot) {
    const String key = slot < 0 ? String(PENDING_EMERGENCY_NVS_KEY)
                                : emergencyCheckpointKey(slot);
    const String candidate = preferences.getString(key.c_str(), "");
    if (candidate.isEmpty()) continue;
    JsonDocument checkpoint;
    const String candidateNonce =
        deserializeJson(checkpoint, candidate) == DeserializationError::Ok
            ? String(checkpoint["nonce"] | "") : String();
    bool alreadyRestoredLive = false;
    for (size_t index = 0; index < liveRecoveredCheckpointCount_; ++index) {
      if (liveRecoveredCheckpointNonces_[index] == candidateNonce) {
        alreadyRestoredLive = true;
        break;
      }
    }
    if (!alreadyRestoredLive) {
      pending = candidate;
      break;
    }
  }
  preferences.end();
  if (pending.isEmpty()) return;
  recoverEmergencyEvent(pending);
}

void ShoreRuntime::recoverEmergencyEvent(const String &pending) {
  JsonDocument event;
  if (deserializeJson(event, pending) != DeserializationError::Ok) {
    clearEmergencyCheckpointIfMatches("");
    recoverEmergencyCheckpoint();
    return;
  }
  const String nonce = event["nonce"] | "";
  const String status = event["status"] | "";
  const String incidentId = event["incident_id"] | "";
  const String packetNodeUid = event["node_uid"] | "";
  if (nonce.length() != 16 || (status != "help" && status != "sos")) {
    clearEmergencyCheckpointIfMatches(nonce);
    recoverEmergencyCheckpoint();
    return;
  }
  if (storageReady_ && journalContainsNonce(nonce)) {
    if (liveOnlyRecovery_.pending &&
        liveOnlyRecovery_.incidentId == incidentId &&
        liveOnlyRecovery_.nonce == nonce) {
      liveOnlyRecovery_.pending = false;
    }
    clearEmergencyCheckpointIfMatches(nonce);
    recoverEmergencyCheckpoint();
    return;
  }
  if (storageReady_ && !incidentId.isEmpty() &&
      journalContainsIncident(incidentId)) {
    clearEmergencyCheckpointIfMatches(nonce);
    recoverEmergencyCheckpoint();
    return;
  }

  // Apply location only after this checkpoint has survived validation and
  // duplicate detection. A stale checkpoint left behind after a failed NVS
  // clear must not replace a newer location rebuilt from the journal.
  if (event["latitude"].is<float>() && event["longitude"].is<float>()) {
    const float latitude = event["latitude"] | 0.0f;
    const float longitude = event["longitude"] | 0.0f;
    if (latitude >= -90.0f && latitude <= 90.0f &&
        longitude >= -180.0f && longitude <= 180.0f) {
      hasLastLocation_ = true;
      lastLatitude_ = latitude;
      lastLongitude_ = longitude;
      hasLastGpsAge_ = event["gps_age_s"].is<uint32_t>();
      if (hasLastGpsAge_) {
        lastGpsAgeSeconds_ = event["gps_age_s"].as<uint32_t>();
      }
    }
  }

  const bool incidentMatches =
      liveOnlyRecovery_.pending &&
      liveOnlyRecovery_.incidentId == incidentId;
  const bool nonceMatches =
      liveOnlyRecovery_.pending && liveOnlyRecovery_.nonce == nonce;
  const auto recovery = coastcue::decideEmergencyCheckpointRecovery(
      storageReady_, liveOnlyRecovery_.pending, incidentMatches, nonceMatches);
  const auto severity = status == "sos" ? coastcue::AlertReducer::kSos
                                          : coastcue::AlertReducer::kHelp;
  if (storageReady_ && nextRecordId_ < 0x80000000UL) {
    const uint32_t recordId = nextRecordId_;
    event["record_id"] = recordId;
    event["durable"] = true;
    event.remove("checkpoint");
    event["recovered_from_checkpoint"] = true;
    event.remove("supersedes_record_id");
    event.remove("source");
    const auto disposition = coastcue::decideRecoveredEmergency(
        alerts_, recordId, severity);
    if (disposition.kind == coastcue::AlertReducer::kSuperseded) {
      event["alert_state"] = "open";
      event["supersedes_record_id"] = disposition.previousRecordId;
    } else if (disposition.kind == coastcue::AlertReducer::kAbsorbed) {
      event["alert_state"] = "resolved";
      event["source"] = "absorbed";
    } else {
      event["alert_state"] = "open";
    }
    if (recovery ==
        coastcue::EmergencyCheckpointRecovery::kPromoteLiveOnly) {
      event["promotes_record_id"] = liveOnlyRecovery_.recordId;
    }
    const String encoded = jsonString(event);
    if (appendJournal(encoded)) {
      journalNonceIndex_.add(replayIndexHash(nonce));
      if (incidentId.length() == 16) {
        journalIncidentIndex_.add(replayIndexHash(incidentId));
      }
      if (firstStoredRecordId_ == 0) firstStoredRecordId_ = recordId;
      ++nextRecordId_;
      Preferences checkpoint;
      if (checkpoint.begin("coastcue", false)) {
        checkpoint.putUInt("next_id", nextRecordId_);
        checkpoint.end();
      }
      applyEmergency(recordId, status, true);
      if (!incidentId.isEmpty() && !packetNodeUid.isEmpty() &&
          activeRecordId_ == recordId) {
        bindActiveIncidentIdentity(
            incidentId, packetNodeUid, event["radio_state_seq"] | 1U, true);
        activeIncidentNonce_ = nonce;
      }
      if (recovery ==
          coastcue::EmergencyCheckpointRecovery::kPromoteLiveOnly) {
        liveOnlyRecovery_.pending = false;
        // The iPhone may hold the live-only event under this nonce. Publish
        // the exact journaled replacement before the recovery status update.
        notifyEvent(encoded);
      }
      clearEmergencyCheckpointIfMatches(nonce);
      compactJournalIfNeeded();
      Serial.println("[SHORE] recovered emergency checkpoint into journal");
      recoverEmergencyCheckpoint();
      return;
    }
    if (recovery ==
        coastcue::EmergencyCheckpointRecovery::kPromoteLiveOnly) {
      restoreLiveOnlyIncidentRecovery();
      Serial.println(
          "[SHORE] live-only promotion append failed; lifecycle retained");
      return;
    }
  }

  // Keep the NVS checkpoint for later persistence, but restore the local
  // safety alert immediately when LittleFS is unavailable.
  const uint32_t recordId = allocateVolatileRecordId();
  applyEmergency(recordId, status, false);
  if (coastcue::shouldBindIncidentIdentity(
          true, !incidentId.isEmpty(), !packetNodeUid.isEmpty(),
          activeRecordId_ == recordId)) {
    bindActiveIncidentIdentity(
        incidentId, packetNodeUid, event["radio_state_seq"] | 1U, false);
    activeIncidentNonce_ = nonce;
    activeLiveEmergency_ = pending;
  }
  Serial.println("[SHORE] recovered emergency checkpoint in live-only mode");
  if (liveRecoveredCheckpointCount_ < EMERGENCY_CHECKPOINT_SLOTS) {
    liveRecoveredCheckpointNonces_[liveRecoveredCheckpointCount_++] = nonce;
    recoverEmergencyCheckpoint();
  }
}

#endif  // COASTCUE_ROLE_SHORE
