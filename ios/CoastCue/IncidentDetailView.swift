import MapKit
import SwiftData
import SwiftUI
import UIKit

struct IncidentDetailView: View {
  let model: AppModel
  @Environment(\.dynamicTypeSize) private var dynamicTypeSize
  let gatewayIdentity: String
  let eventKey: String
  @Query private var alerts: [MarineAlert]
  @Query private var snapshots: [BoatSnapshot]
  @Environment(\.openURL) private var openURL
  @State private var copiedCoordinates = false

  private var alert: MarineAlert? {
    incidentAlert(gatewayIdentity: gatewayIdentity, eventKey: eventKey, alerts: alerts)
  }
  private var incidentSnapshot: BoatSnapshot? {
    snapshots.first(where: {
      $0.gatewayIdentity == gatewayIdentity && $0.eventKey == eventKey
    })
  }

  private var incidentLocation: BoatSnapshot? {
    incidentLocationSnapshot(
      gatewayIdentity: gatewayIdentity,
      eventKey: eventKey,
      snapshots: snapshots
    )
  }

  private var coordinate: CLLocationCoordinate2D? {
    guard let latitude = incidentLocation?.latitude,
          let longitude = incidentLocation?.longitude else { return nil }
    return CLLocationCoordinate2D(latitude: latitude, longitude: longitude)
  }

  private var distanceFromShoreText: String? {
    guard let coordinate else { return nil }
    return model.shoreDistanceText(
      gatewayIdentity: gatewayIdentity, latitude: coordinate.latitude, longitude: coordinate.longitude)
  }

  var body: some View {
    Page(String(localized: "Incident details")) {
      if let alert {
        incidentSummary(alert)
        locationSection
        technicalDetails
      } else {
        ContentUnavailableView(
          String(localized: "Incident unavailable"),
          systemImage: "exclamationmark.triangle",
          description: Text(
            String(localized: "This incident is no longer stored on this iPhone."))
        )
      }
    }
  }

  private func incidentSummary(_ alert: MarineAlert) -> some View {
    CoastCueCard {
      VStack(alignment: .leading, spacing: 12) {
        let layout = dynamicTypeSize.isAccessibilitySize
          ? AnyLayout(VStackLayout(alignment: .leading, spacing: 8))
          : AnyLayout(HStackLayout(spacing: 12))
        layout {
          Text(alert.displayTitle)
            .font(.title.weight(.bold))
            .foregroundStyle(CoastCueStyle.ink)
            .frame(maxWidth: .infinity, alignment: .leading)
          StatusBadge(status: alert.status)
        }
        Text(alert.displayedTimestamp)
          .font(.subheadline)
          .foregroundStyle(CoastCueStyle.muted)
      }
    }
  }

  @ViewBuilder
  private var locationSection: some View {
    if let coordinate {
      let mapsURL = URL(
        string:
          "https://maps.apple.com/?ll=\(coordinate.latitude),\(coordinate.longitude)&q=CoastCue")!

      CoastCueCard {
        VStack(alignment: .leading, spacing: 14) {
          Text(String(localized: "Incident position at report"))
            .font(.headline)
            .foregroundStyle(CoastCueStyle.ink)

          IncidentMap(coordinate: coordinate)
            .frame(height: 280)
            .clipShape(CoastCueStyle.cardShape(radius: CoastCueStyle.nestedRadius))
            .accessibilityLabel(String(localized: "Map showing the incident position"))

          Text(
            coordinateText(coordinate.latitude, coordinate.longitude)
          )
          .font(.system(.footnote, design: .monospaced))
          .foregroundStyle(CoastCueStyle.ink)
          .textSelection(.enabled)

          if let distanceFromShoreText {
            Label(distanceFromShoreText, systemImage: "arrow.left.and.right")
              .font(.subheadline.weight(.semibold))
              .foregroundStyle(CoastCueStyle.ink)
          }

          HStack(spacing: 8) {
            locationAction(
              title: copiedCoordinates ? String(localized: "Copied") : String(localized: "Copy"),
              icon: copiedCoordinates ? "checkmark" : "doc.on.doc"
            ) {
              UIPasteboard.general.string = "\(coordinate.latitude), \(coordinate.longitude)"
              copiedCoordinates = true
            }

            ShareLink(item: mapsURL) {
              locationActionLabel(title: String(localized: "Share"), icon: "square.and.arrow.up")
            }

            locationAction(
              title: String(localized: "Maps"),
              icon: "map"
            ) {
              openURL(mapsURL)
            }
          }
        }
      }
    } else {
      CoastCueCard {
        ContentUnavailableView(
          String(localized: "No valid GPS location"),
          systemImage: "location.slash",
          description: Text(
            String(localized: "This incident report does not include a valid GPS position."))
        )
      }
    }
  }

  private var technicalDetails: some View {
    CoastCueCard {
      VStack(alignment: .leading, spacing: 12) {
        Text(String(localized: "Radio details"))
          .font(.headline)
          .foregroundStyle(CoastCueStyle.ink)
        detailRow(
          String(localized: "GPS age at report"),
          value: incidentSnapshot?.gpsAgeSeconds.map { "\($0) s" } ?? "—")
        detailRow(
          String(localized: "Battery"),
          value: incidentSnapshot?.batteryPercent.map {
            "\($0.formatted(.number.precision(.fractionLength(0))))%"
          } ?? "—")
        detailRow(
          "RSSI",
          value: incidentSnapshot?.rssi.map { "\($0.formatted(.number.precision(.fractionLength(0)))) dBm" }
            ?? "—")
        detailRow(
          "SNR",
          value: incidentSnapshot?.snr.map { "\($0.formatted(.number.precision(.fractionLength(1)))) dB" }
            ?? "—")
      }
    }
  }

  private func detailRow(_ label: String, value: String) -> some View {
    HStack(alignment: .firstTextBaseline) {
      Text(label).foregroundStyle(CoastCueStyle.muted)
      Spacer()
      Text(value)
        .foregroundStyle(CoastCueStyle.ink)
        .multilineTextAlignment(.trailing)
    }
    .font(.subheadline)
  }

  private func locationAction(title: String, icon: String, action: @escaping () -> Void)
    -> some View
  {
    Button(action: action) {
      locationActionLabel(title: title, icon: icon)
    }
  }

  private func locationActionLabel(title: String, icon: String) -> some View {
    VStack(spacing: 6) {
      Image(systemName: icon).font(.headline)
      Text(title).font(.caption.weight(.semibold))
        .multilineTextAlignment(.center)
        .fixedSize(horizontal: false, vertical: true)
    }
    .foregroundStyle(CoastCueStyle.ink)
    .frame(maxWidth: .infinity)
    .padding(.vertical, 12)
    .frame(minHeight: 58)
    .background(CoastCueStyle.canvas)
    .clipShape(CoastCueStyle.cardShape(radius: CoastCueStyle.nestedRadius))
  }
}

/// The live incident map, as MKMapView rather than SwiftUI's Map: inside this scrolling page,
/// SwiftUI's Map made iOS treat the page as already scrolled, drawing a white navigation bar with a
/// rule even at the top. MKMapView pans and zooms the same without that side effect.
private struct IncidentMap: UIViewRepresentable {
  let coordinate: CLLocationCoordinate2D

  func makeUIView(context: Context) -> MKMapView {
    let map = MKMapView()
    map.region = MKCoordinateRegion(
      center: coordinate, span: MKCoordinateSpan(latitudeDelta: 0.035, longitudeDelta: 0.035))
    let pin = MKPointAnnotation()
    pin.coordinate = coordinate
    pin.title = String(localized: "Incident position")
    map.addAnnotation(pin)
    return map
  }

  func updateUIView(_ map: MKMapView, context: Context) {}
}
