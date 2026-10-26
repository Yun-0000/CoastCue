import SwiftData
import SwiftUI

@main
struct CoastCueApp: App {
  @State private var model: AppModel

  init() {
    #if DEBUG
      let service: GatewayBluetoothService =
        ProcessInfo.processInfo.arguments.contains("-CoastCueDemo")
        ? DemoGatewayService()
        : BluetoothGatewayService()
    #else
      let service: GatewayBluetoothService = BluetoothGatewayService()
    #endif
    _model = State(initialValue: AppModel(ble: service))
  }

  var body: some Scene {
    WindowGroup {
      AppShell(model: model)
    }
    .modelContainer(for: [BoatSnapshot.self, MarineAlert.self])
  }
}

#if DEBUG
  /// Debug-only service used for deterministic visual and interaction QA.
  /// It is selected only with the explicit `-CoastCueDemo` launch argument.
  @MainActor private final class DemoGatewayService: @preconcurrency GatewayBluetoothService {
    var savedGatewayIdentity: String? { nil }
    var onStatus: (@MainActor (GatewayStatus) -> Void)?
    var onEventData: (@MainActor (Data) -> Void)?
    var onConnectionChanged: (@MainActor (BluetoothGatewayService.ConnectionState) -> Void)?
    var onError: (@MainActor (String) -> Void)?
    var onAlertCommandWriteFailed: (@MainActor (String) -> Void)?
    var onDeviceInfo: (@MainActor (GatewayDeviceInfo) -> Void)?
    var onEnrollmentCandidate: (@MainActor (GatewayEnrollmentCandidate) -> Void)?

    private let activeRecordID: Int64 = 3
    private var activeState: AlertStatus = .open
    private var nextMessageID: UInt32 = 1

    func connect() {
      onConnectionChanged?(.connecting)
      onConnectionChanged?(.connected)
      onDeviceInfo?(
        GatewayDeviceInfo(
          name: "CoastCue Shore",
          nodeUID: "demo-shore",
          journalEpoch: "1",
          firmware: "demo"
        )
      )
    }

    func confirmEnrollment(_ candidate: GatewayEnrollmentCandidate) {
      onDeviceInfo?(candidate.info)
    }

    func readStatus() {
      onStatus?(status())
    }

    func cancelEnrollment(_ candidate: GatewayEnrollmentCandidate) {
      disconnect()
    }

    func sync(after cursor: Int64, syncID: String) {
      onConnectionChanged?(.syncing)

      let records: [(Int64, [String: Any])] = [
        (
          1,
          event(
            recordID: 1,
            nodeUID: "CC-BOAT-03",
            status: "help",
            latitude: 29.98960,
            longitude: 122.21710,
            minutesAgo: 47,
            syncID: syncID
          )
        ),
        (
          2,
          [
            "kind": "transition",
            "record_id": 2,
            "target_record_id": 1,
            "state": "resolved",
            "source": "button",
            "sync_id": syncID,
          ]
        ),
        (
          activeRecordID,
          event(
            recordID: activeRecordID,
            nodeUID: "CC-BOAT-07",
            status: "sos",
            latitude: 29.98500,
            longitude: 122.23300,
            minutesAgo: 2,
            syncID: syncID
          )
        ),
      ]
      let pendingRecords = records.filter { $0.0 > cursor }
      pendingRecords.forEach { emit($0.1) }
      onStatus?(status())
      emit([
        "kind": "sync_complete",
        "first_record_id": pendingRecords.first?.0 ?? 0,
        "last_record_id": pendingRecords.last?.0 ?? cursor,
        "count": pendingRecords.count,
        "has_more": false,
        "truncated": false,
        "storage_ready": true,
        "sync_id": syncID,
      ])
    }

    func finishSync() {
      onConnectionChanged?(.connected)
    }

    @discardableResult
    func acknowledge(recordID: Int64, commandID: String) -> Bool {
      activeState = .acknowledged
      emitCommandResult(operation: "ack", recordID: recordID, commandID: commandID)
      return true
    }

    @discardableResult
    func resolve(recordID: Int64, commandID: String) -> Bool {
      activeState = .resolved
      emitCommandResult(operation: "resolve", recordID: recordID, commandID: commandID)
      return true
    }

    func disconnect() {
      onConnectionChanged?(.idle)
    }

    func forgetGateway() {
      disconnect()
    }

    private func emitCommandResult(operation: String, recordID: Int64, commandID: String) {
      emit([
        "kind": "command_result",
        "command_id": commandID,
        "op": operation,
        "record_id": recordID,
        "source": "ble",
        "ok": true,
      ])
      onStatus?(status())
    }

    @MainActor
    private func emit(_ object: [String: Any]) {
      guard let data = try? JSONSerialization.data(withJSONObject: object) else { return }
      let messageID = nextMessageID
      nextMessageID &+= 1
      GatewayMessageFramer.frames(for: data, messageID: messageID).forEach { onEventData?($0) }
    }

    private func event(
      recordID: Int64,
      nodeUID: String,
      status: String,
      latitude: Double,
      longitude: Double,
      minutesAgo: Double,
      syncID: String
    ) -> [String: Any] {
      [
        "kind": "event",
        "record_id": recordID,
        "unix_ms": Int64(Date().addingTimeInterval(-minutesAgo * 60).timeIntervalSince1970 * 1_000),
        "time_quality": "phone_set",
        "rssi": -87.0,
        "snr": 7.5,
        "node_uid": nodeUID,
        "nonce": "DEMO-\(recordID)",
        "status": status,
        "latitude": latitude,
        "longitude": longitude,
        "battery_percent": 78.0,
        "sync_id": syncID,
      ]
    }

    private func status() -> GatewayStatus {
      GatewayStatus(
        storageReady: true,
        journalCount: 18,
        latestNodeUID: "CC-BOAT-07",
        lastPacketAgeMS: 2_000,
        radioHealth: "healthy",
        rssi: -87,
        snr: 7.5,
        alertState: activeState.rawValue,
        activeRecordID: activeState == .resolved ? nil : activeRecordID,
        severity: activeState == .resolved ? nil : "sos",
        pairingOpen: false
      )
    }
  }
#endif
