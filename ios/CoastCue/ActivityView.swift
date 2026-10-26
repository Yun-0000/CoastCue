import SwiftData
import SwiftUI

struct ActivityView: View {
  let model: AppModel
  @Environment(\.dynamicTypeSize) private var dynamicTypeSize
  @Query(sort: \MarineAlert.createdAt, order: .reverse) private var alerts: [MarineAlert]

  private var activeAlerts: [MarineAlert] {
    activeAlertsForCurrentGateway(gatewayIdentity: model.gatewayIdentity, alerts: alerts)
  }
  private var pastAlerts: [MarineAlert] {
    pastAlertsForCurrentGateway(gatewayIdentity: model.gatewayIdentity, alerts: alerts)
  }

  var body: some View {
    ScrollView {
      LazyVStack(alignment: .leading, spacing: CoastCueStyle.sectionSpacing) {
        CoastCuePageHeader(title: String(localized: "Activity"))

        if activeAlerts.isEmpty && pastAlerts.isEmpty {
          CoastCueCard {
            ContentUnavailableView(
              String(localized: "No saved activity"),
              systemImage: "clock.arrow.circlepath",
              description: Text(String(localized: "Rescue events appear here after a gateway sync."))
            )
            .frame(maxWidth: .infinity, minHeight: 220)
          }
        } else {
          if !activeAlerts.isEmpty {
            activitySection(
              String(localized: "Active incidents"),
              alerts: activeAlerts
            )
          }

          if !pastAlerts.isEmpty {
            activitySection(
              String(localized: "Past activity"),
              alerts: pastAlerts
            )
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

  private func activitySection(_ title: String, alerts: [MarineAlert]) -> some View {
    VStack(alignment: .leading, spacing: 10) {
      Text(title)
        .font(.subheadline.weight(.semibold))
        .foregroundStyle(CoastCueStyle.muted)

      CoastCueCard {
        LazyVStack(spacing: 0) {
          ForEach(Array(alerts.enumerated()), id: \.element.id) { index, alert in
            activityLink(alert)
            if index < alerts.count - 1 {
              Divider()
                .overlay(CoastCueStyle.line)
            }
          }
        }
      }
    }
  }

  private func activityLink(_ alert: MarineAlert) -> some View {
    NavigationLink(value: AppRoute.incident(alert.gatewayIdentity, alert.eventKey)) {
      HStack(spacing: 12) {
        VStack(alignment: .leading, spacing: 6) {
          let layout = dynamicTypeSize.isAccessibilitySize
            ? AnyLayout(VStackLayout(alignment: .leading, spacing: 6))
            : AnyLayout(HStackLayout(spacing: 8))
          layout {
            Text(alert.displayTitle)
              .font(.headline)
              .foregroundStyle(CoastCueStyle.ink)
            StatusBadge(status: alert.status)
          }
          Text(alert.displayedTimestamp)
            .font(.caption)
            .foregroundStyle(CoastCueStyle.muted)
            .fixedSize(horizontal: false, vertical: true)
        }
        Spacer(minLength: 8)
        Image(systemName: "chevron.right")
          .font(.caption.weight(.bold))
          .foregroundStyle(CoastCueStyle.muted)
      }
      .padding(.vertical, 10)
      .frame(minHeight: 56)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }
}
