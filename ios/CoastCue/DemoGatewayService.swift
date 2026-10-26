#if DEBUG
import Foundation
import UIKit

/// Simulator stand-in for the shore gateway, for filming the app (the iOS Simulator has no
/// Bluetooth). Debug builds only; launch with `-CoastCueDemo`. It speaks the BLE contract in
/// docs/PROTOCOL.md (device info, Status, event / transition / sync_complete / command_result JSON)
/// so the real AppModel, SwiftData store and views run unchanged. Boat alerts are triggered from the
/// Mac while recording:
///   xcrun simctl spawn booted notifyutil -p coastcue.demo.help   (also .sos, .offline, .online)
/// With `-CoastCueDemoUnpaired` the shore starts unpaired: `coastcue.demo.pair` stands for holding
/// the shore button for 3 s, which opens pairing: iOS's Bluetooth pairing prompt asks for the shore's
/// six-digit passkey (replicated here: the Simulator has no Bluetooth), then the app's confirmation sheet.
@MainActor
final class DemoGatewayService: @preconcurrency GatewayBluetoothService {
  var onStatus: (@MainActor (GatewayStatus) -> Void)?
  var onEventData: (@MainActor (Data) -> Void)?
  var onConnectionChanged: (@MainActor (BluetoothGatewayService.ConnectionState) -> Void)?
  var onError: (@MainActor (String) -> Void)?
  var onAlertCommandWriteFailed: (@MainActor (String) -> Void)?
  var onDeviceInfo: (@MainActor (GatewayDeviceInfo) -> Void)?
  var onEnrollmentCandidate: (@MainActor (GatewayEnrollmentCandidate) -> Void)?

  static let info = GatewayDeviceInfo(name: "CoastCue Shore", nodeUID: "shore-7c1e", journalEpoch: "e1", firmware: "us-1.0")
  var savedGatewayIdentity: String? { paired ? Self.info.identity : nil }
  private var paired = !ProcessInfo.processInfo.arguments.contains("-CoastCueDemoUnpaired")

  // The U.S. field test at Shoreline Lake, Mountain View: the shore reference is the boathouse deck
  // (the shore phones' GPS) and the boat sits where the shore screen showed its Help (~120 m out).
  static let shore = (latitude: 37.43330, longitude: -122.08790)
  private static let boat = (latitude: 37.43256, longitude: -122.08885)

  private var journal: [[String: Any]] = []
  // Idle shore status as the firmware encodes it (ShoreRuntime::notifyStatus), re-sent every second.
  private var status: [String: Any] = ["f": true, "c": 0, "h": "recent", "s": "resolved", "e": "none"]
  private var linked = false
  private var offline = false
  private var incident: (record: Int64, id: String, severity: String)?

  init(defaults: UserDefaults = .standard) {
    let reference = ShoreReference(latitude: Self.shore.latitude, longitude: Self.shore.longitude, capturedAt: .now, horizontalAccuracyMeters: 5)
    defaults.set(try? JSONEncoder().encode(reference), forKey: "coastcue.shore-reference.\(Self.info.identity)")
    for (name, action) in [("help", "help"), ("sos", "sos"), ("offline", "offline"), ("online", "online"), ("pair", "pair")] {
      DarwinNotification.observe("coastcue.demo.\(name)") { [weak self] in self?.trigger(action) }
    }
  }

  func connect() {
    guard !offline else { onConnectionChanged?(.idle); return }
    onConnectionChanged?(.connecting)
    later(0.5) { [self] in
      linked = true
      onConnectionChanged?(.connected)
      guard paired else { return }  // an unpaired shore shares nothing until pairing is confirmed
      onDeviceInfo?(Self.info)
      heartbeat()
    }
  }

  private func heartbeat() {
    later(1) { [self] in
      guard linked else { return }
      publishStatus()
      heartbeat()
    }
  }

  func readStatus() { if linked { publishStatus() } }

  func sync(after: Int64, limit: Int, syncID: String) {
    onConnectionChanged?(.syncing)
    let rows = journal.filter { ($0["record_id"] as! Int64) > after }.prefix(limit)
    later(0.6) { [self] in
      for var row in rows {
        row["sync_id"] = syncID
        emit(row)
      }
      let ids = rows.map { $0["record_id"] as! Int64 }
      emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": ids.first ?? 0, "last_record_id": ids.last ?? after,
            "count": ids.count, "has_more": journal.count > ids.count + Int(after), "truncated": false, "storage_ready": true])
    }
  }

  func finishSync() { onConnectionChanged?(.connected) }

  @discardableResult func acknowledge(recordID: Int64, commandID: String) -> Bool { operate("ack", recordID: recordID, commandID: commandID) }
  @discardableResult func resolve(recordID: Int64, commandID: String) -> Bool { operate("resolve", recordID: recordID, commandID: commandID) }

  func disconnect() {
    linked = false
    onConnectionChanged?(.idle)
  }

  func forgetGateway() {
    paired = false
    disconnect()
  }

  func confirmEnrollment(_ candidate: GatewayEnrollmentCandidate) {
    paired = true
    onDeviceInfo?(candidate.info)
    heartbeat()
  }
  func cancelEnrollment(_ candidate: GatewayEnrollmentCandidate) { disconnect() }

  // The shore's side of an operator action: append the transition, then answer the command.
  private func operate(_ op: String, recordID: Int64, commandID: String) -> Bool {
    guard linked, let incident, incident.record == recordID else { return false }
    let state = op == "ack" ? "acknowledged" : "resolved"
    later(1.0) { [self] in
      let row = append(["kind": "transition", "target_record_id": recordID, "state": state, "source": "ble",
                        "incident_id": incident.id, "radio_state_seq": op == "ack" ? 2 : 3])
      emit(row)
      emit(["kind": "command_result", "command_id": commandID, "op": op, "record_id": recordID, "source": "ble", "ok": true])
      status["s"] = state
      if state == "resolved" {
        self.incident = nil
        status["e"] = "none"
      }
      publishStatus()
    }
    return true
  }

  private func trigger(_ action: String) {
    switch action {
    case "offline":
      offline = true
      disconnect()
    case "online":
      offline = false
    case "pair":
      guard linked, !paired else { return }
      showPairingRequest { [self] in
        onEnrollmentCandidate?(GatewayEnrollmentCandidate(peripheralID: UUID(), info: Self.info, connectionGeneration: 1))
      }
    default:
      raise(action)
    }
  }

  // On an iPhone, encrypted GATT access makes iOS itself ask for the passkey the shore screen shows
  // (docs/PROTOCOL.md); the Simulator has no Bluetooth, so the demo puts up the same system prompt.
  private func showPairingRequest(then pair: @escaping @MainActor () -> Void) {
    let name = Self.info.name
    let alert = UIAlertController(
      title: "Bluetooth Pairing Request",
      message: "\u{201C}\(name)\u{201D} would like to pair with your iPhone. Enter the code shown on \u{201C}\(name)\u{201D}.",
      preferredStyle: .alert)
    alert.addTextField { $0.keyboardType = .numberPad }
    alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
    alert.addAction(UIAlertAction(title: "Pair", style: .default) { _ in MainActor.assumeIsolated { pair() } })
    var top = UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.keyWindow }.first?.rootViewController
    while let next = top?.presentedViewController { top = next }
    top?.present(alert, animated: true)
  }

  // A Help or SOS arrives over LoRa: the shore journals it (SOS supersedes an unresolved Help).
  private func raise(_ severity: String) {
    let previous = incident
    let id = String(format: "%016llx", UInt64.random(in: 1...UInt64.max))
    var row: [String: Any] = [
      "kind": "event", "status": severity, "node_uid": "boat-3f9a", "boat_id": "CoastCue-1",
      "nonce": String(format: "%016llx", UInt64.random(in: 1...UInt64.max)),
      "latitude": Self.boat.latitude, "longitude": Self.boat.longitude, "gps_age_s": 2, "battery_percent": 86,
      "rssi": -97, "snr": 6.5, "incident_id": id, "radio_state_seq": 1, "alert_state": "open",
    ]
    if let previous { row["supersedes_record_id"] = previous.record }
    row = append(row)
    let record = row["record_id"] as! Int64
    incident = (record, id, severity)
    status.merge(["s": "open", "i": record, "e": severity, "n": "boat-3f9a", "a": 400, "r": -97, "q": 6.5]) { $1 }
    guard linked else { return }
    emit(row)
    if let previous {
      emit(append(["kind": "transition", "target_record_id": previous.record, "state": "superseded", "source": "superseded"]))
    }
    publishStatus()
  }

  private func append(_ fields: [String: Any]) -> [String: Any] {
    var row = fields
    row["record_id"] = Int64(journal.count + 1)
    row["uptime_ms"] = Int64(ProcessInfo.processInfo.systemUptime * 1000)
    row["unix_ms"] = Int64(Date.now.timeIntervalSince1970 * 1000)
    row["time_quality"] = "phone_set"
    row["durable"] = true
    journal.append(row)
    status["c"] = journal.count
    return row
  }

  private func publishStatus() {
    guard let data = try? JSONSerialization.data(withJSONObject: status),
          let value = try? JSONDecoder().decode(GatewayStatus.self, from: data) else { return }
    onStatus?(value)
  }

  // Like the shore firmware: JSON up to 160 bytes goes as is, larger objects in SM-framed fragments.
  private var messageID: UInt32 = 1
  private func emit(_ object: [String: Any]) {
    guard let json = try? JSONSerialization.data(withJSONObject: object) else { return }
    let limit = GatewayMessageFramer.unframedMessageLimit
    guard json.count > limit else { onEventData?(json); return }
    let count = (json.count + limit - 1) / limit
    let crc = CRC32.checksum(json)
    let le = { (v: UInt32, n: Int) in (0..<n).map { UInt8((v >> (8 * $0)) & 0xFF) } }
    for index in 0..<count {
      let header = [0x53, 0x4D, 1, 1] + le(messageID, 4) + le(UInt32(index), 2) + le(UInt32(count), 2) + le(crc, 4)
      onEventData?(Data(header) + json[(index * limit)..<min((index + 1) * limit, json.count)])
    }
    messageID &+= 1
  }

  private func later(_ seconds: Double, _ work: @escaping @MainActor () -> Void) {
    Task { @MainActor in
      try? await Task.sleep(for: .seconds(seconds))
      work()
    }
  }
}

/// Darwin notifications, so `notifyutil -p` on the Mac can reach the app in the Simulator.
private enum DarwinNotification {
  @MainActor private static var handlers: [String: () -> Void] = [:]

  @MainActor static func observe(_ name: String, _ handler: @escaping () -> Void) {
    handlers[name] = handler
    CFNotificationCenterAddObserver(
      CFNotificationCenterGetDarwinNotifyCenter(), nil,
      { _, _, name, _, _ in
        guard let key = name?.rawValue as String? else { return }
        Task { @MainActor in DarwinNotification.handlers[key]?() }
      },
      name as CFString, nil, .deliverImmediately)
  }
}
#endif
