import Foundation
@preconcurrency import CoreBluetooth
import Observation

struct ConnectionRetryBudget {
    private(set) var hasUsedSoftRetry = false

    mutating func reset() { hasUsedSoftRetry = false }

    mutating func consumeSoftRetry() -> Bool {
        guard !hasUsedSoftRetry else { return false }
        hasUsedSoftRetry = true
        return true
    }
}

enum CommandWriteKind: Equatable {
    case alert(String)
    case other
}

struct QueuedCommandWrite: Equatable {
    let sequence: UInt64
    let data: Data
    let kind: CommandWriteKind

    init(sequence: UInt64 = 0, data: Data, kind: CommandWriteKind) {
        self.sequence = sequence
        self.data = data
        self.kind = kind
    }
}

struct CommandWriteQueue {
    private(set) var pending: [QueuedCommandWrite] = []
    private(set) var inFlight: QueuedCommandWrite?

    mutating func enqueue(_ write: QueuedCommandWrite) {
        switch write.kind {
        case .alert:
            let insertionIndex = pending.firstIndex {
                if case .other = $0.kind { return true }
                return false
            } ?? pending.endIndex
            pending.insert(write, at: insertionIndex)
        case .other:
            pending.append(write)
        }
    }

    mutating func startNext() -> QueuedCommandWrite? {
        guard inFlight == nil, !pending.isEmpty else { return nil }
        let write = pending.removeFirst()
        inFlight = write
        return write
    }

    mutating func completeInFlight() -> QueuedCommandWrite? {
        defer { inFlight = nil }
        return inFlight
    }

    mutating func reset() {
        pending.removeAll()
        inFlight = nil
    }
}

protocol GatewayBluetoothService: AnyObject {
    var savedGatewayIdentity: String? { get }
    var onStatus: (@MainActor (GatewayStatus) -> Void)? { get set }
    var onEventData: (@MainActor (Data) -> Void)? { get set }
    var onConnectionChanged: (@MainActor (BluetoothGatewayService.ConnectionState) -> Void)? { get set }
    var onError: (@MainActor (String) -> Void)? { get set }
    var onAlertCommandWriteFailed: (@MainActor (String) -> Void)? { get set }
    var onDeviceInfo: (@MainActor (GatewayDeviceInfo) -> Void)? { get set }
    var onEnrollmentCandidate: (@MainActor (GatewayEnrollmentCandidate) -> Void)? { get set }
    func connect()
    func confirmEnrollment(_ candidate: GatewayEnrollmentCandidate)
    func readStatus()
    func cancelEnrollment(_ candidate: GatewayEnrollmentCandidate)
    func sync(after: Int64, syncID: String)
    func sync(after: Int64, limit: Int, syncID: String)
    func finishSync()
    @discardableResult func acknowledge(recordID: Int64, commandID: String) -> Bool
    @discardableResult func resolve(recordID: Int64, commandID: String) -> Bool
    func disconnect()
    func forgetGateway()
}

extension GatewayBluetoothService {
    func sync(after: Int64, limit: Int, syncID: String) {
        sync(after: after, syncID: syncID)
    }
}

@MainActor
final class GatewayTrustStore {
    private struct Pin: Codable {
        let peripheralID: UUID
        let gatewayIdentity: String
    }

    private static let storageKey = "coastcue.gateway-pin"
    private static let legacyStorageKey = "coastcue.gateway-pin.v1"
    private let defaults: UserDefaults
    private var pin: Pin?
    private var hasUnreadablePin = false

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if let data = defaults.data(forKey: Self.storageKey) {
            if let pin = try? JSONDecoder().decode(Pin.self, from: data) {
                self.pin = pin
            } else {
                hasUnreadablePin = true
            }
            return
        }
        guard let data = defaults.data(forKey: Self.legacyStorageKey) else { return }
        if let pin = try? JSONDecoder().decode(Pin.self, from: data) {
            self.pin = pin
            defaults.set(data, forKey: Self.storageKey)
            defaults.removeObject(forKey: Self.legacyStorageKey)
        } else {
            hasUnreadablePin = true
        }
    }

    var pinnedPeripheralID: UUID? { pin?.peripheralID }
    var pinnedGatewayIdentity: String? { pin?.gatewayIdentity }
    var hasPinnedGateway: Bool { pin != nil || hasUnreadablePin }

    func canConnect(to peripheralID: UUID) -> Bool {
        guard !hasUnreadablePin else { return false }
        return pin.map { $0.peripheralID == peripheralID } ?? true
    }

    func isTrusted(peripheralID: UUID, info: GatewayDeviceInfo) -> Bool {
        guard !hasUnreadablePin else { return false }
        if let pin {
            return pin.peripheralID == peripheralID && pin.gatewayIdentity == info.identity
        }
        return false
    }

    func pinIfUnpinned(peripheralID: UUID, info: GatewayDeviceInfo) -> Bool {
        guard !hasUnreadablePin, pin == nil else { return false }
        let newPin = Pin(peripheralID: peripheralID, gatewayIdentity: info.identity)
        guard let data = try? JSONEncoder().encode(newPin) else { return false }
        defaults.set(data, forKey: Self.storageKey)
        pin = newPin
        return true
    }

    func forget() {
        defaults.removeObject(forKey: Self.storageKey)
        defaults.removeObject(forKey: Self.legacyStorageKey)
        pin = nil
        hasUnreadablePin = false
    }
}

@MainActor @Observable
final class BluetoothGatewayService: NSObject, @preconcurrency GatewayBluetoothService {
    enum ConnectionState: Equatable {
        case idle, scanning, connecting, confirming, connected, syncing, syncFailed, unavailable
    }

    static var serviceUUID: CBUUID { CBUUID(string: "7A1F0001-6B7C-4D8E-9F10-112233445566") }
    static var statusUUID: CBUUID { CBUUID(string: "7A1F0002-6B7C-4D8E-9F10-112233445566") }
    static var eventsUUID: CBUUID { CBUUID(string: "7A1F0003-6B7C-4D8E-9F10-112233445566") }
    static var commandUUID: CBUUID { CBUUID(string: "7A1F0004-6B7C-4D8E-9F10-112233445566") }
    static var deviceInfoUUID: CBUUID { CBUUID(string: "7A1F0005-6B7C-4D8E-9F10-112233445566") }

    var onStatus: (@MainActor (GatewayStatus) -> Void)?
    var onEventData: (@MainActor (Data) -> Void)?
    var onConnectionChanged: (@MainActor (ConnectionState) -> Void)?
    var onError: (@MainActor (String) -> Void)?
    var onAlertCommandWriteFailed: (@MainActor (String) -> Void)?
    var onDeviceInfo: (@MainActor (GatewayDeviceInfo) -> Void)?
    var onEnrollmentCandidate: (@MainActor (GatewayEnrollmentCandidate) -> Void)?
    var savedGatewayIdentity: String? { trustStore.pinnedGatewayIdentity }

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var characteristics: [CBUUID: CBCharacteristic] = [:]
    private var deviceInfoReady = false
    private var cachedDeviceInfo: GatewayDeviceInfo?
    private var waitingForPowerOn = false
    private var connectionRetryBudget = ConnectionRetryBudget()
    private var connectionGeneration = 0
    private var connectionTimeoutTask: Task<Void, Never>?
    private var writeTimeoutTask: Task<Void, Never>?
    private var automaticReconnectTask: Task<Void, Never>?
    private var automaticReconnectAttempt = 0
    private var recoveringUnexpectedDisconnect = false
    private var userRequestedDisconnect = false
    private var pendingEnrollmentCandidate: GatewayEnrollmentCandidate?
    private var commandWrites = CommandWriteQueue()
    private var nextCommandWriteSequence: UInt64 = 1
    private let trustStore: GatewayTrustStore
    private static let automaticReconnectDelays: [Duration] = [
        .seconds(1), .seconds(2), .seconds(4),
    ]
    private var connectionState: ConnectionState = .idle {
        didSet {
            onConnectionChanged?(connectionState)
        }
    }

    init(defaults: UserDefaults = .standard) {
        trustStore = GatewayTrustStore(defaults: defaults)
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func connect() {
        stopAutomaticReconnect()
        userRequestedDisconnect = false
        connectionRetryBudget.reset()
        switch central.state {
        case .poweredOn:
            startScanning()
        case .unknown, .resetting:
            waitingForPowerOn = true
            connectionState = .connecting
            armConnectionTimeout(while: .connecting)
        default:
            connectionState = .unavailable
            report(String(localized: "Bluetooth is unavailable. Turn it on and try again."))
        }
    }

    private func startScanning() {
        guard central.state == .poweredOn else {
            resetConnection(state: .unavailable)
            report(String(localized: "Bluetooth is unavailable. Turn it on and try again."))
            return
        }
        waitingForPowerOn = false
        if peripheral?.state == .connected, deviceInfoReady {
            readDeviceInfo(force: true)
            return
        }
        if let peripheral {
            central.cancelPeripheralConnection(peripheral)
            self.peripheral = nil
        }
        characteristics.removeAll()
        deviceInfoReady = false
        cachedDeviceInfo = nil
        central.stopScan()
        if let pinnedID = trustStore.pinnedPeripheralID,
           let pinnedPeripheral = central.retrievePeripherals(withIdentifiers: [pinnedID]).first {
            beginConnecting(to: pinnedPeripheral)
            return
        }
        connectionState = .scanning
        armConnectionTimeout(while: .scanning)
        central.scanForPeripherals(
            withServices: [Self.serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
    }

    @discardableResult
    func acknowledge(recordID: Int64, commandID: String) -> Bool {
        send(["op": "ack", "record_id": recordID, "command_id": commandID], alertCommandID: commandID)
    }

    @discardableResult
    func resolve(recordID: Int64, commandID: String) -> Bool {
        send(["op": "resolve", "record_id": recordID, "command_id": commandID], alertCommandID: commandID)
    }

    func disconnect() {
        userRequestedDisconnect = true
        stopAutomaticReconnect()
        central.stopScan()
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
        resetConnection(state: .idle)
    }

    func forgetGateway() {
        userRequestedDisconnect = true
        stopAutomaticReconnect()
        trustStore.forget()
        resetConnection(state: .idle)
    }

    func confirmEnrollment(_ candidate: GatewayEnrollmentCandidate) {
        guard candidate == pendingEnrollmentCandidate,
              candidate.connectionGeneration == connectionGeneration,
              let peripheral,
              peripheral.identifier == candidate.peripheralID,
              peripheral.state == .connected,
              trustStore.pinIfUnpinned(peripheralID: candidate.peripheralID, info: candidate.info) else {
            report(String(localized: "The shore gateway pairing request is no longer valid. Please connect again."))
            return
        }
        pendingEnrollmentCandidate = nil
        deviceInfoReady = true
        cachedDeviceInfo = candidate.info
        connectionRetryBudget.reset()
        stopAutomaticReconnect()
        connectionTimeoutTask?.cancel()
        connectionState = .connected
        onDeviceInfo?(candidate.info)
    }

    func readStatus() {
        guard let peripheral,
              peripheral.state == .connected,
              let status = characteristics[Self.statusUUID] else { return }
        peripheral.readValue(for: status)
    }

    func cancelEnrollment(_ candidate: GatewayEnrollmentCandidate) {
        guard candidate == pendingEnrollmentCandidate else { return }
        resetConnection(state: .idle)
    }

    func sync(after: Int64, syncID: String) {
        sync(after: after, limit: 4, syncID: syncID)
    }

    func sync(after: Int64, limit: Int, syncID: String) {
        connectionTimeoutTask?.cancel()
        connectionState = .syncing
        send(["op": "sync", "after": after, "limit": limit, "sync_id": syncID, "unix_ms": Int64(Date().timeIntervalSince1970 * 1_000)])
    }

    func finishSync() {
        guard connectionState == .syncing else { return }
        connectionState = .connected
    }

    private func readDeviceInfo(force: Bool = false) {
        guard characteristics[Self.deviceInfoUUID] != nil,
              characteristics[Self.commandUUID] != nil,
              characteristics[Self.eventsUUID]?.isNotifying == true,
              let peripheral,
              let deviceInfo = characteristics[Self.deviceInfoUUID] else { return }
        if force, let cachedDeviceInfo {
            armConnectionTimeout(while: connectionState)
            onDeviceInfo?(cachedDeviceInfo)
            return
        }
        guard !deviceInfoReady || force else { return }
        armConnectionTimeout(while: connectionState)
        peripheral.readValue(for: deviceInfo)
    }

    @discardableResult
    private func send(_ payload: [String: Any], alertCommandID: String? = nil) -> Bool {
        guard let peripheral,
              peripheral.state == .connected,
              let command = characteristics[Self.commandUUID],
              let data = try? JSONSerialization.data(withJSONObject: payload) else {
            report(String(localized: "The gateway command channel is not ready."))
            return false
        }
        guard data.count < 192, data.count <= peripheral.maximumWriteValueLength(for: .withResponse) else {
            report(String(localized: "The gateway command is too large."))
            return false
        }
        guard commandWrites.pending.count < 8 else {
            report(String(localized: "The shore gateway is busy. Try again in a moment."))
            return false
        }
        let sequence = nextCommandWriteSequence
        nextCommandWriteSequence &+= 1
        let write = QueuedCommandWrite(
            sequence: sequence,
            data: data,
            kind: alertCommandID.map(CommandWriteKind.alert) ?? .other
        )
        commandWrites.enqueue(write)
        logCommandWrite("queued", write: write)
        pumpCommandWrite(peripheral: peripheral, characteristic: command)
        return true
    }

    private func pumpCommandWrite(peripheral: CBPeripheral? = nil, characteristic: CBCharacteristic? = nil) {
        guard commandWrites.inFlight == nil,
              let peripheral = peripheral ?? self.peripheral,
              peripheral.state == .connected,
              let command = characteristic ?? characteristics[Self.commandUUID],
              let write = commandWrites.startNext() else { return }
        logCommandWrite("started", write: write)
        writeTimeoutTask?.cancel()
        let generation = connectionGeneration
        writeTimeoutTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(8))
            guard let self, !Task.isCancelled,
                  self.connectionGeneration == generation,
                  self.commandWrites.inFlight?.sequence == write.sequence else { return }
            // Delivery is uncertain: reconnect and read the authoritative state,
            // never replay an operator action automatically.
            self.resetConnection(state: .idle)
            self.report(String(localized: "Gateway write timed out. Reconnecting to check its state."))
            self.scheduleAutomaticReconnect()
        }
        peripheral.writeValue(write.data, for: command, type: .withResponse)
    }

    private func logCommandWrite(_ phase: String, write: QueuedCommandWrite, error: Error? = nil) {
        let kind: String
        switch write.kind {
        case .alert(let commandID): kind = "alert:\(commandID)"
        case .other: kind = "other"
        }
        let suffix = error.map { " error=\($0.localizedDescription)" } ?? ""
        print("[BLE-TX] \(phase) seq=\(write.sequence) kind=\(kind) bytes=\(write.data.count) pending=\(commandWrites.pending.count)\(suffix)")
    }

    private func resetConnection(state: ConnectionState) {
        connectionTimeoutTask?.cancel()
        writeTimeoutTask?.cancel()
        central.stopScan()
        if let peripheral {
            central.cancelPeripheralConnection(peripheral)
        }
        characteristics.removeAll()
        deviceInfoReady = false
        cachedDeviceInfo = nil
        pendingEnrollmentCandidate = nil
        commandWrites.reset()
        waitingForPowerOn = false
        peripheral = nil
        connectionGeneration &+= 1
        connectionState = state
    }

    private func stopAutomaticReconnect() {
        automaticReconnectTask?.cancel()
        automaticReconnectTask = nil
        automaticReconnectAttempt = 0
        recoveringUnexpectedDisconnect = false
    }

    private func scheduleAutomaticReconnect() {
        guard !userRequestedDisconnect, trustStore.hasPinnedGateway else {
            stopAutomaticReconnect()
            return
        }
        guard central.state == .poweredOn else {
            recoveringUnexpectedDisconnect = true
            waitingForPowerOn = true
            connectionState = .unavailable
            return
        }
        guard automaticReconnectAttempt < Self.automaticReconnectDelays.count else {
            stopAutomaticReconnect()
            connectionState = .idle
            report(String(localized: "Gateway connection was lost. Tap Connect to retry."))
            return
        }
        recoveringUnexpectedDisconnect = true
        let delay = Self.automaticReconnectDelays[automaticReconnectAttempt]
        automaticReconnectAttempt += 1
        automaticReconnectTask?.cancel()
        connectionState = .connecting
        automaticReconnectTask = Task { [weak self] in
            try? await Task.sleep(for: delay)
            guard let self, !Task.isCancelled,
                  self.recoveringUnexpectedDisconnect,
                  !self.userRequestedDisconnect else { return }
            self.startScanning()
        }
    }

    private func report(_ message: String) {
        onError?(message)
    }

    private func beginConnecting(to peripheral: CBPeripheral) {
        connectionGeneration &+= 1
        pendingEnrollmentCandidate = nil
        self.peripheral = peripheral
        central.stopScan()
        connectionState = .connecting
        armConnectionTimeout(while: .connecting)
        peripheral.delegate = self
        central.connect(peripheral)
    }

    private func isCurrent(_ callbackPeripheral: CBPeripheral) -> Bool { peripheral === callbackPeripheral }
}

extension BluetoothGatewayService: @preconcurrency CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn, waitingForPowerOn {
            waitingForPowerOn = false
            if recoveringUnexpectedDisconnect {
                scheduleAutomaticReconnect()
            } else {
                startScanning()
            }
        } else if central.state != .poweredOn,
                  central.state != .unknown,
                  central.state != .resetting {
            let shouldRecover = !userRequestedDisconnect &&
                trustStore.hasPinnedGateway &&
                (peripheral != nil || recoveringUnexpectedDisconnect)
            automaticReconnectTask?.cancel()
            automaticReconnectTask = nil
            resetConnection(state: .unavailable)
            if shouldRecover {
                recoveringUnexpectedDisconnect = true
                waitingForPowerOn = true
            }
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard connectionState == .scanning,
              trustStore.canConnect(to: peripheral.identifier) else { return }
        beginConnecting(to: peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard isCurrent(peripheral) else {
            central.cancelPeripheralConnection(peripheral)
            return
        }
        characteristics.removeAll()
        deviceInfoReady = false
        // A CoreBluetooth link is not yet a trusted CoastCue gateway. Keep
        // the public state in `connecting` until Device Info is decoded and
        // either matches the saved pin or the user confirms enrollment.
        connectionState = .connecting
        armConnectionTimeout(while: .connecting)
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        guard isCurrent(peripheral) else { return }
        let shouldReconnect = recoveringUnexpectedDisconnect && !userRequestedDisconnect
        resetConnection(state: .idle)
        if shouldReconnect {
            scheduleAutomaticReconnect()
        } else {
            report(error?.localizedDescription ?? String(localized: "Could not connect to gateway."))
        }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        guard isCurrent(peripheral) else { return }
        let shouldReconnect = !userRequestedDisconnect && trustStore.hasPinnedGateway
        resetConnection(state: .idle)
        if shouldReconnect {
            recoveringUnexpectedDisconnect = true
            scheduleAutomaticReconnect()
        } else if let error, !userRequestedDisconnect {
            report(error.localizedDescription)
        }
    }
}

extension BluetoothGatewayService: @preconcurrency CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard isCurrent(peripheral) else { return }
        if let error {
            report(error.localizedDescription)
            return
        }
        peripheral.services?.forEach {
            peripheral.discoverCharacteristics(
                [Self.statusUUID, Self.eventsUUID, Self.commandUUID, Self.deviceInfoUUID],
                for: $0
            )
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard isCurrent(peripheral) else { return }
        if let error {
            report(error.localizedDescription)
            return
        }
        service.characteristics?.forEach { characteristic in
            characteristics[characteristic.uuid] = characteristic
            if characteristic.uuid == Self.statusUUID || characteristic.uuid == Self.eventsUUID {
                peripheral.setNotifyValue(true, for: characteristic)
            }
            if characteristic.uuid == Self.statusUUID {
                peripheral.readValue(for: characteristic)
            }
        }
        readDeviceInfo()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard isCurrent(peripheral) else { return }
        if let error {
            report(error.localizedDescription)
            return
        }
        if characteristic.uuid == Self.eventsUUID, characteristic.isNotifying {
            readDeviceInfo()
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard isCurrent(peripheral) else { return }
        if let error {
            report(error.localizedDescription)
            return
        }
        guard let data = characteristic.value else { return }
        if characteristic.uuid == Self.statusUUID,
           let status = try? JSONDecoder().decode(GatewayStatus.self, from: data) {
            onStatus?(status)
        } else if characteristic.uuid == Self.eventsUUID {
            onEventData?(data)
        } else if characteristic.uuid == Self.deviceInfoUUID {
            do {
                let info = try JSONDecoder().decode(GatewayDeviceInfo.self, from: data)
                if trustStore.isTrusted(peripheralID: peripheral.identifier, info: info) {
                    deviceInfoReady = true
                    cachedDeviceInfo = info
                    pendingEnrollmentCandidate = nil
                    connectionRetryBudget.reset()
                    stopAutomaticReconnect()
                    connectionTimeoutTask?.cancel()
                    connectionState = .connected
                    onDeviceInfo?(info)
                    return
                }
                guard !trustStore.hasPinnedGateway else {
                    report(String(localized: "This is not the paired shore gateway. Forget the saved gateway before pairing a replacement."))
                    resetConnection(state: .idle)
                    return
                }
                if let pendingEnrollmentCandidate,
                   pendingEnrollmentCandidate.peripheralID == peripheral.identifier,
                   pendingEnrollmentCandidate.info == info,
                   pendingEnrollmentCandidate.connectionGeneration == connectionGeneration {
                    return
                }
                let candidate = GatewayEnrollmentCandidate(
                    peripheralID: peripheral.identifier,
                    info: info,
                    connectionGeneration: connectionGeneration
                )
                pendingEnrollmentCandidate = candidate
                connectionTimeoutTask?.cancel()
                connectionState = .confirming
                onEnrollmentCandidate?(candidate)
            } catch {
                report(String(localized: "Gateway identity could not be decoded."))
            }
        }
    }

    private func armConnectionTimeout(while expectedState: ConnectionState) {
        connectionTimeoutTask?.cancel()
        connectionTimeoutTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(12))
            guard let self, !Task.isCancelled, self.connectionState == expectedState else { return }
            if self.recoveringUnexpectedDisconnect {
                self.resetConnection(state: .idle)
                self.scheduleAutomaticReconnect()
                return
            }
            if self.connectionRetryBudget.consumeSoftRetry() {
                self.resetConnection(state: .idle)
                self.startScanning()
            } else {
                self.resetConnection(state: .idle)
                self.report(String(localized: "Gateway connection timed out. Tap Connect to retry."))
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard isCurrent(peripheral) else { return }
        guard characteristic.uuid == Self.commandUUID else {
            if let error { report(error.localizedDescription) }
            return
        }
        writeTimeoutTask?.cancel()
        let write = commandWrites.completeInFlight()
        if let write {
            logCommandWrite("didWrite", write: write, error: error)
        } else {
            print("[BLE-TX] unexpected didWrite without in-flight command error=\(error?.localizedDescription ?? "none")")
        }
        if let error {
            if case .alert(let commandID) = write?.kind {
                onAlertCommandWriteFailed?(commandID)
            } else {
                report(error.localizedDescription)
            }
        }
        pumpCommandWrite(peripheral: peripheral, characteristic: characteristic)
    }
}
