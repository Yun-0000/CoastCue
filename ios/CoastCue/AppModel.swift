import Foundation
@preconcurrency import SwiftData
import Observation

@MainActor
@Observable
final class AppModel {
    static let syncPageLimit = 4
    static let retrySyncPageLimit = 1
    private(set) var isGatewayStatusStale = false
    private var statusReceivedAt: Date?
    private var statusTimeoutTask: Task<Void, Never>?
    var gatewayStatus: GatewayStatus?
    var connectionState: BluetoothGatewayService.ConnectionState = .idle
    var lastError: String?
    var syncMessage = String(localized: "Ready to sync")
    var isHistoryTruncated = false
    var enrollmentCandidate: GatewayEnrollmentCandidate?
    private(set) var gatewayIdentity: String?
    private(set) var gatewayFirmware: String?
    private(set) var currentShoreReference: ShoreReference?
    private(set) var hasCompletedCurrentSync = false

    private enum PendingRecord {
        case event(GatewayEvent)
        case transition(GatewayTransition)

        var recordID: Int64 {
            switch self { case .event(let value): value.recordID; case .transition(let value): value.recordID }
        }
    }

    private struct SyncSession {
        let id: String
        let identity: String
        let startedAt: Date
        let connectionGeneration: Int
        var records: [PendingRecord] = []
        var hasFailure = false
    }

    private struct PendingAlertCommand {
        let commandID: String
        let op: String
        let recordID: Int64
        let identity: String
        let connectionGeneration: Int
        let startedAt: Date
        var verificationDeadline: Date?
    }

    private var framer = GatewayMessageFramer()
    private let ble: GatewayBluetoothService
    private let defaults: UserDefaults
    private let syncTimeout: TimeInterval
    private let alertCommandTimeout: TimeInterval
    private let alertCommandVerificationTimeout: TimeInterval
    private let saveAction: (ModelContext) throws -> Void
    private var cursor: Int64 = 0
    private var modelContext: ModelContext?
    private var session: SyncSession?
    private var syncReplayStartedAt: Date?
    private var retryCount = 0
    private var connectionGeneration = 0
    private var identityGeneration: Int?
    private var statusGeneration: Int?
    private var timeoutTask: Task<Void, Never>?
    private var pendingAlertCommands: [String: PendingAlertCommand] = [:]
    private var alertCommandTimeoutTasks: [String: Task<Void, Never>] = [:]

    init(
        ble: GatewayBluetoothService,
        defaults: UserDefaults = .standard,
        syncTimeout: TimeInterval = 20,
        alertCommandTimeout: TimeInterval = 8,
        alertCommandVerificationTimeout: TimeInterval = 2,
        saveAction: @escaping (ModelContext) throws -> Void = { try $0.save() }
    ) {
        self.ble = ble
        self.defaults = defaults
        self.syncTimeout = syncTimeout
        self.alertCommandTimeout = alertCommandTimeout
        self.alertCommandVerificationTimeout = alertCommandVerificationTimeout
        self.saveAction = saveAction
        ble.onStatus = { [weak self] status in
            guard let self, self.identityGeneration == self.connectionGeneration else { return }
            self.gatewayStatus = status
            self.statusGeneration = self.connectionGeneration
            self.statusReceivedAt = .now
            self.isGatewayStatusStale = false
            self.statusTimeoutTask?.cancel()
            self.statusTimeoutTask = Task { [weak self] in
                try? await Task.sleep(for: .seconds(10))
                guard let self, !Task.isCancelled else { return }
                self.checkGatewayStatusTimeout()
            }
            self.confirmPendingAlertCommands(from: status)
            self.reconcile(status)
        }
        ble.onDeviceInfo = { [weak self] info in self?.acceptDeviceInfo(info) }
        ble.onEnrollmentCandidate = { [weak self] candidate in self?.requestEnrollment(candidate) }
        ble.onConnectionChanged = { [weak self] state in self?.connectionChanged(state) }
        ble.onError = { [weak self] message in self?.lastError = message }
        ble.onAlertCommandWriteFailed = { [weak self] commandID in
            self?.failPendingAlertCommand(
                commandID: commandID,
                message: String(localized: "The alert action could not be written to the shore gateway. Try again.")
            )
        }
        ble.onEventData = { [weak self] data in
            guard let self,
                  self.gatewayIdentity != nil,
                  self.identityGeneration == self.connectionGeneration else { return }
            self.receive(data)
        }
    }

    func connectAndSync(context: ModelContext) {
        modelContext = context
        retryCount = 0
        connectionGeneration += 1
        identityGeneration = nil
        statusGeneration = nil
        statusTimeoutTask?.cancel()
        statusReceivedAt = nil
        isGatewayStatusStale = false
        hasCompletedCurrentSync = false
        session = nil
        syncReplayStartedAt = nil
        clearPendingAlertCommands()
        framer = GatewayMessageFramer()
        timeoutTask?.cancel()
        lastError = nil
        isHistoryTruncated = false
        enrollmentCandidate = nil
        syncMessage = String(localized: "Reading gateway identity…")
        ble.connect()
    }

    func acknowledge(_ alert: MarineAlert) {
        guard alert.gatewayIdentity == gatewayIdentity else {
            lastError = String(localized: "Connect to the gateway before changing an alert.")
            return
        }
        acknowledge(recordID: alert.recordID)
    }

    func resolve(_ alert: MarineAlert) {
        guard alert.gatewayIdentity == gatewayIdentity else {
            lastError = String(localized: "Connect to the gateway before changing an alert.")
            return
        }
        resolve(recordID: alert.recordID)
    }

    func acknowledge(recordID: Int64) {
        sendAlertCommand(op: "ack", recordID: recordID)
    }

    func resolve(recordID: Int64) {
        sendAlertCommand(op: "resolve", recordID: recordID)
    }

    private func sendAlertCommand(op: String, recordID: Int64) {
        guard canSendAlertCommand, let identity = gatewayIdentity, recordID > 0 else {
            lastError = String(localized: "Connect to the gateway before changing an alert.")
            return
        }
        let commandID = UUID().uuidString
        pendingAlertCommands[commandID] = PendingAlertCommand(
            commandID: commandID,
            op: op,
            recordID: recordID,
            identity: identity,
            connectionGeneration: connectionGeneration,
            startedAt: .now,
            verificationDeadline: nil
        )
        scheduleAlertCommandTimeout(commandID: commandID)
        let accepted: Bool
        if op == "ack" {
            accepted = ble.acknowledge(recordID: recordID, commandID: commandID)
        } else {
            accepted = ble.resolve(recordID: recordID, commandID: commandID)
        }
        guard accepted else {
            pendingAlertCommands[commandID] = nil
            alertCommandTimeoutTasks.removeValue(forKey: commandID)?.cancel()
            if lastError == nil {
                lastError = String(localized: "The alert action could not be sent to the shore gateway. Try again.")
            }
            return
        }
    }

    func disconnect() { ble.disconnect() }

    func confirmEnrollment(_ candidate: GatewayEnrollmentCandidate) {
        guard enrollmentCandidate == candidate else { return }
        ble.confirmEnrollment(candidate)
    }

    func cancelEnrollment(_ candidate: GatewayEnrollmentCandidate) {
        guard enrollmentCandidate == candidate else { return }
        enrollmentCandidate = nil
        ble.cancelEnrollment(candidate)
    }

    func forgetGateway(context: ModelContext) {
        if let identity = gatewayIdentity ?? ble.savedGatewayIdentity {
            do {
                for snapshot in try context.fetch(FetchDescriptor<BoatSnapshot>()) where snapshot.gatewayIdentity == identity {
                    context.delete(snapshot)
                }
                for alert in try context.fetch(FetchDescriptor<MarineAlert>()) where alert.gatewayIdentity == identity {
                    context.delete(alert)
                }
                try saveAction(context)
                defaults.removeObject(forKey: cursorKey(identity))
                defaults.removeObject(forKey: shoreReferenceKey(identity))
                cursor = 0
                currentShoreReference = nil
                isHistoryTruncated = false
            } catch {
                context.rollback()
                lastError = error.localizedDescription
                return
            }
        }
        ble.forgetGateway()
        enrollmentCandidate = nil
        gatewayIdentity = nil
        gatewayFirmware = nil
        gatewayStatus = nil
        hasCompletedCurrentSync = false
        clearPendingAlertCommands()
        syncMessage = String(localized: "Ready to sync")
    }

    var canSendAlertCommand: Bool {
        identityGeneration == connectionGeneration &&
            (connectionState == .connected || connectionState == .syncing || connectionState == .syncFailed)
    }
    var isAlertCommandPending: Bool { !pendingAlertCommands.isEmpty }
    func isAlertCommandPending(recordID: Int64) -> Bool {
        pendingAlertCommands.values.contains { $0.recordID == recordID }
    }
    func isAlertCommandPending(recordID: Int64, operation: String) -> Bool {
        pendingAlertCommands.values.contains {
            $0.recordID == recordID && $0.op == operation
        }
    }
    var cursorForTesting: Int64 { cursor }

    @discardableResult
    func saveShoreReference(_ reference: ShoreReference) -> Bool {
        guard let identity = gatewayIdentity,
              let data = try? JSONEncoder().encode(reference) else {
            return false
        }
        defaults.set(data, forKey: shoreReferenceKey(identity))
        currentShoreReference = reference
        return true
    }

    func clearCurrentShoreReference() {
        guard let identity = gatewayIdentity else { return }
        defaults.removeObject(forKey: shoreReferenceKey(identity))
        currentShoreReference = nil
    }

    func shoreReference(for gatewayIdentity: String) -> ShoreReference? {
        if gatewayIdentity == self.gatewayIdentity {
            return currentShoreReference
        }
        return loadShoreReference(for: gatewayIdentity)
    }

    func checkSyncTimeout(now: Date = .now) {
        guard let session, now.timeIntervalSince(session.startedAt) > syncTimeout else { return }
        failPage(String(localized: "Gateway sync timed out. Retrying once…"))
    }

    func checkAlertCommandTimeout(now: Date = .now) {
        let awaitingStatus = pendingAlertCommands.values.filter {
            $0.verificationDeadline == nil && now.timeIntervalSince($0.startedAt) >= alertCommandTimeout
        }
        for command in awaitingStatus {
            guard var pending = pendingAlertCommands[command.commandID] else { continue }
            pending.verificationDeadline = now.addingTimeInterval(alertCommandVerificationTimeout)
            pendingAlertCommands[command.commandID] = pending
            scheduleAlertCommandTimeout(commandID: command.commandID, after: alertCommandVerificationTimeout)
        }
        if !awaitingStatus.isEmpty {
            ble.readStatus()
        }

        let unconfirmed = pendingAlertCommands.values.filter {
            guard let verificationDeadline = $0.verificationDeadline else { return false }
            return now >= verificationDeadline
        }
        guard !unconfirmed.isEmpty else { return }
        for command in unconfirmed {
            pendingAlertCommands[command.commandID] = nil
            alertCommandTimeoutTasks.removeValue(forKey: command.commandID)?.cancel()
        }
        lastError = String(localized: "The gateway did not confirm the alert action. Try again.")
    }

    private func connectionChanged(_ state: BluetoothGatewayService.ConnectionState) {
        if state == .connected,
           connectionState == .syncFailed,
           !hasCompletedCurrentSync {
            return
        }
        connectionState = state
        if state == .idle || state == .unavailable {
            enrollmentCandidate = nil
            connectionGeneration += 1
            statusGeneration = nil
            statusTimeoutTask?.cancel()
            statusReceivedAt = nil
            isGatewayStatusStale = false
            hasCompletedCurrentSync = false
            framer = GatewayMessageFramer()
            session = nil
            syncReplayStartedAt = nil
            clearPendingAlertCommands()
            timeoutTask?.cancel()
            syncMessage = String(localized: "Ready to sync")
        }
    }

    private func acceptDeviceInfo(_ info: GatewayDeviceInfo) {
        clearRecoveredConnectionTimeout()
        let identity = info.identity
        gatewayFirmware = info.firmware
        enrollmentCandidate = nil
        if gatewayIdentity == identity, session != nil, identityGeneration == connectionGeneration {
            return
        }
        if gatewayIdentity != identity {
            gatewayIdentity = identity
            hasCompletedCurrentSync = false
            statusGeneration = nil
            statusTimeoutTask?.cancel()
            statusReceivedAt = nil
            isGatewayStatusStale = false
            clearPendingAlertCommands()
            cursor = Int64(defaults.integer(forKey: cursorKey(identity)))
            currentShoreReference = loadShoreReference(for: identity)
            session = nil
            syncReplayStartedAt = nil
            framer = GatewayMessageFramer()
        }
        identityGeneration = connectionGeneration
        // The initial status read can arrive before identity trust is
        // established, so request it again once this gateway is confirmed.
        ble.readStatus()
        beginSync()
    }

    private func requestEnrollment(_ candidate: GatewayEnrollmentCandidate) {
        clearRecoveredConnectionTimeout()
        enrollmentCandidate = candidate
        connectionState = .confirming
        syncMessage = String(localized: "Confirm shore gateway pairing to begin syncing.")
    }

    private func beginSync() {
        guard let identity = gatewayIdentity else {
            lastError = String(localized: "Gateway identity is unavailable.")
            return
        }
        let id = UUID().uuidString
        let startedAt = Date.now
        syncReplayStartedAt = syncReplayStartedAt ?? startedAt
        session = SyncSession(id: id, identity: identity, startedAt: startedAt, connectionGeneration: connectionGeneration)
        timeoutTask?.cancel()
        let generation = connectionGeneration
        timeoutTask = Task { [weak self] in
            guard let self else { return }
            try? await Task.sleep(for: .seconds(self.syncTimeout))
            guard !Task.isCancelled, self.connectionGeneration == generation else { return }
            self.checkSyncTimeout()
        }
        connectionState = .syncing
        syncMessage = String(localized: "Syncing latest gateway records…")
        let pageLimit = retryCount == 0 ? Self.syncPageLimit : Self.retrySyncPageLimit
        ble.sync(after: cursor, limit: pageLimit, syncID: id)
    }

    private func receive(_ data: Data) {
        do {
            guard let json = try framer.ingest(data) else { return }
            let envelope = try JSONDecoder().decode(FrameEnvelope.self, from: json)
            do {
                switch envelope.kind {
                case "event": try receive(try JSONDecoder().decode(GatewayEvent.self, from: json))
                case "transition": try receive(try JSONDecoder().decode(GatewayTransition.self, from: json))
                case "sync_complete": try receive(try JSONDecoder().decode(SyncComplete.self, from: json))
                case "command_result": apply(try JSONDecoder().decode(CommandResult.self, from: json))
                default: break
                }
            } catch {
                if envelope.syncID == session?.id { session?.hasFailure = true }
                throw error
            }
        } catch {
            lastError = error.localizedDescription
        }
    }

    private func receive(_ event: GatewayEvent) throws {
        if let syncID = event.syncID {
            guard let session, session.id == syncID, session.connectionGeneration == connectionGeneration else { return }
            guard session.records.count < 20 else {
                self.session?.hasFailure = true
                return
            }
            var updated = session
            updated.records.append(.event(event))
            self.session = updated
        } else {
            try save([.event(event)])
            advanceLiveCursor(to: event.recordID, isDurable: event.isDurable)
        }
    }

    private func receive(_ transition: GatewayTransition) throws {
        if let syncID = transition.syncID {
            guard let session, session.id == syncID, session.connectionGeneration == connectionGeneration else { return }
            guard session.records.count < 20 else {
                self.session?.hasFailure = true
                return
            }
            var updated = session
            updated.records.append(.transition(transition))
            self.session = updated
        } else {
            try save([.transition(transition)])
            // The shore publishes transitions only after their journal append.
            advanceLiveCursor(to: transition.recordID, isDurable: true)
        }
    }

    private func receive(_ complete: SyncComplete) throws {
        guard let session, complete.syncID == session.id, gatewayIdentity == session.identity, session.connectionGeneration == connectionGeneration else { return }
        let records = session.records.sorted { $0.recordID < $1.recordID }
        guard !session.hasFailure,
              records.count == complete.count,
              pageIDsAreValid(records, complete: complete, after: cursor),
              (records.last?.recordID ?? cursor) == complete.lastRecordID,
              !complete.hasMore || complete.lastRecordID > cursor else {
            failPage(String(localized: "Gateway sync page was incomplete. Retrying once…"))
            return
        }
        do {
            try save(records, syncedAt: syncReplayStartedAt ?? session.startedAt)
            cursor = complete.lastRecordID
            defaults.set(cursor, forKey: cursorKey(session.identity))
            self.session = nil
            timeoutTask?.cancel()
            retryCount = 0
            isHistoryTruncated = isHistoryTruncated || (complete.truncated ?? false)
            if complete.storageReady == false { lastError = String(localized: "Gateway storage is unavailable.") }
            if complete.hasMore {
                syncMessage = String(localized: "Syncing additional records…")
                beginSync()
            } else {
                clearRecoveredSyncError()
                hasCompletedCurrentSync = true
                syncReplayStartedAt = nil
                ble.finishSync()
                connectionState = .connected
                reconcile(currentGatewayStatus)
                syncMessage = isHistoryTruncated ? String(localized: "Gateway history is truncated") : String(localized: "Sync complete")
            }
        } catch {
            failPage(error.localizedDescription)
        }
    }

    private func failPage(_ message: String) {
        lastError = message
        timeoutTask?.cancel()
        guard retryCount < 1 else {
            session = nil
            syncReplayStartedAt = nil
            ble.finishSync()
            connectionState = .syncFailed
            let terminalMessage = String(localized: "Sync failed. Tap Sync to retry.")
            lastError = terminalMessage
            syncMessage = terminalMessage
            return
        }
        retryCount += 1
        beginSync()
    }

    private func apply(_ result: CommandResult) {
        guard let commandID = result.commandID,
              let pendingAlertCommand = pendingAlertCommands[commandID],
              gatewayIdentity == pendingAlertCommand.identity,
              connectionGeneration == pendingAlertCommand.connectionGeneration else { return }
        if let op = result.op, op != pendingAlertCommand.op { return }
        pendingAlertCommands[commandID] = nil
        alertCommandTimeoutTasks.removeValue(forKey: commandID)?.cancel()
        guard result.ok == true else {
            lastError = localizedAlertCommandError(result.error)
            handleAlertCommandFailure(error: result.error)
            return
        }
        let successfulStatus: AlertStatus = pendingAlertCommand.op == "ack" ? .acknowledged : .resolved
        let recordID = result.recordID ?? pendingAlertCommand.recordID
        advanceAlert(recordID: pendingAlertCommand.recordID, status: successfulStatus)
        if recordID != pendingAlertCommand.recordID {
            advanceAlert(recordID: recordID, status: successfulStatus)
        }
        advanceStatusBackedAlert(recordID: pendingAlertCommand.recordID, status: successfulStatus)
        advanceStatusBackedAlert(recordID: recordID, status: successfulStatus)
        // Status-backed alerts have no local MarineAlert row to advance. Read
        // the authoritative status after a correlated success so their Rescue
        // presentation does not depend on a separate best-effort notification.
        ble.readStatus()
    }

    private func failPendingAlertCommand(commandID: String, message: String) {
        guard pendingAlertCommands[commandID] != nil else { return }
        pendingAlertCommands[commandID] = nil
        alertCommandTimeoutTasks.removeValue(forKey: commandID)?.cancel()
        lastError = message
    }

    private func localizedAlertCommandError(_ code: String?) -> String {
        switch code {
        case "busy":
            String(localized: "The shore gateway is busy. Try again in a moment.")
        case "alert_not_active":
            String(localized: "This alert is no longer active on the shore gateway. Sync to refresh.")
        case "persistence_failed":
            String(localized: "Shore gateway storage failed. The alert was not changed.")
        case .some(let code) where !code.isEmpty:
            code
        default:
            String(localized: "Gateway command failed")
        }
    }

    private func handleAlertCommandFailure(error: String?) {
        switch error {
        case "alert_not_active":
            ble.readStatus()
            if session == nil {
                beginSync()
            }
        case "persistence_failed":
            ble.readStatus()
        default:
            break
        }
    }

    private func advanceStatusBackedAlert(recordID: Int64, status: AlertStatus) {
        guard let current = currentGatewayStatus,
              current.activeRecordID == recordID,
              let currentState = current.alertState.flatMap(AlertStatus.init(rawValue:)),
              alertStatusMayAdvance(from: currentState, to: status) else { return }
        gatewayStatus = GatewayStatus(
            storageReady: current.storageReady,
            journalCount: current.journalCount,
            latestNodeUID: current.latestNodeUID,
            lastPacketAgeMS: current.lastPacketAgeMS,
            radioHealth: current.radioHealth,
            rssi: current.rssi,
            snr: current.snr,
            alertState: status.rawValue,
            activeRecordID: current.activeRecordID,
            severity: current.severity,
            pairingOpen: current.pairingOpen
        )
    }

    private func alertStatusMayAdvance(from current: AlertStatus, to requested: AlertStatus) -> Bool {
        switch (current, requested) {
        case (.open, .acknowledged), (.open, .resolved), (.acknowledged, .resolved),
             (.open, .superseded), (.acknowledged, .superseded): true
        default: false
        }
    }

    private func scheduleAlertCommandTimeout(
        commandID: String,
        after delay: TimeInterval? = nil
    ) {
        alertCommandTimeoutTasks.removeValue(forKey: commandID)?.cancel()
        let timeout = delay ?? alertCommandTimeout
        alertCommandTimeoutTasks[commandID] = Task { [weak self] in
            try? await Task.sleep(for: .seconds(timeout))
            guard let self, !Task.isCancelled,
                  self.pendingAlertCommands[commandID] != nil else { return }
            self.checkAlertCommandTimeout()
        }
    }

    private func clearPendingAlertCommands() {
        for task in alertCommandTimeoutTasks.values { task.cancel() }
        alertCommandTimeoutTasks.removeAll()
        pendingAlertCommands.removeAll()
    }

    private func clearRecoveredConnectionTimeout() {
        let recoveredMessages = [
            String(localized: "Gateway connection timed out. Tap Connect to retry."),
            String(localized: "Gateway write timed out. Reconnecting to check its state."),
            String(localized: "Gateway connection was lost. Tap Connect to retry."),
        ]
        if let lastError, recoveredMessages.contains(lastError) { self.lastError = nil }
    }

    private func clearRecoveredSyncError() {
        let recoveredMessages = [
            String(localized: "Gateway sync timed out. Retrying once…"),
            String(localized: "Gateway sync page was incomplete. Retrying once…"),
        ]
        if let lastError, recoveredMessages.contains(lastError) {
            self.lastError = nil
        }
    }

    private func reconcile(_ status: GatewayStatus?) {
        guard let status, let recordID = status.activeRecordID, let rawState = status.alertState,
              let alertStatus = AlertStatus(rawValue: rawState) else { return }
        advanceAlert(recordID: recordID, status: alertStatus)
    }

    private func confirmPendingAlertCommands(from status: GatewayStatus) {
        guard let recordID = status.activeRecordID,
              let rawState = status.alertState,
              let alertStatus = AlertStatus(rawValue: rawState) else { return }
        let confirmedIDs = pendingAlertCommands.values.compactMap { command -> String? in
            switch (command.op, alertStatus) {
            case ("ack", .acknowledged), ("ack", .resolved), ("resolve", .resolved):
                if command.recordID == recordID {
                    return command.commandID
                }
                return nil
            default:
                return nil
            }
        }
        for commandID in confirmedIDs {
            pendingAlertCommands[commandID] = nil
            alertCommandTimeoutTasks.removeValue(forKey: commandID)?.cancel()
        }
    }

    private func advanceAlert(recordID: Int64, status: AlertStatus) {
        guard let modelContext, let identity = gatewayIdentity else { return }
        do {
            let descriptor = FetchDescriptor<MarineAlert>(predicate: #Predicate {
                $0.gatewayIdentity == identity && $0.recordID == recordID
            })
            var changed = false
            for alert in try modelContext.fetch(descriptor) {
                if alertStatusMayAdvance(from: alert.status, to: status) {
                    alert.status = status
                    changed = true
                }
            }
            if changed { try saveAction(modelContext) }
        } catch {
            modelContext.rollback()
            lastError = error.localizedDescription
        }
    }

    private func advance(_ alert: MarineAlert, to status: AlertStatus) {
        if alertStatusMayAdvance(from: alert.status, to: status) { alert.status = status }
    }

    private func save(_ records: [PendingRecord], syncedAt: Date = .now) throws {
        guard let context = modelContext, let identity = gatewayIdentity, identityGeneration == connectionGeneration else { throw GatewaySyncError.identityUnavailable }
        do {
            for record in records {
                switch record {
                case .event(let event): try save(event, identity: identity, syncedAt: syncedAt, context: context)
                case .transition(let transition): try apply(transition, identity: identity, context: context)
                }
            }
            try saveAction(context)
        } catch {
            context.rollback()
            throw error
        }
    }

    private func save(_ event: GatewayEvent, identity: String, syncedAt: Date, context: ModelContext) throws {
        let nodeUID = event.nodeUID ?? "Unknown"
        let boatID = event.boatID
        let nonce = event.nonce ?? String(event.recordID)
        let newEventKey = "\(identity):\(nodeUID):\(nonce)"
        let isDurable = event.isDurable
        var snapshotQuery = FetchDescriptor<BoatSnapshot>(predicate: #Predicate {
            $0.eventKey == newEventKey ||
                ($0.gatewayIdentity == identity && $0.nodeUID == nodeUID && $0.nonce == nonce)
        })
        snapshotQuery.fetchLimit = 1
        let existingSnapshot = try context.fetch(snapshotQuery).first
        let eventKey = existingSnapshot?.eventKey ?? newEventKey
        if let snapshot = existingSnapshot {
            // A recovered emergency checkpoint has the same semantic key as
            // its live-only notification, but gets a new persistent ID once
            // LittleFS is available again. Promote that existing row in place
            // so all UI and command lookups follow the durable gateway ID.
            // Live replays must never overwrite durable data.
            if isDurable {
                update(snapshot, from: event, nodeUID: nodeUID, boatID: boatID, nonce: nonce, syncedAt: syncedAt)
            }
        } else {
            context.insert(BoatSnapshot(eventKey: eventKey, gatewayIdentity: identity, recordID: event.recordID, nodeUID: nodeUID, boatID: boatID, nonce: nonce, occurredAt: event.occurredAt, syncedAt: syncedAt, timeQuality: event.timeQuality ?? "uptime_only", latitude: event.latitude, longitude: event.longitude, gpsAgeSeconds: event.gpsAgeSeconds, incidentID: event.incidentID, batteryPercent: event.batteryPercent, rssi: event.rssi, snr: event.snr, payload: event.packetB64 ?? "", durable: isDurable))
        }
        let supersededRecordID = event.supersedesRecordID ?? -1
        let alerts = try context.fetch(FetchDescriptor<MarineAlert>(predicate: #Predicate {
            $0.gatewayIdentity == identity && ($0.eventKey == eventKey || $0.recordID == supersededRecordID)
        }))
        if let supersededID = event.supersedesRecordID {
            for alert in alerts where alert.gatewayIdentity == identity && alert.recordID == supersededID { alert.status = .superseded }
        }
        let emergency = event.status
        if let alert = alerts.first(where: { $0.eventKey == eventKey }) {
            // Keep acknowledgements, resolutions, and supersession from
            // regressing when a journal replay supplies the original
            // emergency state again. An existing alert is repaired even if a
            // compact replay omits its status field.
            if isDurable {
                update(alert, from: event, nodeUID: nodeUID, boatID: boatID, syncedAt: syncedAt, title: emergency ?? alert.title)
                if emergency == "help" || emergency == "sos" {
                    let initialStatus = event.alertState.flatMap(AlertStatus.init(rawValue:)) ?? .open
                    advance(alert, to: initialStatus)
                }
            }
        } else if emergency == "help" || emergency == "sos" {
            let initialStatus = event.alertState.flatMap(AlertStatus.init(rawValue:)) ?? .open
            context.insert(MarineAlert(eventKey: eventKey, gatewayIdentity: identity, recordID: event.recordID, nodeUID: nodeUID, boatID: boatID, title: emergency ?? "help", detail: String(localized: "Emergency received by the shore gateway"), occurredAt: event.occurredAt, syncedAt: syncedAt, status: initialStatus, incidentID: event.incidentID))
        }
    }

    private func update(_ snapshot: BoatSnapshot, from event: GatewayEvent, nodeUID: String, boatID: String?, nonce: String, syncedAt: Date) {
        snapshot.recordID = event.recordID
        snapshot.nodeUID = nodeUID
        if let boatID, !boatID.isEmpty { snapshot.boatID = boatID }
        snapshot.nonce = nonce
        snapshot.receivedAt = event.occurredAt ?? syncedAt
        snapshot.occurredAt = event.occurredAt
        snapshot.syncedAt = syncedAt
        snapshot.timeQuality = event.timeQuality ?? "uptime_only"
        snapshot.latitude = event.latitude
        snapshot.longitude = event.longitude
        snapshot.gpsAgeSeconds = event.gpsAgeSeconds
        snapshot.incidentID = event.incidentID
        snapshot.batteryPercent = event.batteryPercent
        snapshot.rssi = event.rssi
        snapshot.snr = event.snr
        snapshot.payload = event.packetB64 ?? ""
        snapshot.durable = true
    }

    private func update(_ alert: MarineAlert, from event: GatewayEvent, nodeUID: String, boatID: String?, syncedAt: Date, title: String) {
        alert.recordID = event.recordID
        alert.nodeUID = nodeUID
        if let boatID, !boatID.isEmpty { alert.boatID = boatID }
        alert.title = title
        alert.detail = String(localized: "Emergency received by the shore gateway")
        alert.createdAt = event.occurredAt ?? syncedAt
        alert.occurredAt = event.occurredAt
        alert.syncedAt = syncedAt
        alert.incidentID = event.incidentID
    }

    private func apply(_ transition: GatewayTransition, identity: String, context: ModelContext) throws {
        let recordID = transition.targetRecordID
        let descriptor = FetchDescriptor<MarineAlert>(predicate: #Predicate {
            $0.gatewayIdentity == identity && $0.recordID == recordID
        })
        for alert in try context.fetch(descriptor) { advance(alert, to: transition.state) }
    }

    private func cursorKey(_ identity: String) -> String { "coastcue.cursor.\(identity)" }
    private func shoreReferenceKey(_ identity: String) -> String { "coastcue.shore-reference.\(identity)" }

    private func loadShoreReference(for identity: String) -> ShoreReference? {
        guard let data = defaults.data(forKey: shoreReferenceKey(identity)) else { return nil }
        return try? JSONDecoder().decode(ShoreReference.self, from: data)
    }

    private func advanceLiveCursor(to recordID: Int64, isDurable: Bool) {
        // Live notifications can be dropped or delivered around a sync page.
        // Advance only across a proven contiguous, durable record when no page
        // is in flight; the next sync still repairs all other ordering cases.
        guard isDurable, session == nil, recordID == cursor + 1,
              let identity = gatewayIdentity else { return }
        cursor = recordID
        defaults.set(cursor, forKey: cursorKey(identity))
    }
    private func pageIDsAreValid(_ records: [PendingRecord], complete: SyncComplete, after: Int64) -> Bool {
        journalPageIDsAreValid(
            records.map(\.recordID), after: after,
            first: complete.firstRecordID, last: complete.lastRecordID,
            truncatedBelow: complete.truncated == true ? complete.truncatedBeforeRecordID : nil,
            missing: complete.missingRecordIDRanges ?? []
        )
    }

    private struct FrameEnvelope: Decodable {
        let kind: String
        let syncID: String?

        enum CodingKeys: String, CodingKey {
            case kind
            case syncID = "sync_id"
        }
    }

    func checkGatewayStatusTimeout(now: Date = .now) {
        guard let statusReceivedAt, now.timeIntervalSince(statusReceivedAt) >= 10 else { return }
        isGatewayStatusStale = true
    }

    var hasCurrentGatewaySnapshot: Bool {
        hasCompletedCurrentSync && currentGatewayStatus != nil && connectionState == .connected
    }

    var currentGatewayStatus: GatewayStatus? {
        statusGeneration == connectionGeneration && !isGatewayStatusStale ? gatewayStatus : nil
    }

    var currentGatewayFirmware: String? {
        identityGeneration == connectionGeneration ? gatewayFirmware : nil
    }
}

private enum GatewaySyncError: LocalizedError { case identityUnavailable
    var errorDescription: String? { String(localized: "Gateway identity is unavailable.") }
}
