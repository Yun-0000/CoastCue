#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <deque>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "shore_logic.h"

struct ShoreRadioDownlink {
  ShoreRadioDownlink() = default;
  ShoreRadioDownlink(const String &target, const String &incident,
                     const String &incidentState, uint32_t sequence)
      : targetNodeUid(target), incidentId(incident), state(incidentState),
        stateSeq(sequence) {}

  String targetNodeUid;
  String incidentId;
  String state;
  uint32_t stateSeq = 0;
};

// The shore runtime starts only after main.cpp has authenticated a LoRa frame.
// It owns durable incident state but leaves RadioLib operations in main.cpp.
class ShoreRuntime {
 public:
  void begin(const String &nodeUid, const String &configuredBoatId);
  bool acceptNonce(const String &wirePacket);
  // Returns true when this uplink creates a deterministic response slot.
  bool ingestAuthenticatedPacket(const String &wirePacket, float rssi, float snr);
  bool takePendingRadioDownlink(ShoreRadioDownlink &downlink) const;
  void service();
  uint32_t pairingPasskey() const { return passkey_; }
  bool pairingOpen() const;
  bool bleAuthenticated() const { return bleConnected_; }

  // NimBLE callbacks only enqueue work or update link state. Filesystem and
  // alert mutations remain on Arduino's main loop.
  void handleCommand(const String &command);
  void onBleLinkConnected(uint16_t connectionHandle);
  void onBleAuthenticated();
  void onBleDisconnected(int reason);

 private:
  void appendRadioEvent(const String &wirePacket, const String &nonce,
                        const String &status, const String &incidentId,
                        float rssi, float snr);
  bool appendTransition(uint32_t targetRecordId, const char *state,
                        const char *source);
  void queueIncidentDownlink(const String &state, uint32_t stateSeq);
  void bindActiveIncidentIdentity(const String &incidentId,
                                  const String &boatNodeUid,
                                  uint32_t stateSeq,
                                  bool receiptAvailable);
  void clearActiveIncidentIdentity();
  void confirmIncidentDownlink(const String &incidentId, uint32_t stateSeq);
  void persistConfirmedIncidentState();
  void applyEmergency(uint32_t recordId, const String &status,
                      bool eventPersisted);
  void applyOperatorAction(const char *op, const char *state, uint32_t recordId,
                           const String &commandId, const char *source);
  void commitDurableRecord(uint32_t recordId, const String &nonce,
                           const String &incidentId);
  void rememberLocation(JsonVariantConst latitude, JsonVariantConst longitude,
                        JsonVariantConst ageSeconds);
  void executeCommand(const String &command);
  void processNextCommand();
  struct CommandRequest {
    char value[192];
  };
  void notifyBusyCommand(const CommandRequest &command);
  bool beginSync(uint32_t after, uint8_t limit, const String &syncId,
                 uint32_t connectionGeneration);
  void serviceSyncScan();
  void finishSyncPage();
  void failSync(const char *error);
  void replaceSyncWithFailureLocked(const String &syncId, uint32_t generation,
                                    const char *error);
  void resetLinkQueues();
  void clearPendingSync();
  bool notifyEvent(const String &json);
  bool notifyOperatorResult(const String &json);
  bool enqueueOutgoing(const String &json, uint32_t connectionGeneration);
  size_t currentAttPayloadBytes() const;
  void abandonActiveNotify(const char *error);
  bool appendJournal(const String &encoded);
  bool checkpointEmergency(const String &encoded);
  bool clearEmergencyCheckpointIfMatches(const String &nonce);
  bool journalContainsExactOrUnreadable(const char *field,
                                        const String &value);
  bool journalContainsNonce(const String &nonce);
  bool journalContainsIncident(const String &incidentId);
  void captureLiveOnlyIncidentRecovery();
  void restoreLiveOnlyIncidentRecovery();
  void recoverEmergencyEvent(const String &pending);
  void recoverEmergencyCheckpoint();
  bool tryRecoverStorage();
  bool initializeMountedStorage();
  bool prepareJournalIdentity();
  bool persistJournalEpochMirror(const String &epoch);
  void clearProvisionalJournalIdentity();
  void refreshDeviceInfo(bool disconnectChangedIdentity = false);
  bool ensureFallbackJournalIdentity();
  void resetJournalDerivedState();
  void initializeVolatileRecordIds();
  uint32_t allocateVolatileRecordId();
  bool recoverJournalTail();
  void replayJournal();
  bool rebuildJournalReplayIndexes();
  void serviceNotifications();
  void notifyStatus();
  void drawScreen();
  void serviceButton();
  void serviceBuzzer();
  void openPairing();
  void clearBond();
  void restartAdvertisingIfAllowed();
  void compactJournalIfNeeded();
  uint64_t currentUnixMs() const;

  String nodeUid_;
  String configuredBoatId_;
  String lastNodeUid_;
  uint32_t nextRecordId_ = 1;
  uint32_t nextVolatileRecordId_ = 0x80000000UL;
  uint32_t journalCount_ = 0;
  uint32_t journalBytes_ = 0;
  uint32_t firstStoredRecordId_ = 0;
  uint32_t truncatedBeforeRecordId_ = 0;
  String journalEpoch_;
  bool journalEpochProvisional_ = false;
  uint32_t activeRecordId_ = 0;
  uint8_t activeSeverity_ = 0;
  String alertState_ = "resolved";
  bool silenced_ = false;
  String activeIncidentId_;
  String activeIncidentNonce_;
  String activeBoatNodeUid_;
  uint32_t activeIncidentStateSeq_ = 0;
  bool activeIncidentDurable_ = false;
  String activeLiveEmergency_;
  struct LiveOnlyIncidentRecovery {
    bool pending = false;
    coastcue::AlertReducer alerts;
    uint32_t recordId = 0;
    uint8_t severity = 0;
    String alertState;
    bool silenced = false;
    String incidentId;
    String nonce;
    String boatNodeUid;
    uint32_t stateSeq = 0;
    String encodedEvent;
  } liveOnlyRecovery_;
  String confirmedIncidentId_;
  uint32_t confirmedIncidentStateSeq_ = 0;
  std::deque<ShoreRadioDownlink> pendingRadioDownlinks_;
  uint64_t unixBaseMs_ = 0;
  uint64_t unixBaseUptimeMs_ = 0;
  uint32_t pairingUntil_ = 0;
  uint32_t bleConnectedNoticeUntil_ = 0;
  uint32_t bleDisconnectedNoticeUntil_ = 0;
  uint32_t passkey_ = 0;
  uint32_t lastDisplayAt_ = 0;
  uint32_t lastStatusAt_ = 0;
  uint32_t nextBuzzAt_ = 0;
  float lastRssi_ = 0;
  float lastSnr_ = 0;
  uint32_t lastPacketAt_ = 0;
  bool hasRadioMetrics_ = false;
  bool hasLastLocation_ = false;
  float lastLatitude_ = 0.0f;
  float lastLongitude_ = 0.0f;
  bool hasLastGpsAge_ = false;
  uint32_t lastGpsAgeSeconds_ = 0;
  bool storageReady_ = false;
  volatile bool bleConnected_ = false;
  bool bleUnexpectedlyOffline_ = false;
  volatile uint16_t connectedHandle_ = 0xFFFF;
  volatile uint32_t connectionGeneration_ = 0;
  volatile uint32_t linkConnectedAt_ = 0;
  bool buttonRaw_ = HIGH;
  bool buttonStable_ = HIGH;
  uint32_t buttonChangedAt_ = 0;
  uint32_t buttonPressedAt_ = 0;
  uint32_t singlePendingAt_ = 0;
  String recentNonces_[64];
  size_t nextNonce_ = 0;
  // Two 128-Kibit filters consume the same 32 KiB as the former 2,048-entry
  // exact indexes. Their seven bounded probes never turn journal row count
  // into a global admission failure; rare positives are exact-confirmed.
  static constexpr size_t kJournalReplayFilterBits = 128 * 1024;
  coastcue::ReplayFilter<kJournalReplayFilterBits> journalNonceIndex_;
  coastcue::ReplayFilter<kJournalReplayFilterBits> journalIncidentIndex_;
  uint32_t nextStorageRecoveryAt_ = 0;
  String liveRecoveredCheckpointNonces_[8];
  size_t liveRecoveredCheckpointCount_ = 0;

  SemaphoreHandle_t queueMutex_ = nullptr;
  QueueHandle_t commandQueue_ = nullptr;
  QueueHandle_t operatorCommandQueue_ = nullptr;
  QueueHandle_t busyCommandQueue_ = nullptr;
  struct OutgoingMessage {
    String json;
    uint32_t connectionGeneration;
  };
  std::deque<OutgoingMessage> outgoingMessages_;
  String activeMessage_;
  uint32_t activeMessageGeneration_ = 0;
  uint32_t nextMessageId_ = 1;
  uint32_t activeMessageId_ = 0;
  uint32_t activeMessageCrc_ = 0;
  uint16_t activeFragmentIndex_ = 0;
  uint16_t activeFragmentCount_ = 0;
  uint32_t nextNotifyAt_ = 0;
  uint8_t notifyFailCount_ = 0;
  size_t activeChunkBytes_ = coastcue::kBleEventChunkMaxBytes;
  size_t activeUnframedLimit_ = coastcue::kBleEventChunkMaxBytes;
  struct PendingSync {
    bool active = false;
    String syncId;
    uint32_t connectionGeneration = 0;
    size_t fileOffset = 0;
    coastcue::SyncPageScanner scanner;
  } pendingSync_;
  uint32_t syncHintAfter_ = 0;
  size_t syncHintOffset_ = 0;
  coastcue::AlertReducer alerts_;
};
