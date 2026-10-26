import SwiftData
import SwiftUI

enum AppTab: String, CaseIterable, Identifiable {
  case rescue, activity, more

  var id: String { rawValue }

  var label: Label<Text, Image> {
    switch self {
    case .rescue:
      Label(String(localized: "Rescue"), systemImage: "lifepreserver.fill")
    case .activity:
      Label(String(localized: "Activity"), systemImage: "clock.arrow.circlepath")
    case .more:
      Label(String(localized: "More"), systemImage: "ellipsis")
    }
  }
}

enum AppRoute: Hashable {
  case incident(String, String)
  case diagnostics
  case gatewayRecords
  case shoreReference
  case pairingHelp
}

enum CoastCueStyle {
  /// Compact root inset after large-title removal (safe-area breathing room, not leftover void).
  static let rootTopPadding: CGFloat = 16
  static let pageHorizontalInset: CGFloat = 20
  static let sectionSpacing: CGFloat = 20
  static let cardRadius: CGFloat = 18
  static let nestedRadius: CGFloat = 12
  static let buttonRadius: CGFloat = 14
  static let badgeRadius: CGFloat = 8
  static let iconWellRadius: CGFloat = 12

  static let canvas = adaptive(UIColor(red: 0.94, green: 0.94, blue: 0.93, alpha: 1), .systemGroupedBackground)
  static let surface = adaptive(UIColor(red: 0.985, green: 0.985, blue: 0.98, alpha: 1), .secondarySystemGroupedBackground)
  static let ink = Color.primary
  static let muted = Color.secondary
  static let line = Color(uiColor: .separator)
  static let orange = adaptive(UIColor(red: 0.70, green: 0.24, blue: 0.06, alpha: 1), UIColor(red: 1, green: 0.62, blue: 0.34, alpha: 1))
  static let onAccent = adaptive(.white, .black)
  static let emergency = adaptive(UIColor(red: 0.73, green: 0.13, blue: 0.10, alpha: 1), .systemRed)
  static let success = adaptive(UIColor(red: 0.13, green: 0.47, blue: 0.31, alpha: 1), .systemGreen)

  private static func adaptive(_ light: UIColor, _ dark: UIColor) -> Color {
    Color(uiColor: UIColor { $0.userInterfaceStyle == .dark ? dark : light })
  }

  static func cardShape(radius: CGFloat = cardRadius) -> RoundedRectangle {
    RoundedRectangle(cornerRadius: radius, style: .continuous)
  }
}

/// Compact page title used on tab roots after large titles were removed.
struct CoastCuePageHeader: View {
  let title: String

  var body: some View {
    Text(title)
      .font(.title.weight(.bold))
      .foregroundStyle(CoastCueStyle.ink)
      .frame(maxWidth: .infinity, alignment: .leading)
      .accessibilityAddTraits(.isHeader)
  }
}

extension BluetoothGatewayService.ConnectionState {
  var displayName: String {
    switch self {
    case .idle: String(localized: "Disconnected")
    case .scanning: String(localized: "Looking for gateway")
    case .connecting: String(localized: "Connecting")
    case .confirming: String(localized: "Confirm gateway")
    case .connected: String(localized: "Connected")
    case .syncing: String(localized: "Syncing")
    case .syncFailed: String(localized: "Sync failed")
    case .unavailable: String(localized: "Bluetooth unavailable")
    }
  }

  var isConnected: Bool {
    self == .connected || self == .syncing || self == .syncFailed
  }
  var isBusy: Bool {
    self == .scanning || self == .connecting || self == .confirming || self == .syncing
  }
}

@MainActor
struct AppShell: View {
  let model: AppModel
  @Environment(\.modelContext) private var context
  @Environment(\.scenePhase) private var scenePhase
  @State private var tab: AppTab = .rescue
  @State private var rescuePath: [AppRoute] = []
  @State private var activityPath: [AppRoute] = []
  @State private var morePath: [AppRoute] = []
  @State private var didStartForegroundSync = false

  var body: some View {
    @Bindable var model = model
    TabView(selection: $tab) {
      NavigationStack(path: $rescuePath) {
        RescueView(model: model)
          .navigationDestination(for: AppRoute.self, destination: destination)
      }
      .tabItem { AppTab.rescue.label }
      .tag(AppTab.rescue)

      NavigationStack(path: $activityPath) {
        ActivityView(model: model)
          .navigationDestination(for: AppRoute.self, destination: destination)
      }
      .tabItem { AppTab.activity.label }
      .tag(AppTab.activity)

      NavigationStack(path: $morePath) {
        MoreView(model: model)
          .navigationDestination(for: AppRoute.self, destination: destination)
      }
      .tabItem { AppTab.more.label }
      .tag(AppTab.more)
    }
    .tint(CoastCueStyle.orange)
    .sheet(item: $model.enrollmentCandidate) { candidate in
      GatewayEnrollmentConfirmationSheet(model: model, candidate: candidate)
        .interactiveDismissDisabled()
    }
    .alert(
      String(localized: "Gateway issue"),
      isPresented: Binding(
        get: { model.lastError != nil },
        set: { if !$0 { model.lastError = nil } }
      )
    ) {
      Button(String(localized: "OK"), role: .cancel) { model.lastError = nil }
    } message: {
      Text(model.lastError ?? "")
    }
    .task {
      guard !didStartForegroundSync else { return }
      didStartForegroundSync = true
      model.connectAndSync(context: context)
    }
    .onChange(of: scenePhase) { oldPhase, newPhase in
      guard didStartForegroundSync,
        oldPhase != .active,
        newPhase == .active,
        !model.connectionState.isBusy
      else { return }
      model.connectAndSync(context: context)
    }
  }

  @ViewBuilder
  private func destination(_ route: AppRoute) -> some View {
    switch route {
    case .incident(let gatewayIdentity, let eventKey):
      IncidentDetailView(model: model, gatewayIdentity: gatewayIdentity, eventKey: eventKey)
    case .diagnostics:
      DiagnosticsView(model: model)
    case .gatewayRecords:
      GatewayRecordsView(gatewayIdentity: model.gatewayIdentity)
    case .shoreReference:
      ShoreReferenceView(model: model)
    case .pairingHelp:
      PairingHelpView(model: model)
    }
  }
}

struct GatewayEnrollmentConfirmationSheet: View {
  @Environment(\.dismiss) private var dismiss
  let model: AppModel
  let candidate: GatewayEnrollmentCandidate

  var body: some View {
    NavigationStack {
      ScrollView {
        VStack(alignment: .leading, spacing: 20) {
          Label(String(localized: "Confirm shore gateway pairing"), systemImage: "checkmark.shield")
            .font(.title3.weight(.semibold))
            .foregroundStyle(CoastCueStyle.ink)

          Text(
            String(
              localized:
                "Confirm only if the iOS Bluetooth pairing prompt appeared and its 6-digit passkey matched the code on this shore gateway’s SH1106 display."
            )
          )
          .foregroundStyle(CoastCueStyle.muted)

          VStack(alignment: .leading, spacing: 8) {
            LabeledContent(String(localized: "Gateway node"), value: candidate.info.nodeUID)
            LabeledContent(String(localized: "Journal epoch"), value: candidate.info.journalEpoch)
          }
          .font(.subheadline)
          .padding(14)
          .frame(maxWidth: .infinity, alignment: .leading)
          .background(CoastCueStyle.canvas)
          .clipShape(CoastCueStyle.cardShape(radius: CoastCueStyle.nestedRadius))

          Button(String(localized: "Confirm matching passkey")) {
            model.confirmEnrollment(candidate)
            if model.enrollmentCandidate == nil {
              dismiss()
            }
          }
          .buttonStyle(PrimaryRescueButtonStyle())
          .accessibilityLabel(String(localized: "Confirm matching shore gateway passkey"))

          Button(String(localized: "Cancel pairing"), role: .cancel) {
            model.cancelEnrollment(candidate)
            dismiss()
          }
          .frame(maxWidth: .infinity, minHeight: 44)
          .accessibilityLabel(String(localized: "Cancel shore gateway pairing"))
        }
        .padding(20)
        .frame(maxWidth: 720)
        .frame(maxWidth: .infinity)
      }
      .background(CoastCueStyle.canvas)
      .navigationTitle(String(localized: "Pair shore gateway"))
      .navigationBarTitleDisplayMode(.inline)
    }
  }
}

struct CoastCueCard<Content: View>: View {
  @ViewBuilder let content: Content

  var body: some View {
    content
      .frame(maxWidth: .infinity, alignment: .leading)
      .padding(16)
      .background(CoastCueStyle.surface)
      .overlay {
        CoastCueStyle.cardShape()
          .stroke(CoastCueStyle.line, lineWidth: 1)
      }
      .clipShape(CoastCueStyle.cardShape())
  }
}

struct ConnectionStrip: View {
  let model: AppModel
  @Environment(\.modelContext) private var context

  var body: some View {
    CoastCueCard {
      ViewThatFits(in: .horizontal) {
        HStack(alignment: .center, spacing: 16) {
          summary
          Spacer(minLength: 0)
          connectionButton
        }
        VStack(alignment: .leading, spacing: 12) {
          summary
          connectionButton
        }
      }
    }
  }

  private var summary: some View {
    HStack(alignment: .top, spacing: 10) {
      if model.connectionState.isBusy {
        ProgressView().controlSize(.small).padding(.top, 3)
          .accessibilityHidden(true)
      } else {
        Image(systemName: model.hasCurrentGatewaySnapshot ? "checkmark.circle.fill" : "antenna.radiowaves.left.and.right.slash")
          .foregroundStyle(model.hasCurrentGatewaySnapshot ? CoastCueStyle.success : CoastCueStyle.muted)
          .padding(.top, 2)
          .accessibilityHidden(true)
      }
      VStack(alignment: .leading, spacing: 4) {
        Text(model.isGatewayStatusStale ? String(localized: "Gateway status is out of date") : model.connectionState.displayName)
          .font(.subheadline.weight(.semibold))
          .foregroundStyle(CoastCueStyle.ink)
        Text(model.isGatewayStatusStale ? String(localized: "Tap Sync to refresh") :
          model.hasCompletedCurrentSync ? String(localized: "Sync complete") : model.syncMessage)
          .font(.caption)
          .foregroundStyle(CoastCueStyle.muted)
        if model.isHistoryTruncated {
          Label(String(localized: "Gateway history is truncated"), systemImage: "exclamationmark.triangle")
            .font(.caption)
            .foregroundStyle(CoastCueStyle.orange)
        }
      }
      .fixedSize(horizontal: false, vertical: true)
    }
    .accessibilityElement(children: .combine)
  }

  private var connectionButton: some View {
    Button {
      if model.connectionState.isBusy { model.disconnect() }
      else { model.connectAndSync(context: context) }
    } label: {
      Text(model.connectionState.isBusy ? String(localized: "Cancel") :
        model.connectionState.isConnected ? String(localized: "Sync") : String(localized: "Connect"))
      .font(.subheadline.weight(.semibold))
      .fixedSize()
      .frame(minHeight: 44)
    }
    .buttonStyle(.plain)
    .foregroundStyle(CoastCueStyle.orange)
    .padding(.horizontal, 8)
    .accessibilityLabel(model.connectionState.isBusy ? String(localized: "Cancel connection or sync") : String(localized: "Connect to shore gateway and sync records"))
  }
}

struct StatusBadge: View {
  let status: AlertStatus

  var body: some View {
    Text(status.label)
      .font(.caption.weight(.semibold))
      .fixedSize(horizontal: false, vertical: true)
      .foregroundStyle(color)
      .padding(.horizontal, 8)
      .padding(.vertical, 5)
      .background(color.opacity(0.09))
      .clipShape(CoastCueStyle.cardShape(radius: CoastCueStyle.badgeRadius))
  }

  private var color: Color {
    switch status {
    case .open: CoastCueStyle.emergency
    case .acknowledged: CoastCueStyle.orange
    case .resolved: CoastCueStyle.success
    case .superseded: CoastCueStyle.muted
    }
  }
}

struct PrimaryRescueButtonStyle: ButtonStyle {
  @Environment(\.isEnabled) private var isEnabled
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .font(.headline)
      .foregroundStyle(CoastCueStyle.onAccent)
      .tint(CoastCueStyle.onAccent)
      .multilineTextAlignment(.center)
      .padding(.horizontal, 16)
      .padding(.vertical, 14)
      .frame(maxWidth: .infinity, minHeight: 52)
      .background(CoastCueStyle.orange)
      .brightness(configuration.isPressed ? -0.08 : 0)
      .opacity(isEnabled ? 1 : 0.5)
      .clipShape(CoastCueStyle.cardShape(radius: CoastCueStyle.buttonRadius))
  }
}

struct ResolveButtonStyle: ButtonStyle {
  @Environment(\.isEnabled) private var isEnabled
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .font(.headline)
      .foregroundStyle(CoastCueStyle.emergency)
      .tint(CoastCueStyle.emergency)
      .multilineTextAlignment(.center)
      .padding(.horizontal, 16)
      .padding(.vertical, 14)
      .frame(maxWidth: .infinity, minHeight: 52)
      .opacity(isEnabled ? 1 : 0.5)
      .background(CoastCueStyle.surface)
      .overlay {
        CoastCueStyle.cardShape(radius: CoastCueStyle.buttonRadius)
          .stroke(CoastCueStyle.emergency.opacity(configuration.isPressed ? 1 : 0.65), lineWidth: 1)
      }
      .clipShape(CoastCueStyle.cardShape(radius: CoastCueStyle.buttonRadius))
  }
}
