import CoreLocation
import Observation
import SwiftData
import SwiftUI

struct MoreView: View {
  let model: AppModel

  var body: some View {
    Page(String(localized: "More"), isTabRoot: true) {
      HStack(spacing: 10) {
        Image(systemName: "lifepreserver.fill")
          .font(.title2)
          .foregroundStyle(CoastCueStyle.orange)
          .padding(8)
          .background(CoastCueStyle.surface)
          .overlay {
            CoastCueStyle.cardShape(radius: CoastCueStyle.iconWellRadius)
              .stroke(CoastCueStyle.line, lineWidth: 1)
          }
          .clipShape(CoastCueStyle.cardShape(radius: CoastCueStyle.iconWellRadius))
        Text("CoastCue")
          .font(.title2.weight(.bold))
          .foregroundStyle(CoastCueStyle.ink)
      }

      ConnectionStrip(model: model)

      VStack(alignment: .leading, spacing: 10) {
        CardSectionTitle(String(localized: "Shore gateway"))

        CardRows(
          [
            (String(localized: "Diagnostics"), "stethoscope", AppRoute.diagnostics),
            (String(localized: "Boat reports"), "tray.full", AppRoute.gatewayRecords),
            (String(localized: "Shore reference"), "mappin.and.ellipse", AppRoute.shoreReference),
            (String(localized: "Pairing"), "link", AppRoute.pairingHelp),
          ].map { title, icon, route in
            NavigationLink(value: route) { navigationRow(title, systemImage: icon) }
              .buttonStyle(.plain)
          },
          dividerLeading: 40)  // icon 28 + spacing 12
      }
    }
  }

  private func navigationRow(_ title: String, systemImage: String) -> some View {
    HStack(spacing: 12) {
      Image(systemName: systemImage)
        .font(.title3)
        .foregroundStyle(CoastCueStyle.orange)
        .frame(width: 28)
      Text(title)
        .foregroundStyle(CoastCueStyle.ink)
      Spacer()
      Image(systemName: "chevron.right")
        .font(.caption.weight(.bold))
        .foregroundStyle(CoastCueStyle.muted)
    }
    .frame(minHeight: 52)
    .contentShape(Rectangle())
  }
}

struct DiagnosticsView: View {
  let model: AppModel

  var body: some View {
    Page(String(localized: "Diagnostics")) {
      CardRows(rows.map { row in
        HStack {
          Text(row.label).foregroundStyle(CoastCueStyle.ink)
          Spacer()
          Text(row.value).foregroundStyle(CoastCueStyle.muted)
        }
        .frame(minHeight: 44)
      })
    }
  }

  private var rows: [(label: String, value: String)] {
    let status = model.currentGatewayStatus
    return [
      (String(localized: "Firmware"), model.currentGatewayFirmware ?? "—"),
      (String(localized: "Boat node UID"), status?.latestNodeUID ?? "—"),
      (String(localized: "Gateway storage"),
       status?.storageReady.map { $0 ? String(localized: "Ready") : String(localized: "Unavailable") } ?? "—"),
      (String(localized: "Stored records"), status?.journalCount.map(String.init) ?? "—"),
      (String(localized: "Radio health"), status?.radioHealth ?? "—"),
      (String(localized: "Last packet age"), status?.lastPacketAgeMS.map { "\($0 / 1_000) s" } ?? "—"),
      ("RSSI", status?.rssi.map { "\($0.formatted(.number.precision(.fractionLength(0)))) dBm" } ?? "—"),
      ("SNR", status?.snr.map { "\($0.formatted(.number.precision(.fractionLength(1)))) dB" } ?? "—"),
      (String(localized: "Pairing window"),
       status?.pairingOpen.map { $0 ? String(localized: "Pairing open") : String(localized: "Pairing closed") } ?? "—"),
    ]
  }
}

struct PairingHelpView: View {
  let model: AppModel
  @Environment(\.modelContext) private var context
  @State private var showingForgetGateway = false

  private let steps = [
    String(localized: "Hold the shore gateway button for 3 seconds."),
    String(localized: "Open CoastCue and accept the Bluetooth prompt."),
    String(localized: "Enter the 6-digit code shown on the shore."),
    String(localized: "Confirm the gateway in CoastCue."),
  ]

  var body: some View {
    Page(String(localized: "Pairing")) {
      VStack(alignment: .leading, spacing: 10) {
        CardSectionTitle(String(localized: "Pair your iPhone"))
        CardRows(
          steps.enumerated().map { index, step in
            pairingStep(index + 1, step).padding(.vertical, 12)
          }, dividerLeading: 34)
      }
      CardRows([
        Button(String(localized: "Forget shore gateway"), role: .destructive) {
          showingForgetGateway = true
        }
        .foregroundStyle(CoastCueStyle.emergency)
        .frame(minHeight: 48)
      ])
    }
    .confirmationDialog(
      String(localized: "Forget the saved shore gateway?"),
      isPresented: $showingForgetGateway,
      titleVisibility: .visible
    ) {
      Button(String(localized: "Forget shore gateway"), role: .destructive) {
        model.forgetGateway(context: context)
      }
      Button(String(localized: "Cancel"), role: .cancel) {}
    } message: {
      Text(String(localized: "Removes this gateway’s alerts and reports from this iPhone."))
    }
  }

  private func pairingStep(_ number: Int, _ text: String) -> some View {
    HStack(alignment: .top, spacing: 12) {
      Text(String(number))
        .font(.caption.weight(.bold))
        .foregroundStyle(CoastCueStyle.onAccent)
        .frame(width: 22, height: 22)  // same circle for every digit
        .background(CoastCueStyle.orange)
        .clipShape(Circle())
      Text(text).foregroundStyle(CoastCueStyle.ink)
    }
  }
}

struct ShoreReferenceView: View {
  let model: AppModel
  @State private var locationCapture = ShoreReferenceLocationCapture()
  @State private var showingClearConfirmation = false

  var body: some View {
    Page(String(localized: "Shore reference")) {
      CoastCueCard { referenceStatus }

      VStack(alignment: .leading, spacing: 8) {
        CardRows([captureButton])
        // The hint under the button; a capture error takes its place.
        if let errorMessage = locationCapture.errorMessage {
          Label(errorMessage, systemImage: "exclamationmark.triangle.fill")
            .font(.footnote)
            .foregroundStyle(CoastCueStyle.emergency)
            .padding(.horizontal, 16)
        } else {
          Text(
            model.gatewayIdentity == nil
              ? String(localized: "Connect to a shore gateway first.")
              : String(localized: "Stand at the shore gateway, then save this location.")
          )
          .font(.footnote)
          .foregroundStyle(CoastCueStyle.muted)
          .padding(.horizontal, 16)
        }
      }

      if model.currentShoreReference != nil {
        CardRows([
          Button(String(localized: "Clear shore reference"), role: .destructive) {
            showingClearConfirmation = true
          }
          .foregroundStyle(CoastCueStyle.emergency)
          .frame(minHeight: 48)
        ])
      }
    }
    .onChange(of: locationCapture.capturedReference) { _, reference in
      guard let reference else { return }
      model.saveShoreReference(reference)
    }
    .confirmationDialog(
      String(localized: "Clear the saved shore reference?"),
      isPresented: $showingClearConfirmation,
      titleVisibility: .visible
    ) {
      Button(String(localized: "Clear shore reference"), role: .destructive) {
        model.clearCurrentShoreReference()
      }
      Button(String(localized: "Cancel"), role: .cancel) {}
    }
  }

  private var captureButton: some View {
    Button {
      locationCapture.capture()
    } label: {
      HStack(spacing: 10) {
        if locationCapture.isLocating {
          ProgressView()
        } else {
          Image(systemName: "location.fill")
        }
        Text(
          model.currentShoreReference == nil
            ? String(localized: "Use this iPhone’s location")
            : String(localized: "Update from this iPhone’s location")
        )
      }
      .foregroundStyle(CoastCueStyle.orange)
      .frame(minHeight: 48)
    }
    .disabled(model.gatewayIdentity == nil || locationCapture.isLocating)
  }

  @ViewBuilder
  private var referenceStatus: some View {
    if let reference = model.currentShoreReference {
      HStack(alignment: .top, spacing: 12) {
        Image(systemName: "mappin.circle.fill")
          .font(.title2)
          .foregroundStyle(CoastCueStyle.orange)
          .frame(minWidth: 30)
        VStack(alignment: .leading, spacing: 5) {
          Text(String(localized: "Shore reference saved"))
            .font(.headline)
            .foregroundStyle(CoastCueStyle.ink)
          Text(
            coordinateText(reference.latitude, reference.longitude)
          )
          .font(.system(.footnote, design: .monospaced))
          Text(
            String(
              format: String(localized: "Saved %@ · accuracy ±%@ m"),
              reference.capturedAt.formatted(date: .abbreviated, time: .shortened),
              reference.horizontalAccuracyMeters.formatted(
                .number.precision(.fractionLength(0))))
          )
          .font(.caption)
          .foregroundStyle(CoastCueStyle.muted)
        }
      }
      .padding(.vertical, 4)
    } else {
      HStack(alignment: .top, spacing: 12) {
        Image(systemName: "mappin.slash")
          .font(.title2)
          .foregroundStyle(CoastCueStyle.muted)
          .frame(minWidth: 30)
        Text(String(localized: "No shore reference saved"))
          .font(.headline)
          .foregroundStyle(CoastCueStyle.ink)
      }
      .padding(.vertical, 4)
    }
  }
}

@MainActor
@Observable
private final class ShoreReferenceLocationCapture: NSObject, @preconcurrency CLLocationManagerDelegate {
  private let manager = CLLocationManager()
  private var waitingForAuthorization = false

  private(set) var capturedReference: ShoreReference?
  private(set) var isLocating = false
  private(set) var errorMessage: String?

  override init() {
    super.init()
    manager.delegate = self
    manager.desiredAccuracy = kCLLocationAccuracyBest
    manager.activityType = .other
  }

  func capture() {
    capturedReference = nil
    errorMessage = nil
    guard CLLocationManager.locationServicesEnabled() else {
      fail(String(localized: "Location Services are unavailable on this iPhone."))
      return
    }

    isLocating = true
    switch manager.authorizationStatus {
    case .authorizedAlways, .authorizedWhenInUse:
      manager.requestLocation()
    case .notDetermined:
      waitingForAuthorization = true
      manager.requestWhenInUseAuthorization()
    case .denied, .restricted:
      fail(String(localized: "Allow location access in Settings to save the shore reference."))
    @unknown default:
      fail(String(localized: "The iPhone location permission could not be determined."))
    }
  }

  func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
    guard waitingForAuthorization else { return }
    switch manager.authorizationStatus {
    case .authorizedAlways, .authorizedWhenInUse:
      waitingForAuthorization = false
      manager.requestLocation()
    case .denied, .restricted:
      waitingForAuthorization = false
      fail(String(localized: "Allow location access in Settings to save the shore reference."))
    case .notDetermined:
      break
    @unknown default:
      waitingForAuthorization = false
      fail(String(localized: "The iPhone location permission could not be determined."))
    }
  }

  func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
    let recentCutoff = Date.now.addingTimeInterval(-60)
    let candidates = locations.filter {
      $0.horizontalAccuracy >= 0 && $0.timestamp >= recentCutoff
    }
    guard let location = candidates.min(by: { $0.horizontalAccuracy < $1.horizontalAccuracy })
    else {
      fail(String(localized: "No recent iPhone location was available. Try again outdoors."))
      return
    }
    guard location.horizontalAccuracy <= ShoreReference.maximumHorizontalAccuracyMeters else {
      fail(String(localized: "Location accuracy is too low. Move outdoors and try again."))
      return
    }

    capturedReference = ShoreReference(
      latitude: location.coordinate.latitude,
      longitude: location.coordinate.longitude,
      capturedAt: location.timestamp,
      horizontalAccuracyMeters: location.horizontalAccuracy
    )
    isLocating = false
    errorMessage = nil
  }

  func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
    fail(String(localized: "The iPhone location could not be read. Try again."))
  }

  private func fail(_ message: String) {
    isLocating = false
    errorMessage = message
  }
}

struct GatewayRecordsView: View {
  let gatewayIdentity: String?
  @Query(sort: \BoatSnapshot.receivedAt, order: .reverse) private var snapshots: [BoatSnapshot]
  @State private var search = ""

  private var results: [BoatSnapshot] {
    boatSnapshotsForCurrentGateway(
      gatewayIdentity: gatewayIdentity,
      snapshots: snapshots
    ).filter {
      search.isEmpty || $0.nodeUID.localizedCaseInsensitiveContains(search)
    }
  }

  var body: some View {
    Page(String(localized: "Boat reports")) {
      if !results.isEmpty { CardRows(results.map(reportRow)) }
    }
    .searchable(text: $search, prompt: String(localized: "Search node UID"))
    .overlay {
      if results.isEmpty && !search.isEmpty {
        ContentUnavailableView.search(text: search)
      } else if results.isEmpty {
        ContentUnavailableView(
          String(localized: "No gateway records"),
          systemImage: "tray",
          description: Text(
            String(localized: "Connect and sync to save radio records on this iPhone."))
        )
      }
    }
  }

  private func reportRow(_ snapshot: BoatSnapshot) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      Text(snapshot.displayedTimestamp)
        .font(.headline)
        .foregroundStyle(CoastCueStyle.ink)
      Text(String(format: String(localized: "Node UID: %@"), snapshot.nodeUID))
        .font(.caption)
        .foregroundStyle(CoastCueStyle.muted)
      if let latitude = snapshot.latitude, let longitude = snapshot.longitude {
        Label(coordinateText(latitude, longitude), systemImage: "location.fill")
          .font(.caption)
          .foregroundStyle(CoastCueStyle.muted)
      }
      if !snapshot.hasReliableEventTime {
        Text(String(localized: "Gateway time unavailable"))
          .font(.caption)
          .foregroundStyle(CoastCueStyle.muted)
      }
    }
    .padding(.vertical, 12)
  }
}
