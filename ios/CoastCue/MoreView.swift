import CoreLocation
import Observation
import SwiftData
import SwiftUI

struct MoreView: View {
  let model: AppModel

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: CoastCueStyle.sectionSpacing) {
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
          Text(String(localized: "Shore gateway"))
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(CoastCueStyle.muted)

          CoastCueCard {
            VStack(spacing: 0) {
              NavigationLink(value: AppRoute.diagnostics) {
                navigationRow(String(localized: "Diagnostics"), systemImage: "stethoscope")
              }
              .buttonStyle(.plain)
              rowDivider
              NavigationLink(value: AppRoute.gatewayRecords) {
                navigationRow(String(localized: "Boat reports"), systemImage: "tray.full")
              }
              .buttonStyle(.plain)
              rowDivider
              NavigationLink(value: AppRoute.shoreReference) {
                navigationRow(
                  String(localized: "Shore reference"), systemImage: "mappin.and.ellipse")
              }
              .buttonStyle(.plain)
              rowDivider
              NavigationLink(value: AppRoute.pairingHelp) {
                navigationRow(String(localized: "Pairing"), systemImage: "link")
              }
              .buttonStyle(.plain)
            }
          }
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
  }

  private var rowDivider: some View {
    Divider()
      .overlay(CoastCueStyle.line)
      .padding(.leading, 40)
  }

  private func navigationRow(_ title: String, systemImage: String) -> some View {
    HStack(spacing: 12) {
      Image(systemName: systemImage)
        .font(.title3)
        .foregroundStyle(CoastCueStyle.orange)
        .frame(minWidth: 28)
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
    List {
      Section(String(localized: "Gateway")) {
        LabeledContent(String(localized: "Firmware"), value: model.currentGatewayFirmware ?? "—")
        LabeledContent(
          String(localized: "Boat node UID"), value: model.currentGatewayStatus?.latestNodeUID ?? "—")
        LabeledContent(
          String(localized: "Gateway storage"),
          value: model.currentGatewayStatus?.storageReady.map {
            $0 ? String(localized: "Ready") : String(localized: "Unavailable")
          } ?? "—"
        )
        LabeledContent(
          String(localized: "Stored records"),
          value: model.currentGatewayStatus?.journalCount.map(String.init) ?? "—")
        LabeledContent(
          String(localized: "Radio health"), value: model.currentGatewayStatus?.radioHealth ?? "—")
        LabeledContent(
          String(localized: "Last packet age"),
          value: model.currentGatewayStatus?.lastPacketAgeMS.map { "\($0 / 1_000)s" } ?? "—")
        LabeledContent("RSSI", value: model.currentGatewayStatus?.rssi.map { "\($0) dBm" } ?? "—")
        LabeledContent("SNR", value: model.currentGatewayStatus?.snr.map { "\($0) dB" } ?? "—")
        LabeledContent(
          String(localized: "Pairing window"),
          value: model.currentGatewayStatus?.pairingOpen.map {
            $0 ? String(localized: "Pairing open") : String(localized: "Pairing closed")
          } ?? "—"
        )
      }

      Section {
        Text(
          String(
            localized:
              "CoastCue connects and syncs automatically while the app is open. Use Sync to retry when needed."
          )
        )
        .font(.footnote)
        .foregroundStyle(CoastCueStyle.muted)
      }
    }
    .scrollContentBackground(.hidden)
    .background(CoastCueStyle.canvas.ignoresSafeArea())
    .navigationTitle(String(localized: "Diagnostics"))
    .toolbar(.visible, for: .navigationBar)
    .navigationBarTitleDisplayMode(.inline)
  }
}

struct PairingHelpView: View {
  let model: AppModel
  @Environment(\.modelContext) private var context
  @State private var showingForgetGateway = false

  var body: some View {
    List {
      Section(String(localized: "Pair your iPhone")) {
        pairingStep(
          1,
          String(
            localized:
              "Hold the shore gateway button for 3 seconds to open its two-minute pairing window, then keep it near this iPhone."
          ))
        pairingStep(
          2,
          String(
            localized:
              "Open CoastCue and accept the iOS Bluetooth prompt; use Connect only if the automatic attempt needs a retry."
          ))
        pairingStep(
          3,
          String(
            localized:
              "Verify that the 6-digit passkey in the iOS prompt matches the shore gateway display."
          ))
        pairingStep(
          4,
          String(localized: "In CoastCue, confirm the gateway only after the two passkeys match."))
      }

      Section(String(localized: "Reset pairing")) {
        Text(
          String(
            localized:
              "To replace the gateway, forget it here, then hold its button for 10 seconds to clear the bond. Hold for 3 seconds to pair again."
          )
        )
        .foregroundStyle(CoastCueStyle.muted)
        Button(String(localized: "Forget shore gateway"), role: .destructive) {
          showingForgetGateway = true
        }
      }
    }
    .scrollContentBackground(.hidden)
    .background(CoastCueStyle.canvas.ignoresSafeArea())
    .navigationTitle(String(localized: "Pairing help"))
    .toolbar(.visible, for: .navigationBar)
    .navigationBarTitleDisplayMode(.inline)
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
      Text(
        String(
          localized:
            "Forgetting removes this gateway’s alerts and boat reports from this iPhone. It does not erase the shore gateway journal. Clear the old bond on the shore gateway before pairing a replacement."
        ))
    }
  }

  private func pairingStep(_ number: Int, _ text: String) -> some View {
    HStack(alignment: .top, spacing: 12) {
      Text(String(number))
        .font(.caption.weight(.bold))
        .foregroundStyle(CoastCueStyle.onAccent)
        .padding(6)
        .background(CoastCueStyle.ink)
        .clipShape(Circle())
      Text(text)
    }
    .padding(.vertical, 4)
  }
}

struct ShoreReferenceView: View {
  let model: AppModel
  @State private var locationCapture = ShoreReferenceLocationCapture()
  @State private var showingClearConfirmation = false

  var body: some View {
    List {
      Section(String(localized: "Fixed shore reference")) {
        referenceStatus
      }

      Section {
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
        }
        .disabled(model.gatewayIdentity == nil || locationCapture.isLocating)

        if let errorMessage = locationCapture.errorMessage {
          Label(errorMessage, systemImage: "exclamationmark.triangle.fill")
            .font(.footnote)
            .foregroundStyle(CoastCueStyle.emergency)
        }
      } footer: {
        Text(
          model.gatewayIdentity == nil
            ? String(localized: "Connect to a shore gateway before saving its reference point.")
            : String(localized: "Stand beside the fixed shore gateway, then save this iPhone’s current location. CoastCue uses it only for approximate straight-line distance.")
        )
      }

      if model.currentShoreReference != nil {
        Section {
          Button(String(localized: "Clear shore reference"), role: .destructive) {
            showingClearConfirmation = true
          }
        }
      }
    }
    .scrollContentBackground(.hidden)
    .background(CoastCueStyle.canvas.ignoresSafeArea())
    .navigationTitle(String(localized: "Shore reference"))
    .navigationBarTitleDisplayMode(.inline)
    .toolbar(.visible, for: .navigationBar)
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
    } message: {
      Text(String(localized: "Distance will stay hidden until a new shore reference is saved."))
    }
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
            "\(reference.latitude, format: .number.precision(.fractionLength(5))), \(reference.longitude, format: .number.precision(.fractionLength(5)))"
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
        VStack(alignment: .leading, spacing: 4) {
          Text(String(localized: "No shore reference saved"))
            .font(.headline)
            .foregroundStyle(CoastCueStyle.ink)
          Text(String(localized: "Boat distance remains hidden until this fixed point is saved."))
            .font(.footnote)
            .foregroundStyle(CoastCueStyle.muted)
        }
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
    guard location.horizontalAccuracy <= 100 else {
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
    List(results) { snapshot in
      VStack(alignment: .leading, spacing: 6) {
        Text(String(localized: "Radio report"))
          .font(.headline)
          .foregroundStyle(CoastCueStyle.ink)
        Text(
          String(
            format: String(localized: "Node UID: %@"),
            snapshot.nodeUID
          )
        )
          .font(.caption)
          .foregroundStyle(CoastCueStyle.muted)
        Text(snapshot.displayedTimestamp)
          .font(.subheadline)
          .foregroundStyle(CoastCueStyle.muted)
        if let latitude = snapshot.latitude, let longitude = snapshot.longitude {
          Label(
            "\(latitude, format: .number.precision(.fractionLength(5))), \(longitude, format: .number.precision(.fractionLength(5)))",
            systemImage: "location.fill"
          )
          .font(.caption)
          .foregroundStyle(CoastCueStyle.muted)
        }
        if !snapshot.hasReliableEventTime {
          Text(String(localized: "Gateway time unavailable"))
            .font(.caption)
            .foregroundStyle(CoastCueStyle.muted)
        }
      }
      .padding(.vertical, 4)
      .listRowBackground(CoastCueStyle.surface)
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
    .scrollContentBackground(.hidden)
    .background(CoastCueStyle.canvas.ignoresSafeArea())
    .navigationTitle(String(localized: "Boat reports"))
    .toolbar(.visible, for: .navigationBar)
    .navigationBarTitleDisplayMode(.inline)
  }
}
