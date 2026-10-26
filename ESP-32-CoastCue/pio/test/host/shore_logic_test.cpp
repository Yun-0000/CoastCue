#include <assert.h>
#include <cstring>
#include <stdio.h>

#include "shore_logic.h"

int main() {
  using coastcue::AlertReducer;
  using coastcue::JournalRecoveryPolicy;
  using coastcue::JournalWindow;
  using coastcue::StatusSnapshot;
  using coastcue::BoatRadioUplink;

  constexpr const char* kEpochA = "0123456789abcdef01234567";
  constexpr const char* kEpochB = "89abcdef0123456789abcdef";
  using coastcue::JournalIdentityDecision;
  assert(coastcue::isValidJournalEpoch(kEpochA));
  assert(!coastcue::isValidJournalEpoch("01234567"));
  assert(!coastcue::isValidJournalEpoch("0123456789ABCDEF01234567"));
  assert(coastcue::decideJournalIdentity(kEpochA, kEpochA, true) ==
         JournalIdentityDecision::kUseMatching);
  assert(coastcue::decideJournalIdentity("", kEpochA, true) ==
         JournalIdentityDecision::kAdoptFilesystem);
  assert(coastcue::decideJournalIdentity(kEpochA, "", false) ==
         JournalIdentityDecision::kSeedEmptyFilesystem);
  assert(coastcue::decideJournalIdentity("", "", false) ==
         JournalIdentityDecision::kCreateNew);
  assert(coastcue::decideJournalIdentity(kEpochA, kEpochB, true) ==
         JournalIdentityDecision::kReject);
  assert(coastcue::decideJournalIdentity(kEpochA, kEpochB, true, true) ==
         JournalIdentityDecision::kAdoptFilesystem);
  assert(coastcue::decideJournalIdentity(kEpochA, "", true) ==
         JournalIdentityDecision::kReject);

  assert(!coastcue::isRadioFresh(false, 1000, 0, 900000));
  assert(coastcue::isRadioFresh(true, 1000, 500, 900000));
  assert(!coastcue::isRadioFresh(true, 900500, 500, 900000));
  assert(!coastcue::shouldCheckpointVolatileEmergency("normal", false));

  // A transition that starts with healthy storage must not become successful
  // merely because its failed append disabled storage during the operation.
  assert(!coastcue::mayApplyOperatorTransition(true, false));
  assert(coastcue::mayApplyOperatorTransition(true, true));
  assert(coastcue::mayApplyOperatorTransition(false, false));

  // A live-only emergency still binds its Incident ID for in-boot de-dup;
  // only the Receipt remains gated on a durable append.
  assert(coastcue::shouldBindIncidentIdentity(true, true, true, true));
  assert(!coastcue::shouldBindIncidentIdentity(false, true, true, true));
  using coastcue::EmergencyCheckpointRecovery;
  assert(coastcue::decideEmergencyCheckpointRecovery(
             false, true, true, true) ==
         EmergencyCheckpointRecovery::kRestoreLiveOnly);
  assert(coastcue::decideEmergencyCheckpointRecovery(
             true, true, true, true) ==
         EmergencyCheckpointRecovery::kPromoteLiveOnly);
  assert(coastcue::decideEmergencyCheckpointRecovery(
             true, false, false, false) ==
         EmergencyCheckpointRecovery::kAppendDurable);
  assert(coastcue::decideEmergencyCheckpointRecovery(
             true, true, true, false) ==
         EmergencyCheckpointRecovery::kAppendDurable);
  assert(coastcue::decideEmergencyCheckpointRecovery(
             true, true, false, true) ==
         EmergencyCheckpointRecovery::kAppendDurable);
  assert(coastcue::shouldCheckpointVolatileEmergency("help", false));
  assert(coastcue::shouldCheckpointVolatileEmergency("sos", false));
  assert(!coastcue::shouldCheckpointVolatileEmergency("sos", true));
  assert(!coastcue::isValidCommandId(nullptr));
  assert(!coastcue::isValidCommandId(""));
  assert(coastcue::isValidCommandId("request-42"));
  assert(!coastcue::isOperatorSafetyCommand("time"));

  const std::string validWireJson =
      "{\"nonce\":\"0123456789abcdef\",\"node_uid\":\"node\","
      "\"status\":\"normal\",\"gps\":{\"valid\":false},"
      "\"note\":\"a string mentioning nonce is not a key\",\"mac\":\"abcd\"}";
  assert(coastcue::hasUniqueRadioTopLevelKeys(validWireJson));
  assert(!coastcue::hasUniqueRadioTopLevelKeys(
      "{\"nonce\":\"0123456789abcdef\",\"nonce\":\"fedcba9876543210\"}"));
  assert(!coastcue::hasUniqueRadioTopLevelKeys(
      "{\"mac\":\"first\",\"mac\":\"second\"}"));
  assert(!coastcue::hasUniqueRadioTopLevelKeys(
      "{\"n\\u006fnce\":\"0123456789abcdef\",\"nonce\":\"fedcba9876543210\"}"));
  assert(coastcue::hasUniqueRadioTopLevelKeys(
      "{\"extension\":1,\"extension\":2,\"gps\":{\"nonce\":\"nested\"}}"));
  assert(!coastcue::hasUniqueRadioTopLevelKeys(
      "{\"gps\":{\"valid\":true,\"valid\":false}}"));
  assert(!coastcue::hasUniqueRadioTopLevelKeys(
      "{\"gps\":{\"valid\":true,\"age_s\":11,\"age_s\":12}}"));
  assert(coastcue::hasUniqueRadioTopLevelKeys(
      "{\"gps\":{\"extension\":1,\"extension\":2,\"valid\":false}}"));
  assert(!coastcue::hasUniqueRadioTopLevelKeys(
      "{\"kind\":\"incident_state\",\"incident_id\":\"a\","
      "\"incident_id\":\"b\"}"));
  assert(!coastcue::hasUniqueRadioTopLevelKeys(
      "{\"i\":\"0123456789abcdef\","
      "\"i\":\"fedcba9876543210\"}"));
  assert(!coastcue::hasUniqueRadioTopLevelKeys(
      "{\"kind\":\"incident_state\",\"state_seq\":1,"
      "\"state_seq\":2}"));

  // The filter has no row cap: 4096 entries stay complete.
  // Membership has no false negatives, so replayed nonces remain rejected.
  coastcue::ReplayFilter<128 * 1024> nonceIndex;
  for (uint64_t hash = 1; hash <= 4096; ++hash) {
    assert(nonceIndex.add(hash));
  }
  assert(nonceIndex.complete());
  for (uint64_t hash = 1; hash <= 4096; ++hash) {
    assert(nonceIndex.contains(hash));
  }
  nonceIndex.failClosed();
  assert(!nonceIndex.complete());
  assert(!nonceIndex.add(5000));
  nonceIndex.reset();
  assert(nonceIndex.complete());
  assert(!nonceIndex.contains(1));
  assert(nonceIndex.add(5000));

  using coastcue::ReplayAdmissionDecision;
  assert(coastcue::decideReplayAdmission(true, false) ==
         ReplayAdmissionDecision::kAccept);
  assert(coastcue::decideReplayAdmission(true, true) ==
         ReplayAdmissionDecision::kConfirmExact);
  assert(coastcue::decideReplayAdmission(false, false) ==
         ReplayAdmissionDecision::kReject);
  assert(coastcue::decideReplayAdmission(false, true) ==
         ReplayAdmissionDecision::kReject);
  assert(!coastcue::shouldRejectExactReplayLookup(false, true));
  assert(coastcue::shouldRejectExactReplayLookup(true, true));
  assert(coastcue::shouldRejectExactReplayLookup(false, false));
  assert(coastcue::shouldRejectExactReplayLookup(true, false));

  const coastcue::BusyCommandResult busyResult{
      "request-42", "ack", 77, true};
  assert(coastcue::encodeBusyCommandResult(busyResult) ==
         "{\"kind\":\"command_result\",\"command_id\":\"request-42\","
         "\"op\":\"ack\",\"record_id\":77,\"source\":\"ble\","
         "\"ok\":false,\"error\":\"busy\"}");

  AlertReducer alerts;
  assert(alerts.ingest(10, AlertReducer::kHelp).kind == AlertReducer::kOpened);
  const auto replacement = alerts.ingest(11, AlertReducer::kSos);
  assert(replacement.kind == AlertReducer::kSuperseded);
  assert(replacement.previousRecordId == 10);
  assert(alerts.activeRecordId() == 11);

  // Recovery never lets the pre-remount RAM snapshot override journal replay.
  // A Help captured while storage was down is absorbed by a durable SOS, while
  // an SOS still supersedes a replayed Help using the durable predecessor ID.
  AlertReducer replayedSos;
  replayedSos.ingest(40, AlertReducer::kSos);
  replayedSos.replayTransition(40, "acknowledged");
  const auto absorbedRecovery = coastcue::decideRecoveredEmergency(
      replayedSos, 41, AlertReducer::kHelp);
  assert(absorbedRecovery.kind == AlertReducer::kAbsorbed);
  assert(absorbedRecovery.previousRecordId == 41);
  assert(replayedSos.activeRecordId() == 40);
  assert(replayedSos.state()[0] == 'a');

  AlertReducer replayedHelp;
  replayedHelp.ingest(50, AlertReducer::kHelp);
  const auto supersedingRecovery = coastcue::decideRecoveredEmergency(
      replayedHelp, 51, AlertReducer::kSos);
  assert(supersedingRecovery.kind == AlertReducer::kSuperseded);
  assert(supersedingRecovery.previousRecordId == 50);
  assert(replayedHelp.activeRecordId() == 50);

  // A JSON adapter maps a missing or wrong-typed value to an unset field before
  // these pure current-protocol gates are called.
  BoatRadioUplink packet;
  packet.nonce = "0123456789abcdef";
  packet.nodeUid = "esp32-a1b2c3d4e5f6";
  packet.status = "normal";
  packet.gps.valid = false;
  assert(coastcue::validateTelemetryUplink(packet));
  assert(!coastcue::validateIncidentUplink(packet));

  packet.nodeUid = "";
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.nodeUid = "esp32-a1b2c3d4e5f6";

  packet.gps.valid.reset();
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.gps.valid = true;
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.gps.latitude = 22.543096;
  packet.gps.longitude = 114.057865;
  packet.gps.ageSeconds = 11;
  assert(coastcue::validateTelemetryUplink(packet));

  packet.gps.ageSeconds = -1;
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.gps.ageSeconds = 4294968;
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.gps.ageSeconds = 4294967;

  packet.status = "danger";
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.status.reset();  // A wrong-typed JSON status is represented as unset.
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.status = "normal";
  packet.nonce = "0123456789ABCDEF";
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.nonce = "0123456789abcdef";
  packet.gps.longitude = 180.001;
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.gps.longitude = 114.057865;
  packet.batteryPresent = true;
  packet.batteryPercent = 101;
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.batteryPercent = 100;
  assert(coastcue::validateTelemetryUplink(packet));

  // A listening boat's telemetry names its Incident.
  packet.incidentIdPresent = true;
  packet.incidentId = "0123456789abcdef";
  assert(coastcue::validateTelemetryUplink(packet));
  assert(!coastcue::validateIncidentUplink(packet));
  packet.incidentId = "0123456789ABCDEF";
  assert(!coastcue::validateTelemetryUplink(packet));
  packet.incidentId = "0123456789abcdef";
  packet.status = "help";
  assert(!coastcue::validateTelemetryUplink(packet));
  assert(coastcue::validateIncidentUplink(packet));
  packet.status = "sos";
  assert(coastcue::validateIncidentUplink(packet));
  packet.incidentId = "0123456789ABCDEF";
  assert(!coastcue::validateIncidentUplink(packet));
  packet.incidentId.reset();
  assert(!coastcue::validateIncidentUplink(packet));

  JournalRecoveryPolicy cleanJournal;
  cleanJournal.observeLine(21, true, true);
  cleanJournal.observeLine(34, true, true);
  assert(!cleanJournal.needsRecovery());
  assert(cleanJournal.retainedBytes() == 55);

  JournalRecoveryPolicy unterminatedTail;
  unterminatedTail.observeLine(21, true, true);
  unterminatedTail.observeLine(12, false, false);
  assert(unterminatedTail.needsRecovery());
  assert(unterminatedTail.retainedBytes() == 21);

  JournalRecoveryPolicy invalidTail;
  invalidTail.observeLine(21, true, true);
  invalidTail.observeLine(9, true, false);
  assert(invalidTail.needsRecovery());
  assert(invalidTail.retainedBytes() == 21);
  const auto absorbed = alerts.ingest(12, AlertReducer::kHelp);
  assert(absorbed.kind == AlertReducer::kAbsorbed);
  assert(absorbed.previousRecordId == 12);
  assert(alerts.activeRecordId() == 11);

  AlertReducer replayed;
  replayed.replayEvent(10, AlertReducer::kHelp);
  replayed.replayEvent(11, AlertReducer::kSos);
  replayed.replayTransition(10, "resolved");
  assert(replayed.activeRecordId() == 11);
  replayed.replayTransition(11, "acknowledged");
  assert(replayed.state()[0] == 'a');
  const auto reopened = replayed.ingest(12, AlertReducer::kHelp);
  assert(reopened.kind == AlertReducer::kAbsorbed);
  assert(replayed.state()[0] == 'a');
  replayed.replayEvent(13, AlertReducer::kHelp);
  assert(replayed.state()[0] == 'a');

  AlertReducer commandTarget;
  commandTarget.ingest(20, AlertReducer::kSos);
  assert(commandTarget.canApplyOperatorAction(20));
  commandTarget.replayTransition(20, "silenced");
  assert(commandTarget.state()[0] == 'o');
  commandTarget.replayTransition(20, "ack");
  assert(commandTarget.state()[0] == 'o');
  commandTarget.replayTransition(20, "resolved");
  assert(!commandTarget.canApplyOperatorAction(20));

  // BLE Ack/Resolve must preserve the volatile high-bit record namespace.
  assert(coastcue::normalizeBleCommandRecordId(1, true) == 1);
  assert(coastcue::normalizeBleCommandRecordId(0x7FFFFFFFULL, true) ==
         0x7FFFFFFFU);
  assert(coastcue::normalizeBleCommandRecordId(0x80000000ULL, true) ==
         0x80000000U);
  assert(coastcue::normalizeBleCommandRecordId(0xFFFFFFFFULL, true) ==
         0xFFFFFFFFU);
  assert(coastcue::normalizeBleCommandRecordId(0, true) == 0);
  assert(coastcue::normalizeBleCommandRecordId(0x100000000ULL, true) == 0);
  assert(coastcue::normalizeBleCommandRecordId(42, false) == 0);

  JournalWindow window(800, 500);
  assert(window.shouldCompact(800));
  assert(!window.canStageReplacement(4595, 4096));
  assert(window.canStageReplacement(4596, 4096));
  const auto plan = window.plan(900);
  assert(plan.bytesToDiscard == 400);
  assert(plan.needsCompaction);

  const StatusSnapshot status{
      true, 18, "esp32-a1b2c3d4e5f6", true, -112.5f, -7.3f,
      2000, "recent", "open", 17, "sos", false};
  const std::string statusPayload = coastcue::encodeStatusPayload(status);
  assert(statusPayload ==
         "{\"f\":true,\"c\":18,\"n\":\"esp32-a1b2c3d4e5f6\","
         "\"a\":2000,\"h\":\"recent\",\"r\":-112.5,\"q\":-7.3,\"s\":\"open\","
         "\"i\":17,\"e\":\"sos\",\"p\":false}");
  assert(statusPayload.size() <= coastcue::kBleStatusPayloadMaxBytes);

  const StatusSnapshot oversizedNode{
      false, UINT32_MAX,
      "esp32-this-node-value-is-intentionally-far-too-long-for-one-ble-notification-"
      "and-must-be-omitted-by-the-status-encoder",
      true, -164.0f, -99.9f, UINT32_MAX, "stale", "acknowledged",
      UINT32_MAX, "help", false};
  const std::string boundedPayload = coastcue::encodeStatusPayload(oversizedNode);
  assert(boundedPayload.size() <= coastcue::kBleStatusPayloadMaxBytes);
  assert(boundedPayload.find("\"n\"") == std::string::npos);
  assert(boundedPayload.find("\"s\":\"acknowledged\"") != std::string::npos);
  assert(boundedPayload.find("\"i\":4294967295") != std::string::npos);

  const StatusSnapshot noRadioYet{
      true, 0, "", false, 0.0f, 0.0f, UINT32_MAX, "stale", "resolved",
      0, "none", false};
  const std::string noRadioPayload = coastcue::encodeStatusPayload(noRadioYet);
  assert(noRadioPayload.find("\"a\":") == std::string::npos);
  assert(noRadioPayload.find("\"r\":") == std::string::npos);
  assert(noRadioPayload.find("\"q\":") == std::string::npos);

  // Notify pump: repeated failures abandon so a sync page always ends.
  {
    uint8_t failCount = 0;
    bool abandoned = false;
    for (uint8_t attempt = 0; attempt < 32; ++attempt) {
      const coastcue::NotifyDeliveryDecision decision =
          coastcue::decideNotifyDelivery(false, failCount);
      failCount = decision.nextFailCount;
      if (decision.action == coastcue::NotifyDeliveryAction::kAbandon) {
        abandoned = true;
        assert(decision.delayMs == 0);
        break;
      }
      assert(decision.action == coastcue::NotifyDeliveryAction::kRetryBackoff);
      assert(decision.delayMs > 0);
      assert(failCount == attempt + 1);
    }
    assert(abandoned);
    assert(failCount == coastcue::kBleNotifyMaxFailures);

    const coastcue::NotifyDeliveryDecision ok =
        coastcue::decideNotifyDelivery(true, 3);
    assert(ok.action == coastcue::NotifyDeliveryAction::kAdvance);
    assert(ok.nextFailCount == 0);
    assert(ok.delayMs == coastcue::kBleNotifyGapMs);
  }

  assert(coastcue::bleAttPayloadBytes(185) == 182);
  assert(coastcue::bleAttPayloadBytes(23) == 20);
  assert(coastcue::bleEventUnframedLimit(182) == coastcue::kBleEventChunkMaxBytes);
  assert(coastcue::bleEventUnframedLimit(20) == 20);
  assert(coastcue::bleEventChunkBytes(182) == coastcue::kBleEventChunkMaxBytes);
  assert(coastcue::bleEventChunkBytes(50) == 50 - coastcue::kBleEventFrameHeaderBytes);
  assert(coastcue::bleEventChunkBytes(16) == 0);

  assert(coastcue::syncJournalStartOffset(20, 20, 4096) == 4096);
  assert(coastcue::syncJournalStartOffset(25, 20, 4096) == 4096);
  assert(coastcue::syncJournalStartOffset(10, 20, 4096) == 0);
  assert(coastcue::syncJournalStartOffset(20, 20, 0) == 0);

  {
    coastcue::SyncPageScanner scanner;
    scanner.begin(100, 2);
    assert(scanner.observe(50) == coastcue::SyncRecordDisposition::kSkip);
    assert(scanner.observe(101) == coastcue::SyncRecordDisposition::kAccept);
    assert(scanner.observe(103) == coastcue::SyncRecordDisposition::kAccept);
    assert(scanner.hasGap());
    assert(scanner.sent() == 2);
    assert(scanner.last() == 103);
    assert(scanner.observe(104) == coastcue::SyncRecordDisposition::kPageFull);
    assert(scanner.hasMore());

    size_t lines = 0;
    bool yielded = false;
    while (lines < coastcue::kSyncScanLinesPerService * 2) {
      ++lines;
      if (scanner.shouldYield(lines)) {
        yielded = true;
        break;
      }
    }
    assert(yielded);
    assert(lines == coastcue::kSyncScanLinesPerService);
  }

  {
    const coastcue::SyncFailureResult failure{"sync-42", "notify_failed"};
    const std::string encoded = coastcue::encodeSyncFailureResult(failure);
    assert(encoded.find("\"kind\":\"command_result\"") != std::string::npos);
    assert(encoded.find("\"op\":\"sync\"") != std::string::npos);
    assert(encoded.find("\"sync_id\":\"sync-42\"") != std::string::npos);
    assert(encoded.find("\"ok\":false") != std::string::npos);
    assert(encoded.find("\"error\":\"notify_failed\"") != std::string::npos);
  }

  // Phone-connected prompts must not imply the button is mandatory.
  {
    const char* openPhone =
        coastcue::operatorIncidentPrompt(false, true);
    const char* ackPhone =
        coastcue::operatorIncidentPrompt(true, true);
    const char* openButton =
        coastcue::operatorIncidentPrompt(false, false);
    const char* ackButton =
        coastcue::operatorIncidentPrompt(true, false);
    assert(std::strcmp(openButton, "Press once: ack") == 0);
    assert(std::strcmp(ackButton, "Double-click: done") == 0);
    assert(std::strcmp(openPhone, "Phone/btn: ack") == 0);
    assert(std::strcmp(ackPhone, "Phone/btn: done") == 0);
    assert(std::strlen(openPhone) <= coastcue::kShoreDisplayPromptMaxChars);
    assert(std::strlen(ackPhone) <= coastcue::kShoreDisplayPromptMaxChars);
    assert(std::strstr(openPhone, "Press once") == nullptr);
  }

  // Ack/resolve are operator safety commands.
  {
    assert(coastcue::isOperatorSafetyCommand("ack"));
    assert(coastcue::isOperatorSafetyCommand("resolve"));
    assert(!coastcue::isOperatorSafetyCommand("sync"));
    assert(coastcue::isOperatorSafetyCommandJson(
        "{\"op\":\"ack\",\"record_id\":9,\"command_id\":\"c1\"}"));
    assert(coastcue::isOperatorSafetyCommandJson(
        "{\"op\":\"resolve\",\"record_id\":9,\"command_id\":\"c2\"}"));
    assert(!coastcue::isOperatorSafetyCommandJson(
        "{\"op\":\"sync\",\"after\":0,\"limit\":20,\"sync_id\":\"s1\"}"));
  }

  // Operator results outrank sync and are never abandoned.
  {
    assert(coastcue::classifyOutgoingJson(
               "{\"kind\":\"command_result\",\"op\":\"ack\",\"command_id\":\"c\","
               "\"ok\":true}") == coastcue::OutgoingKind::kOperatorResult);
    assert(coastcue::classifyOutgoingJson(
               "{\"kind\":\"command_result\",\"op\":\"resolve\",\"command_id\":\"c\","
               "\"ok\":true}") == coastcue::OutgoingKind::kOperatorResult);
    assert(coastcue::classifyOutgoingJson(
               "{\"kind\":\"sync_complete\",\"sync_id\":\"s1\",\"count\":0}") ==
           coastcue::OutgoingKind::kSyncPayload);
    assert(coastcue::classifyOutgoingJson(
               "{\"kind\":\"event\",\"sync_id\":\"s1\",\"record_id\":1}") ==
           coastcue::OutgoingKind::kSyncPayload);
    assert(coastcue::classifyOutgoingJson(
               "{\"kind\":\"transition\",\"source\":\"ble\",\"state\":\"acknowledged\"}") ==
        coastcue::OutgoingKind::kOther);

    assert(coastcue::shouldPromoteBleAuthenticatedLink(
        true, true, false, false, 1));
    assert(coastcue::shouldPromoteBleAuthenticatedLink(
        true, false, true, false, 1));
    assert(coastcue::shouldPromoteBleAuthenticatedLink(
        true, false, false, true, 1));
    assert(!coastcue::shouldPromoteBleAuthenticatedLink(
        false, true, true, true, 1));
    assert(!coastcue::shouldPromoteBleAuthenticatedLink(
        true, true, false, false, 0));
    assert(!coastcue::shouldPromoteBleAuthenticatedLink(
        true, true, false, false, 2));

    assert(coastcue::decideOutgoingAdmission(
               coastcue::OutgoingKind::kOperatorResult, 47, 48) ==
           coastcue::OutgoingAdmission::kEnqueueFront);
    assert(coastcue::decideOutgoingAdmission(
               coastcue::OutgoingKind::kSyncPayload, 47, 48) ==
           coastcue::OutgoingAdmission::kEnqueueTail);
    assert(coastcue::decideOutgoingAdmission(
               coastcue::OutgoingKind::kOperatorResult, 48, 48) ==
           coastcue::OutgoingAdmission::kEnqueueFront);
    assert(coastcue::decideOutgoingAdmission(
               coastcue::OutgoingKind::kSyncPayload, 48, 48) ==
           coastcue::OutgoingAdmission::kRefuse);
    assert(coastcue::mayDropOutgoingForOperator(
        coastcue::OutgoingKind::kSyncPayload));
    assert(!coastcue::mayDropOutgoingForOperator(
        coastcue::OutgoingKind::kOperatorResult));

    uint8_t failCount = 0;
    bool abandoned = false;
    for (uint8_t attempt = 0; attempt < 32; ++attempt) {
      const coastcue::NotifyDeliveryDecision decision =
          coastcue::decideNotifyDelivery(false, failCount, true);
      failCount = decision.nextFailCount;
      if (decision.action == coastcue::NotifyDeliveryAction::kAbandon) {
        abandoned = true;
        break;
      }
      assert(decision.action == coastcue::NotifyDeliveryAction::kRetryBackoff);
    }
    assert(!abandoned);

    const coastcue::OperatorNotifyFailureResult failure{
        "cmd-9", "ack", 42, true, "ble"};
    const std::string encoded =
        coastcue::encodeOperatorNotifyFailureResult(failure);
    assert(encoded.find("\"command_id\":\"cmd-9\"") != std::string::npos);
    assert(encoded.find("\"op\":\"ack\"") != std::string::npos);
    assert(encoded.find("\"error\":\"notify_failed\"") != std::string::npos);
    assert(encoded.find("\"ok\":false") != std::string::npos);
  }

  // Buzzer: silent once silenced, resolved, or without severity.
  assert(coastcue::shouldSoundAttentionBuzzer("open", 1, false));
  assert(coastcue::shouldSoundAttentionBuzzer("open", 2, false));
  assert(coastcue::shouldSoundAttentionBuzzer("acknowledged", 1, false));
  assert(!coastcue::shouldSoundAttentionBuzzer("open", 1, true));
  assert(!coastcue::shouldSoundAttentionBuzzer("acknowledged", 2, true));
  assert(!coastcue::shouldSoundAttentionBuzzer("resolved", 0, false));
  assert(!coastcue::shouldSoundAttentionBuzzer("resolved", 1, false));

  // Ack/resolve share the general command queue whenever it has space; the
  // operator queue is overflow only.
  {
    assert(coastcue::isOperatorSafetyCommandJson(
        "{\"command_id\":\"E621E1F8-C36C-495A-93FC-0C247A3E6E5F\","
        "\"op\":\"ack\",\"record_id\":42}"));
    assert(coastcue::decideCommandEnqueueTarget(true, true, true, false) ==
           coastcue::CommandEnqueueTarget::kGeneral);
    assert(coastcue::decideCommandEnqueueTarget(false, true, true, false) ==
           coastcue::CommandEnqueueTarget::kGeneral);
    assert(coastcue::decideCommandEnqueueTarget(true, false, true, false) ==
           coastcue::CommandEnqueueTarget::kOperator);
    assert(coastcue::decideCommandEnqueueTarget(true, false, false, false) ==
           coastcue::CommandEnqueueTarget::kBusy);
    assert(coastcue::decideCommandEnqueueTarget(true, true, true, true) ==
           coastcue::CommandEnqueueTarget::kBusy);

    char commandId[65] = {};
    assert(coastcue::copyJsonStringField(
        "{\"op\":\"ack\",\"record_id\":9,\"command_id\":\"cmd-42\"}",
        "command_id", commandId, sizeof(commandId)));
    assert(std::strcmp(commandId, "cmd-42") == 0);
    assert(coastcue::copyJsonStringField(
        "{\"op\":\"resolve\",\"record_id\":885,"
        "\"command_id\":\"A9A30C59-EB46-4D2C-AE90-56E78E00BCF1\"}",
        "command_id", commandId, sizeof(commandId)));
    assert(std::strcmp(commandId,
                       "A9A30C59-EB46-4D2C-AE90-56E78E00BCF1") == 0);
  }

  puts("shore_logic_test passed");
  return 0;
}
