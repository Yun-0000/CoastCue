import Foundation
import SwiftData

enum AlertStatus: String, Codable, CaseIterable, Identifiable {
    case open, acknowledged, resolved, superseded
    var id: String { rawValue }

    var label: String {
        switch self {
        case .open: String(localized: "Open")
        case .acknowledged: String(localized: "Acknowledged")
        case .resolved: String(localized: "Resolved")
        case .superseded: String(localized: "Superseded")
        }
    }

}

/// A shore position captured on this iPhone for a specific gateway.
///
/// This remains a value type rather than a SwiftData model because it is a
/// small piece of gateway-local configuration, not shared event history.
struct ShoreReference: Codable, Equatable {
    static let maximumHorizontalAccuracyMeters = 100.0

    let latitude: Double
    let longitude: Double
    let capturedAt: Date
    let horizontalAccuracyMeters: Double

    init?(
        latitude: Double,
        longitude: Double,
        capturedAt: Date,
        horizontalAccuracyMeters: Double
    ) {
        guard isValidCoordinate(latitude: latitude, longitude: longitude),
              capturedAt.timeIntervalSinceReferenceDate.isFinite,
              horizontalAccuracyMeters.isFinite,
              (0...Self.maximumHorizontalAccuracyMeters).contains(horizontalAccuracyMeters) else {
            return nil
        }
        self.latitude = latitude
        self.longitude = longitude
        self.capturedAt = capturedAt
        self.horizontalAccuracyMeters = horizontalAccuracyMeters
    }

    private enum CodingKeys: String, CodingKey {
        case latitude
        case longitude
        case capturedAt
        case horizontalAccuracyMeters
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        let latitude = try container.decode(Double.self, forKey: .latitude)
        let longitude = try container.decode(Double.self, forKey: .longitude)
        let capturedAt = try container.decode(Date.self, forKey: .capturedAt)
        let horizontalAccuracyMeters = try container.decode(Double.self, forKey: .horizontalAccuracyMeters)
        guard let reference = Self(
            latitude: latitude,
            longitude: longitude,
            capturedAt: capturedAt,
            horizontalAccuracyMeters: horizontalAccuracyMeters
        ) else {
            throw DecodingError.dataCorruptedError(
                forKey: .latitude,
                in: container,
                debugDescription: "Shore reference has invalid coordinates or accuracy."
            )
        }
        self = reference
    }
}

@Model
final class BoatSnapshot {
    @Attribute(.unique) var eventKey: String
    var recordID: Int64
    var nodeUID: String
    var boatID: String?
    var gatewayIdentity: String = "unscoped"
    var nonce: String
    var receivedAt: Date
    var occurredAt: Date?
    /// `receivedAt` remains only as a fallback sort key for records without an
    /// explicit event time. New UI uses `occurredAt` and `syncedAt` separately.
    var syncedAt: Date = Date.now
    var timeQuality: String
    var latitude: Double?
    var longitude: Double?
    var gpsAgeSeconds: Int64?
    /// The boat-created LoRa Incident ID, when this report belongs to an Incident.
    /// Optional so existing SwiftData stores migrate without inventing a value.
    var incidentID: String?
    var batteryPercent: Double?
    var rssi: Double?
    var snr: Double?
    var payload: String
    var durable: Bool = true

    init(eventKey: String, gatewayIdentity: String = "unscoped", recordID: Int64, nodeUID: String, boatID: String? = nil, nonce: String, occurredAt: Date? = nil, syncedAt: Date = .now, timeQuality: String, latitude: Double?, longitude: Double?, gpsAgeSeconds: Int64? = nil, incidentID: String? = nil, batteryPercent: Double?, rssi: Double?, snr: Double?, payload: String, durable: Bool = true) {
        self.eventKey = eventKey
        self.gatewayIdentity = gatewayIdentity
        self.recordID = recordID
        self.nodeUID = nodeUID
        self.boatID = boatID
        self.nonce = nonce
        self.receivedAt = occurredAt ?? syncedAt
        self.occurredAt = occurredAt
        self.syncedAt = syncedAt
        self.timeQuality = timeQuality
        self.latitude = latitude
        self.longitude = longitude
        self.gpsAgeSeconds = gpsAgeSeconds
        self.incidentID = incidentID
        self.batteryPercent = batteryPercent
        self.rssi = rssi
        self.snr = snr
        self.payload = payload
        self.durable = durable
    }
}

@Model
final class MarineAlert {
    @Attribute(.unique) var eventKey: String
    var recordID: Int64
    var nodeUID: String
    var boatID: String?
    var gatewayIdentity: String = "unscoped"
    var title: String
    var detail: String
    var createdAt: Date
    var occurredAt: Date?
    /// Kept beside `createdAt` for existing local stores; use the explicit fields below.
    var syncedAt: Date = Date.now
    var statusRaw: String
    /// The boat-created LoRa Incident ID for field correlation. Optional for
    /// compatibility with alerts already persisted on this iPhone.
    var incidentID: String?

    var status: AlertStatus {
        get { AlertStatus(rawValue: statusRaw) ?? .open }
        set { statusRaw = newValue.rawValue }
    }

    /// Alert titles are stored canonically so changing the iPhone language does
    /// not leave a mixed-language activity history.
    var displayTitle: String {
        switch title.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "sos": "SOS"
        case "help", "求助": String(localized: "Help")
        default: title
        }
    }

    init(eventKey: String, gatewayIdentity: String = "unscoped", recordID: Int64, nodeUID: String, boatID: String? = nil, title: String, detail: String, occurredAt: Date? = nil, syncedAt: Date = .now, status: AlertStatus = .open, incidentID: String? = nil) {
        self.eventKey = eventKey
        self.gatewayIdentity = gatewayIdentity
        self.recordID = recordID
        self.nodeUID = nodeUID
        self.boatID = boatID
        self.title = title
        self.detail = detail
        self.createdAt = occurredAt ?? syncedAt
        self.occurredAt = occurredAt
        self.syncedAt = syncedAt
        self.statusRaw = status.rawValue
        self.incidentID = incidentID
    }
}

extension BoatSnapshot {
    var displayedTimestamp: String { displayEventTimestamp(occurredAt: occurredAt, syncedAt: syncedAt) }
    var hasReliableEventTime: Bool { occurredAt != nil && timeQuality != "uptime_only" }
}

private func displayEventTimestamp(occurredAt: Date?, syncedAt: Date) -> String {
    guard let occurredAt else {
        return "\(String(localized: "Unknown event time")) · \(String(localized: "Synced")) \(syncedAt.formatted(date: .abbreviated, time: .shortened))"
    }
    return occurredAt.formatted(date: .abbreviated, time: .shortened)
}

/// Live-only shore records use the high-bit ID namespace.
private let volatileRecordIDFlag: Int64 = 0x8000_0000

func latestValidLocationSnapshot(
    gatewayIdentity: String,
    nodeUID: String? = nil,
    snapshots: [BoatSnapshot]
) -> BoatSnapshot? {
    let candidates = snapshots.filter { snapshot in
        guard snapshot.gatewayIdentity == gatewayIdentity,
              let latitude = snapshot.latitude,
              let longitude = snapshot.longitude else { return false }
        guard nodeUID == nil || snapshot.nodeUID == nodeUID else { return false }
        return isValidCoordinate(latitude: latitude, longitude: longitude)
    }

    let durable = newestLocationSnapshot(
        in: candidates.filter { ($0.recordID & volatileRecordIDFlag) == 0 }
    )
    let volatile = newestLocationSnapshot(
        in: candidates.filter { ($0.recordID & volatileRecordIDFlag) != 0 }
    )

    guard let durable else { return volatile }
    guard let volatile else { return durable }

    if let durableEventTime = reliableEventTime(durable),
       let volatileEventTime = reliableEventTime(volatile),
       durableEventTime != volatileEventTime {
        return durableEventTime > volatileEventTime ? durable : volatile
    }
    if durable.syncedAt != volatile.syncedAt {
        return durable.syncedAt > volatile.syncedAt ? durable : volatile
    }
    // An exact cross-namespace tie is inherently ambiguous. Retaining the
    // live-only candidate keeps the outage position visible until there is
    // positive evidence that a durable report is newer.
    return volatile
}

private func newestLocationSnapshot(in snapshots: [BoatSnapshot]) -> BoatSnapshot? {
    guard let first = snapshots.first else { return nil }
    let isVolatile = (first.recordID & volatileRecordIDFlag) != 0
    if !isVolatile {
        // Durable IDs come from one journal epoch and are monotonic.
        return snapshots.max { $0.recordID < $1.recordID }
    }

    // A live-only ID is seeded afresh on every shore reboot. It is monotonic
    // only for that boot, and the wire format carries no boot identifier, so
    // comparing two such IDs can select an older position after a restart.
    // Prefer trustworthy event time, then the time this iPhone observed the
    // report. An exact tie has no reliable ordering evidence.
    return snapshots.dropFirst().reduce(first) { newest, candidate in
        if let newestTime = reliableEventTime(newest),
           let candidateTime = reliableEventTime(candidate),
           newestTime != candidateTime {
            return candidateTime > newestTime ? candidate : newest
        }
        if candidate.syncedAt != newest.syncedAt {
            return candidate.syncedAt > newest.syncedAt ? candidate : newest
        }
        return newest
    }
}

private func reliableEventTime(_ snapshot: BoatSnapshot) -> Date? {
    snapshot.timeQuality == "uptime_only" ? nil : snapshot.occurredAt
}

func incidentLocationSnapshot(
    gatewayIdentity: String,
    eventKey: String,
    snapshots: [BoatSnapshot]
) -> BoatSnapshot? {
    snapshots.first { snapshot in
        guard snapshot.gatewayIdentity == gatewayIdentity,
              snapshot.eventKey == eventKey,
              let latitude = snapshot.latitude,
              let longitude = snapshot.longitude else { return false }
        return isValidCoordinate(latitude: latitude, longitude: longitude)
    }
}

/// Returns a valid boat position that was reported after an alert. This is
/// deliberately separate from `incidentLocationSnapshot`: the latter remains
/// the exact location attached to the incident record.
func latestValidLocationAfterAlert(
    _ alert: MarineAlert,
    snapshots: [BoatSnapshot]
) -> BoatSnapshot? {
    let candidates = snapshots.filter { snapshot in
        snapshot.gatewayIdentity == alert.gatewayIdentity &&
            snapshot.nodeUID == alert.nodeUID &&
            snapshotWasReportedAfterAlert(snapshot, alert: alert)
    }
    return latestValidLocationSnapshot(
        gatewayIdentity: alert.gatewayIdentity,
        nodeUID: alert.nodeUID,
        snapshots: candidates
    )
}

private func snapshotWasReportedAfterAlert(_ snapshot: BoatSnapshot, alert: MarineAlert) -> Bool {
    let snapshotIsVolatile = (snapshot.recordID & volatileRecordIDFlag) != 0
    let alertIsVolatile = (alert.recordID & volatileRecordIDFlag) != 0

    // Durable journal records have a monotonic sequence within one gateway
    // epoch. Live-only IDs do not: their seed changes on every shore reboot.
    if !snapshotIsVolatile && !alertIsVolatile {
        return snapshot.recordID > alert.recordID
    }

    if snapshotIsVolatile && alertIsVolatile {
        if let snapshotTime = reliableEventTime(snapshot),
           let alertTime = alert.occurredAt {
            return snapshotTime > alertTime
        }
        return snapshot.syncedAt > alert.syncedAt
    }

    // For a live/durable comparison, prefer actual event times when both are
    // known. Otherwise `syncedAt` is the only ordering evidence available.
    if let snapshotTime = reliableEventTime(snapshot),
       let alertTime = alert.occurredAt {
        return snapshotTime > alertTime
    }
    return snapshot.syncedAt > alert.syncedAt
}

func incidentAlert(
    gatewayIdentity: String,
    eventKey: String,
    alerts: [MarineAlert]
) -> MarineAlert? {
    alerts.first {
        $0.gatewayIdentity == gatewayIdentity && $0.eventKey == eventKey
    }
}

/// "12.34568, 98.76543" — the one coordinate format shown in the app.
func coordinateText(_ latitude: Double, _ longitude: Double) -> String {
    String(format: "%.5f, %.5f", latitude, longitude)
}

func isValidCoordinate(latitude: Double, longitude: Double) -> Bool {
    latitude.isFinite && longitude.isFinite &&
        (-90...90).contains(latitude) && (-180...180).contains(longitude)
}

/// Computes a straight-line distance over the Earth's surface without relying
/// on Core Location, making it safe to use in domain logic and tests.
func greatCircleDistanceMeters(
    fromLatitude: Double,
    fromLongitude: Double,
    toLatitude: Double,
    toLongitude: Double
) -> Double? {
    guard isValidCoordinate(latitude: fromLatitude, longitude: fromLongitude),
          isValidCoordinate(latitude: toLatitude, longitude: toLongitude) else {
        return nil
    }

    let degreesToRadians = Double.pi / 180
    let latitudeDelta = (toLatitude - fromLatitude) * degreesToRadians
    let longitudeDelta = (toLongitude - fromLongitude) * degreesToRadians
    let fromLatitudeRadians = fromLatitude * degreesToRadians
    let toLatitudeRadians = toLatitude * degreesToRadians
    let halfChord = sin(latitudeDelta / 2) * sin(latitudeDelta / 2)
        + cos(fromLatitudeRadians) * cos(toLatitudeRadians)
        * sin(longitudeDelta / 2) * sin(longitudeDelta / 2)
    let clampedHalfChord = min(1, max(0, halfChord))
    let centralAngle = 2 * atan2(sqrt(clampedHalfChord), sqrt(1 - clampedHalfChord))
    return 6_371_000 * centralAngle
}

func formattedApproximateDistance(_ meters: Double) -> String {
    guard meters.isFinite, meters >= 0 else { return "—" }
    if meters < 1_000 {
        return "\(Int(meters.rounded())) m"
    }
    return String(format: "%.1f km", meters / 1_000)
}

func activeAlertsForCurrentGateway(
    gatewayIdentity: String?,
    alerts: [MarineAlert]
) -> [MarineAlert] {
    guard let gatewayIdentity else { return [] }
    return alerts.filter {
        $0.gatewayIdentity == gatewayIdentity &&
            ($0.status == .open || $0.status == .acknowledged)
    }
}

func pastAlertsForCurrentGateway(
    gatewayIdentity: String?,
    alerts: [MarineAlert]
) -> [MarineAlert] {
    guard let gatewayIdentity else { return [] }
    return alerts.filter {
        $0.gatewayIdentity == gatewayIdentity &&
            ($0.status == .resolved || $0.status == .superseded)
    }
}

func boatSnapshotsForCurrentGateway(
    gatewayIdentity: String?,
    snapshots: [BoatSnapshot]
) -> [BoatSnapshot] {
    guard let gatewayIdentity else { return [] }
    return snapshots.filter { $0.gatewayIdentity == gatewayIdentity }
}

extension MarineAlert {
    var displayedTimestamp: String { displayEventTimestamp(occurredAt: occurredAt, syncedAt: syncedAt) }
}

/// Kept intentionally compact because this value is sent as one BLE status
/// notification. Larger event/history payloads use the framed Events channel.
struct GatewayStatus: Codable, Equatable {
    let storageReady: Bool?
    let journalCount: Int?
    let latestNodeUID: String?
    let lastPacketAgeMS: Int64?
    let radioHealth: String?
    let rssi: Double?
    let snr: Double?
    var alertState: String?
    let activeRecordID: Int64?
    let severity: String?
    let pairingOpen: Bool?

    enum CodingKeys: String, CodingKey {
        case storageReady = "f"
        case journalCount = "c"
        case latestNodeUID = "n"
        case lastPacketAgeMS = "a"
        case radioHealth = "h"
        case rssi = "r"
        case snr = "q"
        case alertState = "s"
        case activeRecordID = "i"
        case severity = "e"
        case pairingOpen = "p"
    }
}

enum RescuePresentationState: Equatable {
    case awaitingCurrentStatus
    case activeAlert
    case activeDetailsPending
    case noActiveRescue
}

func rescuePresentationState(
    status: GatewayStatus?,
    hasLocalAlert: Bool,
    localAlertRecordID: Int64? = nil,
    isCurrent: Bool
) -> RescuePresentationState {
    // A locally unresolved alert remains the fail-safe presentation until a
    // current status identifies the exact terminal Incident. A bare
    // `resolved` with no record ID can follow a journal reset or demo state
    // and must not clear an unrelated local alert.
    if hasLocalAlert {
        if isCurrent,
           status?.alertState == AlertStatus.resolved.rawValue,
           let terminalRecordID = status?.activeRecordID,
           terminalRecordID == localAlertRecordID {
            return .noActiveRescue
        }
        return .activeAlert
    }
    guard isCurrent else { return .awaitingCurrentStatus }
    if let status, let alertState = status.alertState {
        if alertState == AlertStatus.resolved.rawValue {
            return .noActiveRescue
        }
        guard status.activeRecordID != nil else {
            return .awaitingCurrentStatus
        }
        return .activeDetailsPending
    }
    return .awaitingCurrentStatus
}

/// Chooses the locally stored alert that can safely back the Rescue screen.
/// A current status record ID is authoritative for an active Incident. A
/// terminal status without an ID is not enough to clear an unrelated local
/// alert, so the local nonterminal alert remains visible until sync can
/// correlate the terminal state.
func activeRescueAlert(
    status: GatewayStatus?,
    isCurrent: Bool,
    gatewayIdentity: String?,
    alerts: [MarineAlert]
) -> MarineAlert? {
    let localAlert = activeAlertsForCurrentGateway(
        gatewayIdentity: gatewayIdentity,
        alerts: alerts
    ).first
    guard isCurrent, let status, let gatewayIdentity else { return localAlert }
    if status.alertState == AlertStatus.resolved.rawValue {
        guard let terminalRecordID = status.activeRecordID else { return localAlert }
        return localAlert?.recordID == terminalRecordID ? nil : localAlert
    }
    guard let activeRecordID = status.activeRecordID else { return nil }
    return alerts.first {
        $0.gatewayIdentity == gatewayIdentity &&
            $0.recordID == activeRecordID &&
            $0.status != .resolved &&
            $0.status != .superseded
    }
}

struct GatewayEvent: Codable, Equatable {
    let kind: String
    let recordID: Int64
    let uptimeMS: Int64?
    let unixMS: Int64?
    let timeQuality: String?
    let rssi: Double?
    let snr: Double?
    let packetB64: String?
    let nodeUID: String?
    let boatID: String?
    let nonce: String?
    let status: String?
    let latitude: Double?
    let longitude: Double?
    let gpsAgeSeconds: Int64?
    let incidentID: String?
    let batteryPercent: Double?
    let syncID: String?
    let supersedesRecordID: Int64?
    let alertState: String?
    let source: String?
    let durable: Bool?

    enum CodingKeys: String, CodingKey {
        case kind
        case recordID = "record_id"
        case uptimeMS = "uptime_ms"
        case unixMS = "unix_ms"
        case timeQuality = "time_quality"
        case rssi, snr
        case packetB64 = "packet_b64"
        case nodeUID = "node_uid"
        case boatID = "boat_id"
        case nonce, status, latitude, longitude
        case gpsAgeSeconds = "gps_age_s"
        case incidentID = "incident_id"
        case batteryPercent = "battery_percent"
        case syncID = "sync_id"
        case supersedesRecordID = "supersedes_record_id"
        case alertState = "alert_state"
        case source, durable
    }

    var occurredAt: Date? { unixMS.map { Date(timeIntervalSince1970: TimeInterval($0) / 1_000) } }

    /// Sync records necessarily came from the durable journal. Unsolicited
    /// live records must explicitly prove durability before they can advance
    /// the cursor or replace a live-only snapshot.
    var isDurable: Bool { durable ?? (syncID != nil) }
}

struct GatewayTransition: Codable, Equatable {
    let kind: String
    let recordID: Int64
    let targetRecordID: Int64
    let state: AlertStatus
    let source: String?
    let unixMS: Int64?
    let timeQuality: String?
    let syncID: String?

    enum CodingKeys: String, CodingKey {
        case kind
        case recordID = "record_id"
        case targetRecordID = "target_record_id"
        case state, source
        case unixMS = "unix_ms"
        case timeQuality = "time_quality"
        case syncID = "sync_id"
    }
}

struct SyncComplete: Codable, Equatable {
    let kind: String
    let firstRecordID: Int64
    let lastRecordID: Int64
    let count: Int
    let hasMore: Bool
    let truncated: Bool?
    let storageReady: Bool?
    let syncID: String?
    let missingRecordIDRanges: [[Int64]]?
    let truncatedBeforeRecordID: Int64?

    enum CodingKeys: String, CodingKey {
        case kind
        case firstRecordID = "first_record_id"
        case lastRecordID = "last_record_id"
        case count
        case hasMore = "has_more"
        case truncated
        case storageReady = "storage_ready"
        case syncID = "sync_id"
        case missingRecordIDRanges = "missing_record_id_ranges"
        case truncatedBeforeRecordID = "truncated_before_record_id"
    }
}

struct GatewayDeviceInfo: Codable, Equatable {
    let name: String
    let nodeUID: String
    let journalEpoch: String
    let firmware: String

    var identity: String { "\(nodeUID):\(journalEpoch)" }

    enum CodingKeys: String, CodingKey {
        case name
        case nodeUID = "node_uid"
        case journalEpoch = "journal_epoch"
        case firmware
    }
}

struct GatewayEnrollmentCandidate: Equatable, Identifiable {
    var id = UUID()
    let peripheralID: UUID
    let info: GatewayDeviceInfo
    let connectionGeneration: Int
}

struct CommandResult: Codable, Equatable {
    let kind: String
    let commandID: String?
    let op: String?
    let recordID: Int64?
    let source: String?
    let ok: Bool?
    let error: String?

    enum CodingKeys: String, CodingKey {
        case kind, op
        case commandID = "command_id"
        case recordID = "record_id"
        case source, ok, error
    }
}
