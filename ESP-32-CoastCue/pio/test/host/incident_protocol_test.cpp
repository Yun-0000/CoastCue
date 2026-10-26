#include <cassert>
#include <cstring>

#include "boat_incident_snapshot.h"
#include "incident_protocol.h"

int main() {
  using coastcue::BoatIncidentState;
  using coastcue::IncidentApplyResult;
  using coastcue::IncidentState;

  IncidentState parsed = IncidentState::kNone;
  assert(coastcue::parseIncidentStateWireName("receipt", parsed));
  assert(parsed == IncidentState::kReceipt);
  assert(std::strcmp(coastcue::incidentStateWireName(IncidentState::kResolved), "resolved") == 0);
  assert(!coastcue::parseIncidentStateWireName("ack", parsed));
  assert(coastcue::isValidIncidentId("0123456789abcdef"));
  assert(!coastcue::isValidIncidentId("0123456789ABCDEF"));
  assert(!coastcue::isValidIncidentId("0123456789abcde"));

  // The boat must not clear the shore's pending revision until its own NVS
  // checkpoint succeeded. A later replay may retry the local persistence.
  assert(!coastcue::shouldSendStateReceipt(
      IncidentApplyResult::kApplied, false));
  assert(coastcue::shouldSendStateReceipt(
      IncidentApplyResult::kApplied, true));
  assert(coastcue::shouldSendStateReceipt(
      IncidentApplyResult::kDuplicate, true));
  assert(!coastcue::shouldSendStateReceipt(
      IncidentApplyResult::kConflict, true));
  assert(!coastcue::shouldSendStateReceipt(
      IncidentApplyResult::kInvalid, true));
  // Stale or terminal-ignored downlinks were not accepted. Confirming the
  // boat's different current sequence cannot satisfy that shore revision and
  // only consumes another half-duplex slot.
  assert(!coastcue::shouldSendStateReceipt(
      IncidentApplyResult::kStale, true));
  assert(!coastcue::shouldSendStateReceipt(
      IncidentApplyResult::kTerminalIgnored, true));

  assert(coastcue::shouldRetirePendingStateReceipt(
      true, false, "0123456789abcdef", "fedcba9876543210"));
  assert(!coastcue::shouldRetirePendingStateReceipt(
      true, true, "0123456789abcdef", "fedcba9876543210"));
  assert(!coastcue::shouldRetirePendingStateReceipt(
      true, false, "0123456789abcdef", "0123456789abcdef"));
  assert(!coastcue::shouldRetirePendingStateReceipt(
      false, false, "0123456789abcdef", "fedcba9876543210"));
  assert(coastcue::isMatchingPendingStateReceipt(
      "0123456789abcdef", 3, "0123456789abcdef", 3));
  assert(!coastcue::isMatchingPendingStateReceipt(
      "0123456789abcdef", 2, "0123456789abcdef", 3));
  assert(!coastcue::isMatchingPendingStateReceipt(
      "0123456789abcdef", 3, "fedcba9876543210", 3));
  assert(!coastcue::isMatchingPendingStateReceipt(
      "0123456789abcdef", 0, "0123456789abcdef", 0));

  coastcue::BoatIncidentSnapshot snapshot;
  std::memcpy(snapshot.incidentId, "0123456789abcdef", 17);
  snapshot.state = IncidentState::kAcknowledged;
  snapshot.stateSeq = 23;
  snapshot.hasStateSeq = true;
  snapshot.kind = coastcue::BoatEmergencyKind::kHelp;
  uint8_t encodedSnapshot[coastcue::kBoatIncidentSnapshotBytes];
  assert(coastcue::encodeBoatIncidentSnapshot(
      snapshot, encodedSnapshot, sizeof(encodedSnapshot)));
  coastcue::BoatIncidentSnapshot decodedSnapshot;
  assert(coastcue::decodeBoatIncidentSnapshot(
      encodedSnapshot, sizeof(encodedSnapshot), decodedSnapshot));
  assert(std::strcmp(decodedSnapshot.incidentId, snapshot.incidentId) == 0);
  assert(decodedSnapshot.state == snapshot.state);
  assert(decodedSnapshot.stateSeq == snapshot.stateSeq);
  assert(decodedSnapshot.hasStateSeq);
  assert(decodedSnapshot.kind == snapshot.kind);
  encodedSnapshot[0] = 'X';
  assert(!coastcue::decodeBoatIncidentSnapshot(
      encodedSnapshot, sizeof(encodedSnapshot), decodedSnapshot));
  assert(coastcue::encodeBoatIncidentSnapshot(
      snapshot, encodedSnapshot, sizeof(encodedSnapshot)));
  encodedSnapshot[5] = 2;
  assert(!coastcue::decodeBoatIncidentSnapshot(
      encodedSnapshot, sizeof(encodedSnapshot), decodedSnapshot));
  assert(coastcue::encodeBoatIncidentSnapshot(
      snapshot, encodedSnapshot, sizeof(encodedSnapshot)));
  encodedSnapshot[10] = 'A';
  assert(!coastcue::decodeBoatIncidentSnapshot(
      encodedSnapshot, sizeof(encodedSnapshot), decodedSnapshot));
  coastcue::BoatIncidentSnapshot invalidOpen = snapshot;
  invalidOpen.state = IncidentState::kOpen;
  assert(!coastcue::encodeBoatIncidentSnapshot(
      invalidOpen, encodedSnapshot, sizeof(encodedSnapshot)));
  snapshot.stateSeq = UINT32_MAX;
  assert(coastcue::encodeBoatIncidentSnapshot(
      snapshot, encodedSnapshot, sizeof(encodedSnapshot)));
  assert(coastcue::decodeBoatIncidentSnapshot(
      encodedSnapshot, sizeof(encodedSnapshot), decodedSnapshot));
  assert(decodedSnapshot.stateSeq == UINT32_MAX);

  // Normal GPS never enters the Incident or receipt path.
  assert(!coastcue::isValidIncidentUplink({"0123456789abcdef", IncidentState::kOpen, false}));
  assert(!coastcue::isValidIncidentUplink({"0123456789abcdef", IncidentState::kReceipt, true}));
  assert(coastcue::isValidIncidentUplink({"0123456789abcdef", IncidentState::kOpen, true}));

  BoatIncidentState boat;
  assert(boat.begin("0123456789abcdef"));
  assert(boat.shouldRetryOpen());
  assert(coastcue::shouldSendOpenIncidentOnPeriodicSlot(boat));
  assert(boat.applyDownlink({"0123456789abcdef", IncidentState::kReceipt, 7}) == IncidentApplyResult::kApplied);
  assert(!boat.shouldRetryOpen());
  assert(!coastcue::shouldSendOpenIncidentOnPeriodicSlot(boat));
  assert(boat.applyDownlink({"0123456789abcdef", IncidentState::kReceipt, 7}) == IncidentApplyResult::kDuplicate);
  assert(boat.applyDownlink({"0123456789abcdef", IncidentState::kAcknowledged, 8}) == IncidentApplyResult::kApplied);
  assert(boat.applyDownlink({"0123456789abcdef", IncidentState::kReceipt, 7}) == IncidentApplyResult::kStale);
  assert(boat.applyDownlink({"0123456789abcdef", IncidentState::kReceipt, 8}) == IncidentApplyResult::kConflict);
  assert(boat.applyDownlink({"0123456789abcdef", IncidentState::kReceipt, 9}) == IncidentApplyResult::kStale);

  BoatIncidentState directAcknowledged;
  assert(directAcknowledged.begin("0011223344556677"));
  assert(directAcknowledged.applyDownlink({"0011223344556677", IncidentState::kAcknowledged, 3}) == IncidentApplyResult::kApplied);
  assert(!directAcknowledged.shouldRetryOpen());

  // OPEN may become RESOLVED without a receipt or acknowledgement.
  BoatIncidentState directResolved;
  assert(directResolved.begin("fedcba9876543210"));
  assert(directResolved.applyDownlink({"fedcba9876543210", IncidentState::kResolved, 42}) == IncidentApplyResult::kApplied);
  assert(directResolved.isTerminal());
  assert(!directResolved.shouldRetryOpen());
  assert(directResolved.applyDownlink({"fedcba9876543210", IncidentState::kResolved, 42}) == IncidentApplyResult::kDuplicate);
  // Resolved dominates even a malformed later nonterminal revision.
  assert(directResolved.applyDownlink({"fedcba9876543210", IncidentState::kAcknowledged, 99}) == IncidentApplyResult::kTerminalIgnored);
  assert(directResolved.applyDownlink({"fedcba9876543210", IncidentState::kReceipt, 1}) == IncidentApplyResult::kTerminalIgnored);
  assert(directResolved.state() == IncidentState::kResolved);

  // Retries have fresh transport nonces outside this reducer; the stable ID
  // is what binds all replies. A different ID cannot alter this incident.
  assert(std::strcmp(boat.incidentId(), "0123456789abcdef") == 0);
  assert(coastcue::shouldOpenIncidentResponseWindow(
      "0123456789abcdef", boat));
  assert(!coastcue::shouldOpenIncidentResponseWindow(
      "0011223344556677", boat));
  assert(boat.applyDownlink({"0011223344556677", IncidentState::kResolved, 9}) == IncidentApplyResult::kInvalid);

  BoatIncidentState restored;
  assert(restored.restore("0123456789abcdef", IncidentState::kAcknowledged, 8));
  assert(!restored.shouldRetryOpen());
  assert(restored.applyDownlink({"0123456789abcdef", IncidentState::kResolved, 9}) == IncidentApplyResult::kApplied);
  assert(!restored.restore("invalid", IncidentState::kOpen, 0, false));
  assert(!restored.restore("0123456789abcdef", IncidentState::kOpen, 1, false));
  assert(!restored.restore("0123456789abcdef", IncidentState::kReceipt, 0));
  assert(!coastcue::isValidIncidentDownlink({"0123456789abcdef", IncidentState::kOpen, 1}));
  assert(!coastcue::isValidIncidentDownlink({"0123456789abcdef", IncidentState::kReceipt, 0}));

  coastcue::BoatTerminalReconciliation terminalReconciliation;
  terminalReconciliation.begin();
  assert(terminalReconciliation.shouldListen());
  terminalReconciliation.observeQuietWindow();
  assert(terminalReconciliation.shouldListen());
  terminalReconciliation.observeStateReplay();
  terminalReconciliation.observeQuietWindow();
  assert(terminalReconciliation.shouldListen());
  terminalReconciliation.observeQuietWindow();
  assert(!terminalReconciliation.shouldListen());
}
