#include <cassert>

#include "boat_button_logic.h"
#include "boat_emergency_scheduler.h"
#include "boat_gps_policy.h"
#include "boat_report_scheduler.h"

int main() {
  // Help requires an intentional hold beyond electrical debounce; exactly
  // three seconds is SOS.
  assert(coastcue::classifyBoatButtonRelease(0) ==
         coastcue::BoatButtonEvent::kNone);
  assert(coastcue::classifyBoatButtonRelease(249) ==
         coastcue::BoatButtonEvent::kNone);
  assert(coastcue::classifyBoatButtonRelease(250) ==
         coastcue::BoatButtonEvent::kHelp);
  assert(coastcue::classifyBoatButtonRelease(2999) ==
         coastcue::BoatButtonEvent::kHelp);
  assert(coastcue::classifyBoatButtonRelease(3000) ==
         coastcue::BoatButtonEvent::kSos);
  assert(!coastcue::shouldFireBoatSosWhileHeld(true, false, 2999));
  assert(coastcue::shouldFireBoatSosWhileHeld(true, false, 3000));
  assert(!coastcue::shouldFireBoatSosWhileHeld(true, true, 3000));
  assert(!coastcue::shouldFireBoatSosWhileHeld(false, false, 3000));

  constexpr uint32_t kReportPeriodMs = 5 * 60 * 1000;
  BoatReportScheduler scheduler;

  scheduler.scheduleFirstPeriodicReport(1, kReportPeriodMs);
  assert(scheduler.normalReportDue(1, kReportPeriodMs, false));

  // A first fresh GPS report must remain uncommitted until transmission ends.
  assert(scheduler.normalReportDue(1, kReportPeriodMs, true));
  scheduler.normalReportStarted(true);
  assert(!scheduler.hasSentFreshGpsReport());
  assert(!scheduler.normalReportDue(2, kReportPeriodMs, true));

  // A timeout or finish failure leaves it immediately due for radio recovery.
  scheduler.normalReportFinished(false, 3);
  assert(!scheduler.hasSentFreshGpsReport());
  assert(scheduler.normalReportDue(4, kReportPeriodMs, true));

  scheduler.normalReportStarted(true);
  scheduler.normalReportFinished(true, 5);
  assert(scheduler.hasSentFreshGpsReport());
  assert(!scheduler.normalReportDue(6, kReportPeriodMs, true));

  // The periodic report also remains due after a failed completion.
  BoatReportScheduler periodic;
  assert(periodic.normalReportDue(kReportPeriodMs, kReportPeriodMs, false));
  periodic.normalReportStarted(false);
  periodic.normalReportFinished(false, kReportPeriodMs + 1);
  assert(periodic.normalReportDue(kReportPeriodMs + 2, kReportPeriodMs, false));

  // Emergency state stores only intent. A fresh authenticated packet is built
  // by the caller immediately before every bounded radio attempt.
  coastcue::BoatEmergencyScheduler emergency;
  assert(emergency.queue(coastcue::BoatEmergencyKind::kHelp) ==
         coastcue::BoatEmergencyQueueResult::kQueued);
  assert(emergency.attemptDue(0, 2000));
  emergency.attemptStarted(0);
  assert(emergency.attemptCount() == 1);
  assert(emergency.attemptFinished(100) ==
         coastcue::BoatEmergencyCompletion::kRetryPending);
  assert(!emergency.attemptDue(2099, 2000));
  assert(emergency.attemptDue(2100, 2000));

  // Local failures consume the same finite budget; they cannot block normal
  // reports forever.
  assert(emergency.attemptStartFailed(2100) ==
         coastcue::BoatEmergencyCompletion::kRetryPending);
  assert(emergency.attemptCount() == 2);
  assert(emergency.attemptStartFailed(4200) ==
         coastcue::BoatEmergencyCompletion::kExhausted);
  assert(!emergency.hasPending());

  // SOS upgrades a queued Help immediately, or right after an in-flight Help.
  assert(emergency.queue(coastcue::BoatEmergencyKind::kHelp) ==
         coastcue::BoatEmergencyQueueResult::kQueued);
  emergency.attemptStarted(5000);
  assert(emergency.queue(coastcue::BoatEmergencyKind::kSos) ==
         coastcue::BoatEmergencyQueueResult::kUpgradePending);
  assert(emergency.activeKind() == coastcue::BoatEmergencyKind::kHelp);
  assert(emergency.attemptFinished(5100) ==
         coastcue::BoatEmergencyCompletion::kUpgraded);
  assert(emergency.activeKind() == coastcue::BoatEmergencyKind::kSos);
  assert(emergency.attemptCount() == 0);
  assert(emergency.attemptDue(5100, 2000));

  // Receipt-driven cancellation is safe only after local transmit completion.
  emergency.attemptStarted(5100);
  assert(!emergency.cancelPending());
  assert(emergency.attemptFinished(5200) ==
         coastcue::BoatEmergencyCompletion::kRetryPending);
  assert(emergency.cancelPending());
  assert(!emergency.hasPending());
  assert(emergency.attemptCount() == 0);
  assert(!emergency.cancelPending());

  // Help→SOS is a new Incident identity so the shore must reopen attention;
  // repeated same-severity presses remain within the current Incident.
  using coastcue::BoatEmergencyKind;
  using coastcue::BoatIncidentIdentityAction;
  assert(coastcue::decideBoatIncidentIdentity(
             false, false, BoatEmergencyKind::kNone,
             BoatEmergencyKind::kHelp) ==
         BoatIncidentIdentityAction::kStartNew);
  assert(coastcue::decideBoatIncidentIdentity(
             true, false, BoatEmergencyKind::kHelp,
             BoatEmergencyKind::kHelp) ==
         BoatIncidentIdentityAction::kKeepCurrent);
  assert(coastcue::decideBoatIncidentIdentity(
             true, false, BoatEmergencyKind::kHelp,
             BoatEmergencyKind::kSos) ==
         BoatIncidentIdentityAction::kStartNew);
  assert(!coastcue::shouldQueueBoatEmergencyAction(
      true, false, false, BoatEmergencyKind::kHelp,
      BoatEmergencyKind::kHelp));
  assert(!coastcue::shouldQueueBoatEmergencyAction(
      true, false, false, BoatEmergencyKind::kSos,
      BoatEmergencyKind::kSos));
  assert(coastcue::shouldQueueBoatEmergencyAction(
      true, false, false, BoatEmergencyKind::kHelp,
      BoatEmergencyKind::kSos));
  assert(coastcue::shouldQueueBoatEmergencyAction(
      true, true, false, BoatEmergencyKind::kHelp,
      BoatEmergencyKind::kHelp));
  assert(coastcue::shouldDeferBoatSosIdentityHandoff(true, true, false));
  assert(coastcue::shouldDeferBoatSosIdentityHandoff(true, false, true));
  assert(!coastcue::shouldDeferBoatSosIdentityHandoff(true, false, false));
  assert(!coastcue::shouldDeferBoatSosIdentityHandoff(false, true, true));
  assert(coastcue::shouldMarkBoatSosIdentityPending(
      coastcue::BoatEmergencyQueueResult::kUpgraded));
  assert(coastcue::shouldMarkBoatSosIdentityPending(
      coastcue::BoatEmergencyQueueResult::kQueued));
  assert(!coastcue::shouldMarkBoatSosIdentityPending(
      coastcue::BoatEmergencyQueueResult::kUpgradePending));
  assert(coastcue::decideBoatIncidentIdentity(
             true, false, BoatEmergencyKind::kSos,
             BoatEmergencyKind::kSos) ==
         BoatIncidentIdentityAction::kKeepCurrent);
  assert(coastcue::decideBoatIncidentIdentity(
             true, true, BoatEmergencyKind::kHelp,
             BoatEmergencyKind::kSos) ==
         BoatIncidentIdentityAction::kStartNew);

  const auto staleNormal =
      coastcue::selectBoatGpsPolicy(true, 11000, false, 10000);
  assert(!staleNormal.includeCoordinates);
  assert(!staleNormal.fresh);
  const auto staleEmergency =
      coastcue::selectBoatGpsPolicy(true, 11000, true, 10000);
  assert(staleEmergency.includeCoordinates);
  assert(!staleEmergency.fresh);
  assert(staleEmergency.ageSeconds == 11);
  const auto fieldFreshBoundary =
      coastcue::selectBoatGpsPolicy(true, 30000, false, 30000);
  assert(fieldFreshBoundary.includeCoordinates);
  assert(fieldFreshBoundary.fresh);
  const auto fieldStaleBoundary =
      coastcue::selectBoatGpsPolicy(true, 30001, false, 30000);
  assert(!fieldStaleBoundary.includeCoordinates);
  assert(!fieldStaleBoundary.fresh);
  const auto oldEmergency = coastcue::selectBoatGpsPolicy(
      true, UINT32_MAX - 1, true, 10000);
  assert(oldEmergency.includeCoordinates);
  assert(oldEmergency.ageSeconds == 4294967);
}
