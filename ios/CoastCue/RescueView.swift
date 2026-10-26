import SwiftData
import SwiftUI

struct RescueView: View {
  let model: AppModel
  @Query(sort: \MarineAlert.createdAt, order: .reverse) private var alerts: [MarineAlert]
  @Query(sort: \BoatSnapshot.receivedAt, order: .reverse) private var snapshots: [BoatSnapshot]
  @State private var alertToResolve: MarineAlert?
  @State private var statusRecordIDToResolve: Int64?
  @State private var didRequestResolve = false
  @State private var resolvedRoute: AppRoute?
  @Environment(\.dynamicTypeSize) private var dynamicTypeSize
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  private var activeAlert: MarineAlert? {
    activeRescueAlert(
      status: model.currentGatewayStatus,
      isCurrent: model.hasCurrentGatewaySnapshot,
      gatewayIdentity: model.gatewayIdentity,
      alerts: alerts
    )
  }

  private var latestPosition: BoatSnapshot? {
    guard let identity = model.gatewayIdentity else { return nil }
    return latestValidLocationSnapshot(gatewayIdentity: identity, snapshots: snapshots)
  }

  private var presentationState: RescuePresentationState {
    rescuePresentationState(
      status: model.currentGatewayStatus,
      hasLocalAlert: activeAlert != nil,
      localAlertRecordID: activeAlert?.recordID,
      isCurrent: model.hasCurrentGatewaySnapshot
    )
  }

  var body: some View {
    ScrollView {
      VStack(spacing: CoastCueStyle.sectionSpacing) {
        CoastCuePageHeader(title: String(localized: "Rescue"))

        ConnectionStrip(model: model)

        switch presentationState {
        case .awaitingCurrentStatus:
          awaitingGatewayStatus
        case .activeAlert:
          if let alert = activeAlert {
            activeRescue(alert)
          } else {
            activeDetailsPending
          }
        case .activeDetailsPending:
          activeDetailsPending
        case .noActiveRescue:
          noActiveRescue
        }
      }
      .padding(.horizontal, CoastCueStyle.pageHorizontalInset)
      .padding(.top, CoastCueStyle.rootTopPadding)
      .padding(.bottom, 28)
      .frame(maxWidth: 720)
      .frame(maxWidth: .infinity)
    }
    .background(CoastCueStyle.canvas.ignoresSafeArea())
    .toolbar(.hidden, for: .navigationBar)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: presentationState)
    .onChange(of: model.gatewayIdentity) { _, _ in
      didRequestResolve = false
      resolvedRoute = nil
    }
    .onChange(of: model.currentGatewayStatus?.activeRecordID) { previous, current in
      if let current, current != previous {
        didRequestResolve = false
        resolvedRoute = nil
      }
    }
    .background {
      Color.clear
        .alert(
          String(localized: "Resolve this rescue?"),
          isPresented: Binding(
            get: { alertToResolve != nil || statusRecordIDToResolve != nil },
            set: {
              if !$0 {
                alertToResolve = nil
                statusRecordIDToResolve = nil
              }
            }
          )
        ) {
          Button(String(localized: "Resolve"), role: .destructive) {
            didRequestResolve = true
            if let alertToResolve {
              resolvedRoute = .incident(alertToResolve.gatewayIdentity, alertToResolve.eventKey)
              model.resolve(alertToResolve)
            } else if let statusRecordIDToResolve {
              resolvedRoute = nil
              model.resolve(recordID: statusRecordIDToResolve)
            }
            alertToResolve = nil
            statusRecordIDToResolve = nil
          }
          Button(String(localized: "Cancel"), role: .cancel) {}
        } message: {
          Text(String(localized: "Mark this incident resolved at the shore gateway only when assistance is complete."))
        }
        .tint(CoastCueStyle.ink)
    }
  }

  @ViewBuilder
  private func activeRescue(_ alert: MarineAlert) -> some View {
    let incidentSnapshot = incidentLocationSnapshot(
      gatewayIdentity: alert.gatewayIdentity,
      eventKey: alert.eventKey,
      snapshots: snapshots
    )
    let postAlertPosition = incidentSnapshot == nil
      ? latestValidLocationAfterAlert(alert, snapshots: snapshots)
      : nil

    CoastCueCard {
      VStack(alignment: .leading, spacing: 18) {
        VStack(alignment: .leading, spacing: 8) {
          let layout = dynamicTypeSize.isAccessibilitySize
            ? AnyLayout(VStackLayout(alignment: .leading, spacing: 8))
            : AnyLayout(HStackLayout(alignment: .center, spacing: 12))
          layout {
            Text(alert.displayTitle)
              .font(.largeTitle.weight(.bold))
              .foregroundStyle(CoastCueStyle.ink)
              .frame(maxWidth: .infinity, alignment: .leading)
            StatusBadge(status: alert.status)
          }
          Text(alert.displayedTimestamp)
            .font(.subheadline)
            .foregroundStyle(CoastCueStyle.muted)
        }

        Divider().overlay(CoastCueStyle.line)

        NavigationLink(value: AppRoute.incident(alert.gatewayIdentity, alert.eventKey)) {
          HStack(spacing: 12) {
            Image(
              systemName: incidentSnapshot != nil || postAlertPosition != nil
                ? "location.fill" : "location.slash"
            )
            .font(.title3)
            .foregroundStyle(
              incidentSnapshot != nil
                ? CoastCueStyle.emergency
                : postAlertPosition != nil ? CoastCueStyle.orange : CoastCueStyle.muted
            )
            .frame(minWidth: 28)
            VStack(alignment: .leading, spacing: 2) {
              Text(String(localized: "Location & incident details"))
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(CoastCueStyle.ink)
              Text(
                incidentSnapshot != nil
                  ? String(localized: "Incident position available")
                  : postAlertPosition != nil
                    ? String(localized: "Latest known boat position after alert")
                    : String(localized: "Incident report has no valid GPS position")
              )
              .font(.caption)
              .foregroundStyle(CoastCueStyle.muted)
              if let position = incidentSnapshot ?? postAlertPosition,
                let distance = shoreDistanceText(for: position)
              {
                Text(distance)
                  .font(.caption.weight(.semibold))
                  .foregroundStyle(CoastCueStyle.orange)
              }
            }
            Spacer()
            Image(systemName: "chevron.right")
              .font(.caption.weight(.bold))
              .foregroundStyle(CoastCueStyle.muted)
          }
          .padding(12)
          .background(CoastCueStyle.canvas)
          .clipShape(CoastCueStyle.cardShape(radius: CoastCueStyle.nestedRadius))
        }
        .buttonStyle(.plain)

        VStack(alignment: .leading, spacing: 12) {
          if alert.status == .acknowledged { acknowledgedNotice }
          if alert.status == .open {
            Button {
              model.acknowledge(alert)
            } label: {
              if model.isAlertCommandPending(recordID: alert.recordID, operation: "ack") {
                pendingActionLabel(String(localized: "Acknowledging…"))
              } else {
                Label(String(localized: "Acknowledge"), systemImage: "checkmark")
              }
            }
            .buttonStyle(PrimaryRescueButtonStyle())
            .disabled(!model.canSendAlertCommand || model.isAlertCommandPending(recordID: alert.recordID))
            .accessibilityLabel(String(localized: "Acknowledge alert"))
          }

          Button(role: .destructive) {
            alertToResolve = alert
          } label: {
            if model.isAlertCommandPending(recordID: alert.recordID, operation: "resolve") {
              pendingActionLabel(String(localized: "Resolving…"))
            } else {
              Label(String(localized: "Resolve"), systemImage: "checkmark.seal")
            }
          }
          .buttonStyle(ResolveButtonStyle())
          .disabled(!model.canSendAlertCommand || model.isAlertCommandPending(recordID: alert.recordID))
          .accessibilityLabel(String(localized: "Resolve alert"))
        }

        if model.isAlertCommandPending(recordID: alert.recordID) {
          Text(String(localized: "Waiting for confirmation from the shore gateway."))
            .font(.footnote)
            .foregroundStyle(CoastCueStyle.muted)
        }
        if !model.canSendAlertCommand {
          Label(
            String(localized: "Connect to send actions"),
            systemImage: "antenna.radiowaves.left.and.right.slash"
          )
          .font(.footnote)
          .foregroundStyle(CoastCueStyle.muted)
        }
      }
    }

    if let postAlertPosition,
      let latitude = postAlertPosition.latitude,
      let longitude = postAlertPosition.longitude
    {
      postAlertPositionCard(
        latitude: latitude,
        longitude: longitude,
        snapshot: postAlertPosition
      )
    }
  }

  private func postAlertPositionCard(
    latitude: Double,
    longitude: Double,
    snapshot: BoatSnapshot
  ) -> some View {
    return CoastCueCard {
      VStack(alignment: .leading, spacing: 12) {
        Label(
          String(localized: "Latest known boat position after alert"),
          systemImage: "location.fill"
        )
        .font(.headline)
        .foregroundStyle(CoastCueStyle.ink)

        VStack(alignment: .leading, spacing: 5) {
          Text(
            "\(latitude, format: .number.precision(.fractionLength(5))), \(longitude, format: .number.precision(.fractionLength(5)))"
          )
          .font(.system(.footnote, design: .monospaced))
          Text(snapshot.displayedTimestamp)
            .font(.caption)
        }
        .foregroundStyle(CoastCueStyle.muted)

        if let distance = shoreDistanceText(for: snapshot) {
          Label(distance, systemImage: "arrow.left.and.right")
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(CoastCueStyle.orange)
        }
      }
    }
  }

  private var noActiveRescue: some View {
    VStack(spacing: CoastCueStyle.sectionSpacing) {
      CoastCueCard {
        VStack(alignment: .leading, spacing: 14) {
          Image(systemName: "checkmark.shield.fill")
            .font(.largeTitle)
            .foregroundStyle(CoastCueStyle.success)
          Text(didRequestResolve ? String(localized: "Rescue resolved") : String(localized: "No active rescue"))
            .font(.title2.weight(.bold))
            .foregroundStyle(CoastCueStyle.ink)
          Text(didRequestResolve
            ? String(localized: "The shore gateway has confirmed resolution.")
            : String(localized: "The shore gateway is current. CoastCue will sync again when the app returns to the foreground."))
          .font(.body)
          .foregroundStyle(CoastCueStyle.muted)
          if didRequestResolve, let resolvedRoute {
            NavigationLink(value: resolvedRoute) {
              Text(String(localized: "View resolved incident"))
                .font(.subheadline.weight(.semibold))
                .frame(minHeight: 44)
            }
          }
        }
      }

      if let snapshot = latestPosition,
        let latitude = snapshot.latitude,
        let longitude = snapshot.longitude
      {
        CoastCueCard {
          VStack(alignment: .leading, spacing: 12) {
            Label(String(localized: "Latest boat position"), systemImage: "location.fill")
              .font(.headline)
              .foregroundStyle(CoastCueStyle.ink)

            VStack(alignment: .leading, spacing: 5) {
              Text(
                "\(latitude, format: .number.precision(.fractionLength(5))), \(longitude, format: .number.precision(.fractionLength(5)))"
              )
              .font(.system(.footnote, design: .monospaced))
              Text(snapshot.displayedTimestamp)
                .font(.caption)
            }
            .foregroundStyle(CoastCueStyle.muted)

            if let distance = shoreDistanceText(for: snapshot) {
              Label(distance, systemImage: "arrow.left.and.right")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(CoastCueStyle.orange)
            }
          }
        }
      }
    }
  }

  private var awaitingGatewayStatus: some View {
    CoastCueCard {
      VStack(alignment: .leading, spacing: 14) {
        Image(systemName: "antenna.radiowaves.left.and.right")
          .font(.largeTitle)
          .foregroundStyle(CoastCueStyle.orange)
        Text(String(localized: "Waiting for current gateway status"))
          .font(.title2.weight(.bold))
          .foregroundStyle(CoastCueStyle.ink)
        Text(String(localized: "Connect and sync before relying on whether a rescue is active."))
          .font(.body)
          .foregroundStyle(CoastCueStyle.muted)
        NavigationLink(value: AppRoute.pairingHelp) {
          Label(String(localized: "Pairing help"), systemImage: "questionmark.circle")
            .frame(minHeight: 44)
        }
        .font(.subheadline.weight(.semibold))
      }
    }
  }

  private var activeDetailsPending: some View {
    CoastCueCard {
      VStack(alignment: .leading, spacing: 14) {
        Image(systemName: "exclamationmark.triangle.fill")
          .font(.largeTitle)
          .foregroundStyle(CoastCueStyle.emergency)
        Text(String(localized: "Active rescue received"))
          .font(.title2.weight(.bold))
          .foregroundStyle(CoastCueStyle.ink)
        Text(
          String(
            localized:
              "The gateway reports an active alert whose detail record is not stored on this iPhone. Gateway actions remain available.")
        )
        .font(.body)
        .foregroundStyle(CoastCueStyle.muted)

        if let status = model.currentGatewayStatus,
          let recordID = status.activeRecordID,
          let alertState = status.alertState.flatMap(AlertStatus.init(rawValue:))
        {
          HStack {
            Text(status.severity == "sos" ? "SOS" : String(localized: "Help"))
              .font(.title2.weight(.bold))
              .foregroundStyle(CoastCueStyle.ink)
            Spacer()
            StatusBadge(status: alertState)
          }
          VStack(alignment: .leading, spacing: 12) {
            if alertState == .acknowledged { acknowledgedNotice }
            if alertState == .open {
              Button {
                model.acknowledge(recordID: recordID)
              } label: {
                if model.isAlertCommandPending(recordID: recordID, operation: "ack") {
                  pendingActionLabel(String(localized: "Acknowledging…"))
                } else {
                  Label(String(localized: "Acknowledge"), systemImage: "checkmark")
                }
              }
              .buttonStyle(PrimaryRescueButtonStyle())
              .disabled(!model.canSendAlertCommand || model.isAlertCommandPending(recordID: recordID))
            }

            Button(role: .destructive) {
              statusRecordIDToResolve = recordID
            } label: {
              if model.isAlertCommandPending(recordID: recordID, operation: "resolve") {
                pendingActionLabel(String(localized: "Resolving…"))
              } else {
                Label(String(localized: "Resolve"), systemImage: "checkmark.seal")
              }
            }
            .buttonStyle(ResolveButtonStyle())
            .disabled(!model.canSendAlertCommand || model.isAlertCommandPending(recordID: recordID))
          }

          if model.isAlertCommandPending(recordID: recordID) {
            Text(String(localized: "Waiting for confirmation from the shore gateway."))
              .font(.footnote)
              .foregroundStyle(CoastCueStyle.muted)
          }
          if !model.canSendAlertCommand {
            Label(
              String(localized: "Connect to send actions"),
              systemImage: "antenna.radiowaves.left.and.right.slash"
            )
            .font(.footnote)
            .foregroundStyle(CoastCueStyle.muted)
          }
        }
      }
    }
  }

  private var acknowledgedNotice: some View {
    Label {
      Text(String(localized: "The shore gateway has acknowledged this rescue. Resolve it only when assistance is complete."))
        .foregroundStyle(CoastCueStyle.ink)
    } icon: {
      Image(systemName: "checkmark.circle.fill")
        .foregroundStyle(CoastCueStyle.success)
    }
    .font(.subheadline)
    .fixedSize(horizontal: false, vertical: true)
    .padding(.vertical, 4)
  }

  private func pendingActionLabel(_ title: String) -> some View {
    HStack(spacing: 10) {
      ProgressView()
        .controlSize(.small)
      Text(title)
    }
  }

  private func shoreDistanceText(for snapshot: BoatSnapshot) -> String? {
    guard let reference = model.shoreReference(for: snapshot.gatewayIdentity),
      let latitude = snapshot.latitude,
      let longitude = snapshot.longitude,
      let meters = greatCircleDistanceMeters(
        fromLatitude: reference.latitude,
        fromLongitude: reference.longitude,
        toLatitude: latitude,
        toLongitude: longitude)
    else { return nil }
    return String(
      format: String(localized: "About %@ from shore reference"),
      formattedApproximateDistance(meters))
  }
}

#Preview("Rescue") {
  NavigationStack {
    RescueView(model: AppModel(ble: BluetoothGatewayService()))
  }
  .modelContainer(for: [BoatSnapshot.self, MarineAlert.self], inMemory: true)
}
