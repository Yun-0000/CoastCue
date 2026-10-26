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
    Page(String(localized: "Rescue"), isTabRoot: true) {
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
          Text(String(localized: "Only when assistance is complete."))
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

        CardDivider()

        NavigationLink(value: AppRoute.incident(alert.gatewayIdentity, alert.eventKey)) {
          HStack(spacing: 12) {
            Image(
              systemName: incidentSnapshot != nil || postAlertPosition != nil
                ? "location.fill" : "location.slash"
            )
            .font(.title3)
            .foregroundStyle(
              incidentSnapshot != nil || postAlertPosition != nil ? CoastCueStyle.orange : CoastCueStyle.muted
            )
            .frame(minWidth: 28)
            VStack(alignment: .leading, spacing: 2) {
              Text(String(localized: "Location & incident details"))
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(CoastCueStyle.ink)
              // Only what the user can't already infer: the distance (or coordinates), and a
              // note when the position is not the incident's own or is missing.
              if incidentSnapshot == nil {
                Text(
                  postAlertPosition != nil
                    ? String(localized: "Latest known boat position after alert")
                    : String(localized: "Incident report has no valid GPS position")
                )
                .font(.caption)
                .foregroundStyle(CoastCueStyle.muted)
              }
              if let position = incidentSnapshot ?? postAlertPosition {
                if let distance = shoreDistanceText(for: position) {
                  Text(distance)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(CoastCueStyle.orange)
                } else if let latitude = position.latitude, let longitude = position.longitude {
                  Text(coordinateText(latitude, longitude))
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(CoastCueStyle.muted)
                }
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

        alertActions(
          recordID: alert.recordID, status: alert.status,
          acknowledge: { model.acknowledge(alert) },
          resolve: { alertToResolve = alert })
      }
    }

    if let postAlertPosition {
      positionCard(String(localized: "Latest known boat position after alert"), snapshot: postAlertPosition)
    }
  }

  @ViewBuilder
  private func positionCard(_ title: String, snapshot: BoatSnapshot) -> some View {
    if let latitude = snapshot.latitude, let longitude = snapshot.longitude {
      CoastCueCard {
        VStack(alignment: .leading, spacing: 12) {
          Label {
            Text(title).foregroundStyle(CoastCueStyle.ink)
          } icon: {
            Image(systemName: "location.fill").foregroundStyle(CoastCueStyle.orange)
          }
          .font(.headline)

          VStack(alignment: .leading, spacing: 5) {
            Text(
              coordinateText(latitude, longitude)
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
          if didRequestResolve, let resolvedRoute {
            NavigationLink(value: resolvedRoute) {
              Text(String(localized: "View resolved incident"))
                .font(.subheadline.weight(.semibold))
                .frame(minHeight: 44)
            }
          }
        }
      }

      if let latestPosition {
        positionCard(String(localized: "Latest boat position"), snapshot: latestPosition)
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
        Text(String(localized: "Details are still syncing."))
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
          alertActions(
            recordID: recordID, status: alertState,
            acknowledge: { model.acknowledge(recordID: recordID) },
            resolve: { statusRecordIDToResolve = recordID })
        }
      }
    }
  }

  /// Acknowledge/Resolve controls shared by local and status-only alerts.
  @ViewBuilder
  private func alertActions(
    recordID: Int64, status: AlertStatus,
    acknowledge: @escaping () -> Void, resolve: @escaping () -> Void
  ) -> some View {
    let pending = model.isAlertCommandPending(recordID: recordID)
    VStack(alignment: .leading, spacing: 12) {
      if status == .acknowledged { acknowledgedNotice }
      if status == .open {
        Button(action: acknowledge) {
          if model.isAlertCommandPending(recordID: recordID, operation: "ack") {
            pendingActionLabel(String(localized: "Acknowledging…"))
          } else {
            Label(String(localized: "Acknowledge"), systemImage: "checkmark")
          }
        }
        .buttonStyle(PrimaryRescueButtonStyle())
        .disabled(!model.canSendAlertCommand || pending)
        .accessibilityLabel(String(localized: "Acknowledge alert"))
      }

      Button(role: .destructive, action: resolve) {
        if model.isAlertCommandPending(recordID: recordID, operation: "resolve") {
          pendingActionLabel(String(localized: "Resolving…"))
        } else {
          Label(String(localized: "Resolve"), systemImage: "checkmark.seal")
        }
      }
      .buttonStyle(ResolveButtonStyle())
      .disabled(!model.canSendAlertCommand || pending)
      .accessibilityLabel(String(localized: "Resolve alert"))
    }

    if pending {
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

  private var acknowledgedNotice: some View {
    Label {
      Text(String(localized: "Acknowledged. Resolve when assistance is complete."))
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
    guard let latitude = snapshot.latitude, let longitude = snapshot.longitude else { return nil }
    return model.shoreDistanceText(gatewayIdentity: snapshot.gatewayIdentity, latitude: latitude, longitude: longitude)
  }
}

#Preview("Rescue") {
  NavigationStack {
    RescueView(model: AppModel(ble: BluetoothGatewayService()))
  }
  .modelContainer(for: [BoatSnapshot.self, MarineAlert.self], inMemory: true)
}
