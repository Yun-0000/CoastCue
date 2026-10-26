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
    guard let coordinate,
      let reference = model.shoreReference(for: gatewayIdentity),
      let meters = greatCircleDistanceMeters(
        fromLatitude: reference.latitude,
        fromLongitude: reference.longitude,
        toLatitude: coordinate.latitude,
        toLongitude: coordinate.longitude)
    else { return nil }
    return String(
      format: String(localized: "About %@ from shore reference"),
      formattedApproximateDistance(meters))
  }

  var body: some View {
    ScrollView {
      VStack(spacing: 16) {
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
      .padding(.horizontal, CoastCueStyle.pageHorizontalInset)
      .padding(.top, 8)
      .padding(.bottom, 28)
      .frame(maxWidth: 720)
      .frame(maxWidth: .infinity)
    }
    .background(CoastCueStyle.canvas.ignoresSafeArea())
    .navigationTitle(String(localized: "Incident details"))
    .toolbar(.visible, for: .navigationBar)
    .navigationBarTitleDisplayMode(.inline)
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

          Map(
            initialPosition: .region(
              .init(
                center: coordinate,
                span: .init(latitudeDelta: 0.035, longitudeDelta: 0.035)
              ))
          ) {
            Marker(String(localized: "Incident position"), coordinate: coordinate)
              .tint(CoastCueStyle.emergency)
          }
          .frame(height: 280)
          .clipShape(CoastCueStyle.cardShape(radius: CoastCueStyle.nestedRadius))
          .accessibilityLabel(String(localized: "Map showing the incident position"))

          Text(
            "\(coordinate.latitude, format: .number.precision(.fractionLength(5))), \(coordinate.longitude, format: .number.precision(.fractionLength(5)))"
          )
          .font(.system(.footnote, design: .monospaced))
          .foregroundStyle(CoastCueStyle.ink)
          .textSelection(.enabled)
          Text(String(localized: "Map tiles may be unavailable offline. Coordinates remain available."))
            .font(.caption)
            .foregroundStyle(CoastCueStyle.muted)

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
