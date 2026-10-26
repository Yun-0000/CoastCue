import XCTest
import SwiftData
@testable import CoastCue

final class CoastCueTests: XCTestCase {
    @MainActor
    func testSyncUsesBoundedPagesAndContinuesFromCommittedCursor() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)

        let first = try XCTUnwrap(service.syncRequests.last)
        XCTAssertEqual(first.after, 0)
        XCTAssertEqual(first.limit, 4)
        for recordID in 1...4 { service.emit(event(recordID: Int64(recordID), syncID: first.syncID)) }
        service.emit(["kind": "sync_complete", "sync_id": first.syncID, "first_record_id": 1, "last_record_id": 4, "count": 4, "has_more": true])

        let second = try XCTUnwrap(service.syncRequests.last)
        XCTAssertEqual(service.syncRequests.count, 2)
        XCTAssertEqual(second.after, 4)
        XCTAssertEqual(second.limit, 4)
        service.emit(event(recordID: 5, syncID: second.syncID))
        service.emit(["kind": "sync_complete", "sync_id": second.syncID, "first_record_id": 5, "last_record_id": 5, "count": 1, "has_more": false])

        XCTAssertEqual(model.cursorForTesting, 5)
        XCTAssertEqual(try container.mainContext.fetchCount(FetchDescriptor<BoatSnapshot>()), 5)
    }

    @MainActor
    func testSyncDoesNotAdvanceCursorWhenPageIsMissingARecord() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 2, "count": 2, "has_more": false])

        XCTAssertEqual(service.syncRequests.count, 2)
        XCTAssertEqual(service.syncRequests.last?.after, 0)
        XCTAssertEqual(service.syncRequests.last?.limit, 1)
        XCTAssertEqual(try container.mainContext.fetchCount(FetchDescriptor<BoatSnapshot>()), 0)
    }

    @MainActor
    func testSyncDoesNotAdvanceCursorWhenSavingFails() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults(), saveAction: { _ in throw TestSaveError.failed })
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, nodeUID: "boat", nonce: "1"))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])

        XCTAssertEqual(service.syncRequests.count, 2)
        XCTAssertEqual(model.cursorForTesting, 0)
        XCTAssertEqual(try container.mainContext.fetchCount(FetchDescriptor<BoatSnapshot>()), 0)
    }

    @MainActor
    func testGatewayIdentityResetsCursorAndNamespacesRecordLookup() throws {
        let service = FakeGatewayService()
        let defaults = testDefaults()
        let model = AppModel(ble: service, defaults: defaults)
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let firstID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: firstID))
        service.emit(["kind": "sync_complete", "sync_id": firstID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])
        service.emitDeviceInfo(nodeUID: "shore-b", epoch: "fresh")
        let secondID = try XCTUnwrap(service.syncRequests.last?.syncID)

        XCTAssertEqual(service.syncRequests.last?.after, 0)
        service.emit(event(recordID: 1, syncID: secondID))
        service.emit(["kind": "sync_complete", "sync_id": secondID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])
        XCTAssertEqual(try container.mainContext.fetchCount(FetchDescriptor<BoatSnapshot>()), 2)
    }

    @MainActor
    func testSuccessfulSyncFinishesBluetoothSyncState() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)

        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])

        XCTAssertEqual(service.finishSyncRequests, 1)
        XCTAssertEqual(model.connectionState, .connected)
    }

    @MainActor
    func testForegroundResyncRetainsTrustedLocalSnapshotButWithholdsStaleGatewayStatus() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let firstID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(["kind": "sync_complete", "sync_id": firstID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])
        let status = GatewayStatus(storageReady: true, journalCount: 1, latestNodeUID: "old-boat", lastPacketAgeMS: 0, radioHealth: "recent", rssi: -80, snr: 8, alertState: "resolved", activeRecordID: nil, severity: nil, pairingOpen: false)
        service.onStatus?(status)
        XCTAssertEqual(model.currentGatewayStatus?.latestNodeUID, "old-boat")
        XCTAssertTrue(model.hasCurrentGatewaySnapshot)

        model.connectAndSync(context: container.mainContext)

        XCTAssertEqual(model.gatewayIdentity, "gw:epoch")
        XCTAssertEqual(model.gatewayStatus?.latestNodeUID, "old-boat")
        XCTAssertNil(model.currentGatewayStatus)
        XCTAssertEqual(model.currentGatewayFirmware, "test-firmware")
        XCTAssertFalse(model.hasCurrentGatewaySnapshot)
        XCTAssertEqual(
            rescuePresentationState(status: model.currentGatewayStatus, hasLocalAlert: true, isCurrent: model.hasCurrentGatewaySnapshot),
            .activeAlert
        )

        let secondID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.onStatus?(GatewayStatus(storageReady: true, journalCount: 2, latestNodeUID: "new-boat", lastPacketAgeMS: 0, radioHealth: "recent", rssi: -70, snr: 9, alertState: "resolved", activeRecordID: nil, severity: nil, pairingOpen: false))
        XCTAssertEqual(model.currentGatewayStatus?.latestNodeUID, "new-boat")
        XCTAssertFalse(model.hasCurrentGatewaySnapshot)

        service.emit(["kind": "sync_complete", "sync_id": secondID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])
        XCTAssertTrue(model.hasCurrentGatewaySnapshot)
    }

    @MainActor
    func testContiguousDurableLiveEventAdvancesCursorAfterSync() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])

        service.emit(["kind": "event", "record_id": 1, "node_uid": "boat", "nonce": "1", "durable": true])

        XCTAssertEqual(model.cursorForTesting, 1)
        model.connectAndSync(context: container.mainContext)
        XCTAssertEqual(service.syncRequests.last?.after, 1)
    }

    @MainActor
    func testDurableSyncPromotesLiveEmergencyToPersistentRecordIDWithoutDuplicates() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        let context = container.mainContext

        model.connectAndSync(context: context)
        let initialSyncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: initialSyncID, status: "help"))
        service.emit(["kind": "sync_complete", "sync_id": initialSyncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])

        let volatileRecordID: Int64 = 2_147_483_650
        let nonce = "checkpoint-nonce"
        service.emit([
            "kind": "event",
            "record_id": volatileRecordID,
            "node_uid": "boat",
            "nonce": nonce,
            "status": "sos",
            "durable": false,
            "time_quality": "uptime_only",
            "latitude": 30.0,
            "longitude": 120.0,
            "packet_b64": "live",
        ])

        let liveSnapshot = try XCTUnwrap(context.fetch(FetchDescriptor<BoatSnapshot>()).first(where: { $0.nonce == nonce }))
        let liveAlert = try XCTUnwrap(context.fetch(FetchDescriptor<MarineAlert>()).first(where: { $0.eventKey == liveSnapshot.eventKey }))
        XCTAssertEqual(liveSnapshot.recordID, volatileRecordID)
        XCTAssertFalse(liveSnapshot.durable)
        XCTAssertEqual(liveAlert.recordID, volatileRecordID)

        model.connectAndSync(context: context)
        let recoverySyncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.onStatus?(GatewayStatus(
            storageReady: true,
            journalCount: 2,
            latestNodeUID: "boat",
            lastPacketAgeMS: 0,
            radioHealth: "recent",
            rssi: -70,
            snr: 9,
            alertState: "open",
            activeRecordID: 2,
            severity: "sos",
            pairingOpen: false
        ))
        service.emit(event(
            recordID: 2,
            syncID: recoverySyncID,
            nonce: nonce,
            status: "sos",
            extras: [
                "durable": true,
                "time_quality": "gps",
                "latitude": 31.2,
                "longitude": 121.5,
                "packet_b64": "durable",
                "supersedes_record_id": 1,
            ]
        ))
        service.emit(["kind": "sync_complete", "sync_id": recoverySyncID, "first_record_id": 2, "last_record_id": 2, "count": 1, "has_more": false])

        let snapshots = try context.fetch(FetchDescriptor<BoatSnapshot>())
        let alerts = try context.fetch(FetchDescriptor<MarineAlert>())
        let promotedSnapshot = try XCTUnwrap(snapshots.first(where: { $0.nonce == nonce }))
        let promotedAlert = try XCTUnwrap(alerts.first(where: { $0.eventKey == promotedSnapshot.eventKey }))
        XCTAssertEqual(snapshots.count, 2)
        XCTAssertEqual(alerts.count, 2)
        XCTAssertEqual(promotedSnapshot.recordID, 2)
        XCTAssertTrue(promotedSnapshot.durable)
        XCTAssertEqual(promotedSnapshot.timeQuality, "gps")
        XCTAssertEqual(promotedSnapshot.latitude, 31.2)
        XCTAssertEqual(promotedSnapshot.longitude, 121.5)
        XCTAssertEqual(promotedSnapshot.payload, "durable")
        XCTAssertEqual(promotedAlert.recordID, 2)
        XCTAssertEqual(alerts.first(where: { $0.recordID == 1 })?.status, .superseded)
        XCTAssertEqual(model.currentGatewayStatus?.activeRecordID, promotedAlert.recordID)

        model.resolve(promotedAlert)
        XCTAssertEqual(service.alertCommands.last?.recordID, 2)

        promotedAlert.status = .resolved
        try context.save()
        service.emit([
            "kind": "event",
            "record_id": 2,
            "node_uid": "boat",
            "nonce": nonce,
            "status": "sos",
            "durable": true,
            "time_quality": "gps",
            "latitude": 31.2,
            "longitude": 121.5,
            "packet_b64": "durable",
            "supersedes_record_id": 1,
        ])
        XCTAssertEqual(try context.fetch(FetchDescriptor<BoatSnapshot>()).count, 2)
        XCTAssertEqual(try context.fetch(FetchDescriptor<MarineAlert>()).count, 2)
        let replayedAlert = try XCTUnwrap(context.fetch(FetchDescriptor<MarineAlert>()).first(where: { $0.eventKey == promotedAlert.eventKey }))
        XCTAssertEqual(replayedAlert.recordID, 2)
        XCTAssertEqual(replayedAlert.status, .resolved)
    }

    @MainActor
    func testLiveEventDoesNotAdvanceCursorAcrossGapOrDuringSync() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)

        service.emit(["kind": "event", "record_id": 1, "node_uid": "boat", "nonce": "1", "durable": true])
        XCTAssertEqual(model.cursorForTesting, 0)

        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])
        service.emit(["kind": "event", "record_id": 3, "node_uid": "boat", "nonce": "3", "durable": true])
        XCTAssertEqual(model.cursorForTesting, 0)
    }

    @MainActor
    func testMalformedLiveFrameDoesNotPoisonActiveSyncPage() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)

        service.emit(["kind": "event", "record_id": "malformed-live-record"])
        service.emit(event(recordID: 1, syncID: syncID))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])

        XCTAssertEqual(service.syncRequests.count, 1)
        XCTAssertEqual(model.cursorForTesting, 1)
        XCTAssertEqual(try container.mainContext.fetchCount(FetchDescriptor<BoatSnapshot>()), 1)
    }

    @MainActor
    func testMalformedActivePageFrameStillRejectsPage() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)

        service.emit(["kind": "event", "sync_id": syncID, "record_id": "malformed-page-record"])
        service.emit(event(recordID: 1, syncID: syncID))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])

        XCTAssertEqual(service.syncRequests.count, 2)
        XCTAssertEqual(model.cursorForTesting, 0)
        XCTAssertEqual(try container.mainContext.fetchCount(FetchDescriptor<BoatSnapshot>()), 0)
    }

    @MainActor
    func testRepeatedMalformedActivePageFailsClosedWithConsistentMessage() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)

        for attempt in 0..<2 {
            let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
            service.emit(["kind": "event", "sync_id": syncID, "record_id": "malformed-\(attempt)"])
            service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])
        }

        let terminalMessage = String(localized: "Sync failed. Tap Sync to retry.")
        XCTAssertEqual(service.syncRequests.count, 2)
        XCTAssertEqual(model.cursorForTesting, 0)
        XCTAssertEqual(model.syncMessage, terminalMessage)
        XCTAssertEqual(model.lastError, terminalMessage)
        XCTAssertEqual(try container.mainContext.fetchCount(FetchDescriptor<BoatSnapshot>()), 0)
        assertLinkAndSyncPresentationAreConsistent(model)
    }

    @MainActor
    func testTerminalSyncFailureDoesNotPresentConnectedWithSyncFailed() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults(), syncTimeout: 0)
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)

        model.checkSyncTimeout(now: .now.addingTimeInterval(1))
        XCTAssertEqual(model.connectionState, .syncing)
        XCTAssertNotEqual(model.connectionState.displayName, String(localized: "Connected"))

        model.checkSyncTimeout(now: .now.addingTimeInterval(1))

        let terminalMessage = String(localized: "Sync failed. Tap Sync to retry.")
        XCTAssertEqual(model.syncMessage, terminalMessage)
        XCTAssertEqual(model.lastError, terminalMessage)
        XCTAssertEqual(model.connectionState, .syncFailed)
        XCTAssertNotEqual(model.connectionState.displayName, String(localized: "Connected"))
        assertLinkAndSyncPresentationAreConsistent(model)
    }

    @MainActor
    func testStaleConnectedCallbackAfterSyncFailureDoesNotRestoreConnectedPresentation() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults(), syncTimeout: 0)
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        model.checkSyncTimeout(now: .now.addingTimeInterval(1))
        model.checkSyncTimeout(now: .now.addingTimeInterval(1))

        XCTAssertEqual(model.connectionState, .syncFailed)

        service.onConnectionChanged?(.connected)
        let lateSyncID = UUID().uuidString
        service.emit(["kind": "sync_complete", "sync_id": lateSyncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])

        XCTAssertEqual(model.connectionState, .syncFailed)
        XCTAssertEqual(model.syncMessage, String(localized: "Sync failed. Tap Sync to retry."))
        XCTAssertNotEqual(model.connectionState.displayName, String(localized: "Connected"))
        assertLinkAndSyncPresentationAreConsistent(model)
    }

    @MainActor
    func testRetryAfterSyncFailureIgnoresStalePriorSessionCallbacks() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults(), syncTimeout: 0)
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let staleSyncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        model.checkSyncTimeout(now: .now.addingTimeInterval(1))
        model.checkSyncTimeout(now: .now.addingTimeInterval(1))

        model.connectAndSync(context: container.mainContext)
        XCTAssertEqual(model.connectionState, .syncing)

        service.emit(["kind": "sync_complete", "sync_id": staleSyncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])
        XCTAssertEqual(model.connectionState, .syncing)
        XCTAssertNotEqual(model.syncMessage, String(localized: "Sync complete"))

        let freshSyncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        XCTAssertNotEqual(freshSyncID, staleSyncID)
        service.emit(["kind": "sync_complete", "sync_id": freshSyncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])
        XCTAssertEqual(model.connectionState, .connected)
        XCTAssertEqual(model.syncMessage, String(localized: "Sync complete"))
        assertLinkAndSyncPresentationAreConsistent(model)
    }

    @MainActor
    func testDisconnectResetsCompletedSyncMessage() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])
        XCTAssertEqual(model.syncMessage, String(localized: "Sync complete"))

        service.disconnect()

        XCTAssertFalse(model.hasCompletedCurrentSync)
        XCTAssertEqual(model.syncMessage, String(localized: "Ready to sync"))
    }

    @MainActor
    func testRepeatedDeviceInfoDoesNotRestartActiveSync() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        XCTAssertEqual(service.syncRequests.count, 1)
        service.emitDeviceInfo(nodeUID: "gw", epoch: "epoch")
        XCTAssertEqual(service.syncRequests.count, 1)
    }

    @MainActor
    func testTruncatedInternalGapWithoutDeclarationDoesNotAdvanceCursor() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID))
        service.emit(event(recordID: 3, syncID: syncID))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 3, "count": 2, "has_more": false, "truncated": true])
        XCTAssertEqual(model.cursorForTesting, 0)
        XCTAssertEqual(service.syncRequests.count, 2)
    }

    @MainActor
    func testDeclaredTruncatedGapCommitsPage() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID))
        service.emit(event(recordID: 3, syncID: syncID))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 3, "count": 2, "has_more": false, "truncated": true, "missing_record_id_ranges": [[2, 2]]])
        XCTAssertEqual(model.cursorForTesting, 3)
        XCTAssertEqual(service.syncRequests.count, 1)
    }

    @MainActor
    func testLeadingCompactionGapCommitsPageWhenGatewayDeclaresTruncation() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 6, syncID: syncID))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 6,
            "last_record_id": 6,
            "count": 1,
            "has_more": false,
            "truncated": true,
            "truncated_before_record_id": 6,
        ])

        XCTAssertEqual(model.cursorForTesting, 6)
        XCTAssertEqual(service.syncRequests.count, 1)
    }

    @MainActor
    func testRetainedActiveAlertBeforeCompactedTailCommitsPage() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 5, syncID: syncID, status: "sos"))
        service.emit(event(recordID: 100, syncID: syncID))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 5,
            "last_record_id": 100,
            "count": 2,
            "has_more": false,
            "truncated": true,
            "truncated_before_record_id": 100,
        ])

        XCTAssertEqual(model.cursorForTesting, 100)
        XCTAssertEqual(service.syncRequests.count, 1)
    }

    @MainActor
    func testSupersededEmergencyMovesOlderAlertIntoHistory() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit(["kind": "transition", "sync_id": syncID, "record_id": 2, "target_record_id": 1, "state": "superseded"])
        service.emit(event(recordID: 3, syncID: syncID, status: "help"))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 3, "count": 3, "has_more": false])

        let alerts = try container.mainContext.fetch(FetchDescriptor<MarineAlert>())
        XCTAssertEqual(alerts.first(where: { $0.recordID == 1 })?.status, .superseded)
        XCTAssertEqual(alerts.first(where: { $0.recordID == 3 })?.status, .open)
    }

    @MainActor
    func testJournalAcknowledgementDoesNotRegressResolvedAlert() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit(["kind": "transition", "sync_id": syncID, "record_id": 2, "target_record_id": 1, "state": "resolved"])
        service.emit(["kind": "transition", "sync_id": syncID, "record_id": 3, "target_record_id": 1, "state": "acknowledged"])
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 3, "count": 3, "has_more": false])

        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)
        XCTAssertEqual(alert.status, .resolved)
    }

    @MainActor
    func testJournalAcknowledgementDoesNotRegressSupersededAlert() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "help"))
        service.emit(["kind": "transition", "sync_id": syncID, "record_id": 2, "target_record_id": 1, "state": "superseded"])
        service.emit(["kind": "transition", "sync_id": syncID, "record_id": 3, "target_record_id": 1, "state": "acknowledged"])
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 3, "count": 3, "has_more": false])

        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)
        XCTAssertEqual(alert.status, .superseded)
    }

    @MainActor
    func testSelfDescribingEmergencySupersedesOlderAlertWithoutTransition() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit(event(recordID: 2, syncID: syncID, status: "help", extras: ["supersedes_record_id": 1, "alert_state": "resolved", "durable": false]))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 2, "count": 2, "has_more": false])

        let alerts = try container.mainContext.fetch(FetchDescriptor<MarineAlert>())
        XCTAssertEqual(alerts.first(where: { $0.recordID == 1 })?.status, .superseded)
        XCTAssertEqual(alerts.first(where: { $0.recordID == 2 })?.status, .resolved)
        XCTAssertFalse(try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<BoatSnapshot>()).first(where: { $0.recordID == 2 })).durable)
    }

    @MainActor
    func testDuplicateEventRepairsSupersedeAndMissingAlert() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])

        let context = container.mainContext
        context.insert(BoatSnapshot(eventKey: "gw:epoch:boat:2", gatewayIdentity: "gw:epoch", recordID: 2, nodeUID: "boat", nonce: "2", occurredAt: nil, syncedAt: .now, timeQuality: "uptime_only", latitude: nil, longitude: nil, batteryPercent: nil, rssi: nil, snr: nil, payload: ""))
        context.insert(MarineAlert(eventKey: "gw:epoch:boat:1", gatewayIdentity: "gw:epoch", recordID: 1, nodeUID: "boat", title: "help", detail: "", occurredAt: nil, syncedAt: .now, status: .open))
        try context.save()

        service.emit(["kind": "event", "record_id": 2, "node_uid": "boat", "nonce": "2", "status": "sos", "supersedes_record_id": 1, "durable": true])

        let alerts = try context.fetch(FetchDescriptor<MarineAlert>())
        XCTAssertEqual(alerts.first(where: { $0.recordID == 1 })?.status, .superseded)
        XCTAssertEqual(alerts.first(where: { $0.recordID == 2 })?.status, .open)
    }

    @MainActor
    func testBoatIDEnrichmentUpdatesExistingNonceWithoutSplittingRows() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        let context = container.mainContext
        model.connectAndSync(context: context)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)

        service.emit(event(recordID: 1, syncID: syncID, nodeUID: "boat-node", nonce: "same-nonce", status: "sos"))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])
        service.emit([
            "kind": "event", "record_id": 1, "node_uid": "boat-node",
            "boat_id": "FIELD-BOAT", "nonce": "same-nonce", "status": "sos",
            "durable": true,
        ])

        let snapshots = try context.fetch(FetchDescriptor<BoatSnapshot>())
        let alerts = try context.fetch(FetchDescriptor<MarineAlert>())
        XCTAssertEqual(snapshots.count, 1)
        XCTAssertEqual(alerts.count, 1)
        XCTAssertEqual(snapshots.first?.boatID, "FIELD-BOAT")
        XCTAssertEqual(alerts.first?.boatID, "FIELD-BOAT")
        XCTAssertEqual(snapshots.first?.eventKey, alerts.first?.eventKey)
    }

    @MainActor
    func testStatusOnlyActiveIncidentCanSendOperatorCommands() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])
        service.onStatus?(GatewayStatus(
            storageReady: true, journalCount: 1, latestNodeUID: "boat-node",
            lastPacketAgeMS: 0, radioHealth: "recent", rssi: -80, snr: 5,
            alertState: "open", activeRecordID: 42, severity: "sos", pairingOpen: false
        ))

        XCTAssertEqual(
            rescuePresentationState(status: model.currentGatewayStatus, hasLocalAlert: false, isCurrent: model.hasCurrentGatewaySnapshot),
            .activeDetailsPending
        )
        model.acknowledge(recordID: 42)
        model.resolve(recordID: 42)
        XCTAssertEqual(service.alertCommands.map(\.op), ["ack", "resolve"])
        XCTAssertEqual(service.alertCommands.map(\.recordID), [42, 42])
    }

    @MainActor
    func testStatusOnlySuccessfulAcknowledgementRefreshesFallbackPresentation() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])
        service.onStatus?(GatewayStatus(
            storageReady: true, journalCount: 1, latestNodeUID: "boat-node",
            lastPacketAgeMS: 0, radioHealth: "recent", rssi: -80, snr: 5,
            alertState: "open", activeRecordID: 42, severity: "sos", pairingOpen: false
        ))
        let statusReadsBeforeCommand = service.statusReadRequests

        model.acknowledge(recordID: 42)
        let command = try XCTUnwrap(service.alertCommands.last)
        XCTAssertTrue(model.isAlertCommandPending(recordID: 42))
        service.statusOnRead = GatewayStatus(
            storageReady: true, journalCount: 2, latestNodeUID: "boat-node",
            lastPacketAgeMS: 0, radioHealth: "recent", rssi: -80, snr: 5,
            alertState: "acknowledged", activeRecordID: 42, severity: "sos", pairingOpen: false
        )
        service.emit(commandResult(for: command, ok: true))

        XCTAssertEqual(service.statusReadRequests, statusReadsBeforeCommand + 1)
        XCTAssertEqual(model.currentGatewayStatus?.alertState, "acknowledged")
        XCTAssertFalse(model.isAlertCommandPending(recordID: 42))
    }

    @MainActor
    func testTimeoutLeavesSyncRecoverable() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults(), syncTimeout: 0)
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        model.checkSyncTimeout(now: .now.addingTimeInterval(1))

        XCTAssertEqual(service.syncRequests.count, 2)
        XCTAssertNotNil(model.lastError)
    }

    @MainActor
    func testAlertCommandTimeoutAllowsStatusFallbackToConfirmAfterFirstDeadline() throws {
        let service = FakeGatewayService()
        let model = AppModel(
            ble: service,
            defaults: testDefaults(),
            alertCommandTimeout: 10,
            alertCommandVerificationTimeout: 2
        )
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])

        model.acknowledge(recordID: 42)
        let firstDeadline = Date.now.addingTimeInterval(11)
        model.checkAlertCommandTimeout(now: firstDeadline)

        XCTAssertTrue(model.isAlertCommandPending(recordID: 42))
        XCTAssertEqual(service.statusReadRequests, 2)
        XCTAssertNil(model.lastError)

        service.onStatus?(GatewayStatus(
            storageReady: true, journalCount: 1, latestNodeUID: "boat-node",
            lastPacketAgeMS: 0, radioHealth: "recent", rssi: -80, snr: 5,
            alertState: "acknowledged", activeRecordID: 42, severity: "sos", pairingOpen: false
        ))

        XCTAssertFalse(model.isAlertCommandPending)
        XCTAssertNil(model.lastError)
    }

    @MainActor
    func testAlertCommandTimeoutReportsFailureOnlyAfterVerificationDeadline() throws {
        let service = FakeGatewayService()
        let model = AppModel(
            ble: service,
            defaults: testDefaults(),
            alertCommandTimeout: 10,
            alertCommandVerificationTimeout: 2
        )
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])

        model.resolve(recordID: 42)
        let firstDeadline = Date.now.addingTimeInterval(11)
        model.checkAlertCommandTimeout(now: firstDeadline)

        XCTAssertTrue(model.isAlertCommandPending(recordID: 42))
        XCTAssertNil(model.lastError)

        model.checkAlertCommandTimeout(now: firstDeadline.addingTimeInterval(2))

        XCTAssertFalse(model.isAlertCommandPending)
        XCTAssertEqual(
            model.lastError,
            String(localized: "The gateway did not confirm the alert action. Try again.")
        )
    }

    @MainActor
    func testFailedAlertCommandSendDoesNotLeavePendingCommand() throws {
        let service = FakeGatewayService()
        service.alertCommandSendShouldFail = true
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 1,
            "last_record_id": 1,
            "count": 1,
            "has_more": false,
        ])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)

        model.acknowledge(alert)

        XCTAssertTrue(service.alertCommands.isEmpty)
        XCTAssertFalse(model.isAlertCommandPending)
        XCTAssertFalse(model.isAlertCommandPending(recordID: 1))
        XCTAssertEqual(
            model.lastError,
            String(localized: "The gateway command channel is not ready.")
        )
        XCTAssertEqual(alert.status, .open)
        XCTAssertTrue(model.canSendAlertCommand)
    }

    @MainActor
    func testAlertCommandWriteFailureClearsPendingCommandImmediately() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 1,
            "last_record_id": 1,
            "count": 1,
            "has_more": false,
        ])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)

        model.resolve(alert)
        let command = try XCTUnwrap(service.alertCommands.last)
        XCTAssertTrue(model.isAlertCommandPending(recordID: 1))

        service.failAlertCommandWrite(commandID: command.commandID)

        XCTAssertFalse(model.isAlertCommandPending)
        XCTAssertFalse(model.isAlertCommandPending(recordID: 1))
        XCTAssertEqual(
            model.lastError,
            String(localized: "The alert action could not be written to the shore gateway. Try again.")
        )
        XCTAssertEqual(alert.status, .open)
        XCTAssertTrue(model.canSendAlertCommand)
    }

    @MainActor
    func testImmediateAlertCommandResultCannotBeatPendingCorrelation() throws {
        let service = FakeGatewayService()
        service.completeAlertCommandDuringSend = true
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 17, syncID: syncID, status: "sos"))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 17,
            "last_record_id": 17,
            "count": 1,
            "has_more": false,
            "truncated": true,
            "truncated_before_record_id": 17,
        ])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)

        model.acknowledge(alert)

        XCTAssertEqual(alert.status, .acknowledged)
        XCTAssertFalse(model.isAlertCommandPending(recordID: 17))
        XCTAssertNil(model.lastError)
    }

    @MainActor
    func testCommandResultErrorCodesMapToDistinctMessages() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 1,
            "last_record_id": 1,
            "count": 1,
            "has_more": false,
        ])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)

        let busyMessage = String(localized: "The shore gateway is busy. Try again in a moment.")
        let inactiveMessage = String(localized: "This alert is no longer active on the shore gateway. Sync to refresh.")
        let persistenceMessage = String(localized: "Shore gateway storage failed. The alert was not changed.")
        XCTAssertNotEqual(busyMessage, inactiveMessage)
        XCTAssertNotEqual(busyMessage, persistenceMessage)
        XCTAssertNotEqual(inactiveMessage, persistenceMessage)

        model.acknowledge(alert)
        var command = try XCTUnwrap(service.alertCommands.last)
        service.emit(commandResult(for: command, ok: false, error: "busy"))
        XCTAssertEqual(model.lastError, busyMessage)
        XCTAssertFalse(model.isAlertCommandPending)
        XCTAssertTrue(model.canSendAlertCommand)
        XCTAssertEqual(alert.status, .open)

        let statusReadsBeforeInactive = service.statusReadRequests
        let syncCountBeforeInactive = service.syncRequests.count
        model.acknowledge(alert)
        command = try XCTUnwrap(service.alertCommands.last)
        service.emit(commandResult(for: command, ok: false, error: "alert_not_active"))
        XCTAssertEqual(model.lastError, inactiveMessage)
        XCTAssertFalse(model.isAlertCommandPending)
        XCTAssertTrue(model.canSendAlertCommand)
        XCTAssertGreaterThan(service.statusReadRequests, statusReadsBeforeInactive)
        XCTAssertGreaterThan(service.syncRequests.count, syncCountBeforeInactive)
        XCTAssertEqual(alert.status, .open)

        model.resolve(alert)
        command = try XCTUnwrap(service.alertCommands.last)
        service.emit(commandResult(for: command, ok: false, error: "persistence_failed"))
        XCTAssertEqual(model.lastError, persistenceMessage)
        XCTAssertFalse(model.isAlertCommandPending)
        XCTAssertEqual(alert.status, .open)
    }

    @MainActor
    func testSyncedBleTransitionDoesNotRegressAdvancedLocalAlert() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 1,
            "last_record_id": 1,
            "count": 1,
            "has_more": false,
        ])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)

        model.resolve(alert)
        let command = try XCTUnwrap(service.alertCommands.last)
        service.emit(commandResult(for: command, ok: true))
        XCTAssertEqual(alert.status, .resolved)
        XCTAssertFalse(model.isAlertCommandPending)

        model.connectAndSync(context: container.mainContext)
        let replayID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: replayID, status: "sos", extras: ["alert_state": "open"]))
        service.emit([
            "kind": "transition",
            "sync_id": replayID,
            "record_id": 2,
            "target_record_id": 1,
            "state": "acknowledged",
            "source": "ble",
        ])
        service.emit([
            "kind": "sync_complete",
            "sync_id": replayID,
            "first_record_id": 1,
            "last_record_id": 2,
            "count": 2,
            "has_more": false,
        ])

        XCTAssertEqual(alert.status, .resolved)
    }

    @MainActor
    func testSuccessfulRetryClearsRecoveredGatewayIssue() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults(), syncTimeout: 0)
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        model.checkSyncTimeout(now: .now.addingTimeInterval(1))
        XCTAssertNotNil(model.lastError)

        let retryID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(["kind": "sync_complete", "sync_id": retryID, "first_record_id": 0, "last_record_id": 0, "count": 0, "has_more": false])

        XCTAssertNil(model.lastError)
        XCTAssertEqual(model.connectionState, .connected)
    }

    @MainActor
    func testUnsolicitedCommandResultDoesNotMutateAlert() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 1,
            "last_record_id": 1,
            "count": 1,
            "has_more": false,
        ])

        service.emit([
            "kind": "command_result",
            "op": "resolve",
            "record_id": 1,
            "source": "ble",
            "ok": true,
        ])

        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)
        XCTAssertEqual(alert.status, .open)
    }

    @MainActor
    func testOnlyMatchingCommandIDCanCompletePendingAlertCommand() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 1,
            "last_record_id": 1,
            "count": 1,
            "has_more": false,
        ])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)

        model.resolve(alert)
        let command = try XCTUnwrap(service.alertCommands.last)
        service.emit([
            "kind": "command_result",
            "op": "resolve",
            "record_id": 1,
            "command_id": "stale-command",
            "source": "ble",
            "ok": true,
        ])
        XCTAssertEqual(alert.status, .open)

        service.emit([
            "kind": "command_result",
            "op": "resolve",
            "record_id": 1,
            "command_id": command.commandID,
            "source": "ble",
            "ok": true,
        ])
        XCTAssertEqual(alert.status, .resolved)
    }

    @MainActor
    func testCommandResultCorrelatesByCommandIDEvenIfRecordIDDiffers() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 1,
            "last_record_id": 1,
            "count": 1,
            "has_more": false,
        ])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)

        model.acknowledge(alert)
        let command = try XCTUnwrap(service.alertCommands.last)
        service.emit([
            "kind": "command_result",
            "op": "ack",
            "record_id": 99,
            "command_id": command.commandID,
            "ok": true,
        ])

        XCTAssertEqual(alert.status, .acknowledged)
        XCTAssertFalse(model.isAlertCommandPending(recordID: 1))
        XCTAssertNil(model.lastError)
    }

    @MainActor
    func testAuthoritativeStatusCompletesCommandWhenCommandResultIsLost() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 1,
            "last_record_id": 1,
            "count": 1,
            "has_more": false,
        ])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)

        model.acknowledge(alert)
        XCTAssertTrue(model.isAlertCommandPending(recordID: 1))
        let unrelatedStatus = try JSONDecoder().decode(GatewayStatus.self, from: Data(
            #"{"kind":"status","f":true,"s":"acknowledged","i":99}"#.utf8
        ))
        service.onStatus?(unrelatedStatus)
        XCTAssertTrue(model.isAlertCommandPending(recordID: 1), "Another incident must not confirm this command")
        XCTAssertEqual(alert.status, .open)
        service.onStatus?(GatewayStatus(
            storageReady: true,
            journalCount: 2,
            latestNodeUID: "boat",
            lastPacketAgeMS: 0,
            radioHealth: "recent",
            rssi: -80,
            snr: 8,
            alertState: "acknowledged",
            activeRecordID: 1,
            severity: "sos",
            pairingOpen: false
        ))

        XCTAssertEqual(alert.status, .acknowledged)
        XCTAssertFalse(model.isAlertCommandPending(recordID: 1))

        model.resolve(alert)
        XCTAssertTrue(model.isAlertCommandPending(recordID: 1))
        service.onStatus?(GatewayStatus(
            storageReady: true,
            journalCount: 3,
            latestNodeUID: "boat",
            lastPacketAgeMS: 0,
            radioHealth: "recent",
            rssi: -80,
            snr: 8,
            alertState: "resolved",
            activeRecordID: 1,
            severity: "sos",
            pairingOpen: false
        ))

        XCTAssertEqual(alert.status, .resolved)
        XCTAssertFalse(model.isAlertCommandPending(recordID: 1))
    }

    @MainActor
    func testOutOfOrderResultsCompleteTheirOwnAlertCommands() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit(event(recordID: 2, syncID: syncID, status: "help"))
        service.emit([
            "kind": "sync_complete",
            "sync_id": syncID,
            "first_record_id": 1,
            "last_record_id": 2,
            "count": 2,
            "has_more": false,
        ])
        let alerts = try container.mainContext.fetch(FetchDescriptor<MarineAlert>())
        let first = try XCTUnwrap(alerts.first(where: { $0.recordID == 1 }))
        let second = try XCTUnwrap(alerts.first(where: { $0.recordID == 2 }))

        model.acknowledge(first)
        model.resolve(second)
        let acknowledge = try XCTUnwrap(service.alertCommands.first)
        let resolve = try XCTUnwrap(service.alertCommands.last)

        service.emit(commandResult(for: resolve, ok: true))
        service.emit(commandResult(for: acknowledge, ok: true))

        XCTAssertEqual(first.status, .acknowledged)
        XCTAssertEqual(second.status, .resolved)
    }

    @MainActor
    func testLateAcknowledgementResultDoesNotRegressResolvedAlert() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)

        model.acknowledge(alert)
        model.resolve(alert)
        let acknowledge = try XCTUnwrap(service.alertCommands.first)
        let resolve = try XCTUnwrap(service.alertCommands.last)

        service.emit(commandResult(for: resolve, ok: true))
        service.emit(commandResult(for: acknowledge, ok: true))

        XCTAssertEqual(alert.status, .resolved)

        service.onStatus?(GatewayStatus(
            storageReady: true,
            journalCount: 1,
            latestNodeUID: "boat",
            lastPacketAgeMS: 0,
            radioHealth: "recent",
            rssi: -80,
            snr: 8,
            alertState: "open",
            activeRecordID: 1,
            severity: "sos",
            pairingOpen: false
        ))
        XCTAssertEqual(alert.status, .resolved)
    }

    @MainActor
    func testDisconnectClearsPendingAlertCommands() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)

        model.resolve(alert)
        let command = try XCTUnwrap(service.alertCommands.last)
        service.disconnect()
        service.emit(commandResult(for: command, ok: true))

        XCTAssertEqual(alert.status, .open)
    }

    @MainActor
    func testForgetGatewayClearsOnlyItsLocalDataCursorAndPendingCommands() throws {
        let service = FakeGatewayService()
        let defaults = testDefaults()
        let model = AppModel(ble: service, defaults: defaults)
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])
        let alert = try XCTUnwrap(container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first)
        container.mainContext.insert(BoatSnapshot(
            eventKey: "other:epoch:boat:1",
            gatewayIdentity: "other:epoch",
            recordID: 1,
            nodeUID: "other-boat",
            nonce: "1",
            timeQuality: "uptime_only",
            latitude: nil,
            longitude: nil,
            batteryPercent: nil,
            rssi: nil,
            snr: nil,
            payload: ""
        ))
        container.mainContext.insert(MarineAlert(
            eventKey: "other:epoch:boat:1",
            gatewayIdentity: "other:epoch",
            recordID: 1,
            nodeUID: "other-boat",
            title: "help",
            detail: "Other gateway"
        ))
        try container.mainContext.save()

        model.resolve(alert)
        let command = try XCTUnwrap(service.alertCommands.last)
        let shoreReference = try XCTUnwrap(ShoreReference(latitude: 31, longitude: 121, capturedAt: .now, horizontalAccuracyMeters: 4))
        XCTAssertTrue(model.saveShoreReference(shoreReference))
        let otherReference = try XCTUnwrap(ShoreReference(latitude: 32, longitude: 122, capturedAt: .now, horizontalAccuracyMeters: 8))
        defaults.set(try JSONEncoder().encode(otherReference), forKey: "coastcue.shore-reference.other:epoch")
        model.forgetGateway(context: container.mainContext)
        service.emit(commandResult(for: command, ok: true))

        let snapshots = try container.mainContext.fetch(FetchDescriptor<BoatSnapshot>())
        let alerts = try container.mainContext.fetch(FetchDescriptor<MarineAlert>())
        XCTAssertEqual(snapshots.map(\.gatewayIdentity), ["other:epoch"])
        XCTAssertEqual(alerts.map(\.gatewayIdentity), ["other:epoch"])
        XCTAssertNil(model.gatewayIdentity)
        XCTAssertEqual(model.cursorForTesting, 0)
        XCTAssertNil(defaults.object(forKey: "coastcue.shore-reference.gw:epoch"))
        XCTAssertEqual(
            try JSONDecoder().decode(ShoreReference.self, from: try XCTUnwrap(defaults.data(forKey: "coastcue.shore-reference.other:epoch"))),
            otherReference
        )

        model.connectAndSync(context: container.mainContext)
        XCTAssertEqual(service.syncRequests.last?.after, 0)
    }

    @MainActor
    func testColdStartForgetUsesSavedIdentityToClearOnlyThatGateway() throws {
        let service = FakeGatewayService()
        service.savedGatewayIdentity = "gw:epoch"
        let defaults = testDefaults()
        defaults.set(42, forKey: "coastcue.cursor.gw:epoch")
        let model = AppModel(ble: service, defaults: defaults)
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        container.mainContext.insert(BoatSnapshot(
            eventKey: "gw:epoch:boat:1",
            gatewayIdentity: "gw:epoch",
            recordID: 1,
            nodeUID: "boat",
            nonce: "1",
            timeQuality: "uptime_only",
            latitude: nil,
            longitude: nil,
            batteryPercent: nil,
            rssi: nil,
            snr: nil,
            payload: ""
        ))
        container.mainContext.insert(MarineAlert(
            eventKey: "gw:epoch:boat:1",
            gatewayIdentity: "gw:epoch",
            recordID: 1,
            nodeUID: "boat",
            title: "sos",
            detail: "Saved gateway"
        ))
        container.mainContext.insert(BoatSnapshot(
            eventKey: "other:epoch:boat:1",
            gatewayIdentity: "other:epoch",
            recordID: 1,
            nodeUID: "other-boat",
            nonce: "1",
            timeQuality: "uptime_only",
            latitude: nil,
            longitude: nil,
            batteryPercent: nil,
            rssi: nil,
            snr: nil,
            payload: ""
        ))
        try container.mainContext.save()

        XCTAssertNil(model.gatewayIdentity)
        model.forgetGateway(context: container.mainContext)

        XCTAssertEqual(
            try container.mainContext.fetch(FetchDescriptor<BoatSnapshot>()).map(\.gatewayIdentity),
            ["other:epoch"]
        )
        XCTAssertTrue(try container.mainContext.fetch(FetchDescriptor<MarineAlert>()).isEmpty)
        XCTAssertNil(defaults.object(forKey: "coastcue.cursor.gw:epoch"))
        XCTAssertEqual(service.forgetRequests, 1)
    }

    @MainActor
    func testForgetKeepsTrustAndLocalDataWhenDeletionCannotBeSaved() throws {
        let service = FakeGatewayService()
        service.savedGatewayIdentity = "gw:epoch"
        let defaults = testDefaults()
        let shoreReference = try XCTUnwrap(ShoreReference(latitude: 31, longitude: 121, capturedAt: .now, horizontalAccuracyMeters: 4))
        defaults.set(try JSONEncoder().encode(shoreReference), forKey: "coastcue.shore-reference.gw:epoch")
        let model = AppModel(
            ble: service,
            defaults: defaults,
            saveAction: { _ in throw TestSaveError.failed }
        )
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        container.mainContext.insert(MarineAlert(
            eventKey: "gw:epoch:boat:1",
            gatewayIdentity: "gw:epoch",
            recordID: 1,
            nodeUID: "boat",
            title: "sos",
            detail: "Saved gateway"
        ))
        try container.mainContext.save()

        model.forgetGateway(context: container.mainContext)

        XCTAssertEqual(try container.mainContext.fetchCount(FetchDescriptor<MarineAlert>()), 1)
        XCTAssertEqual(service.savedGatewayIdentity, "gw:epoch")
        XCTAssertEqual(service.forgetRequests, 0)
        XCTAssertEqual(
            try JSONDecoder().decode(ShoreReference.self, from: try XCTUnwrap(defaults.data(forKey: "coastcue.shore-reference.gw:epoch"))),
            shoreReference
        )
        XCTAssertNotNil(model.lastError)
    }

    @MainActor
    func testEnrollmentCandidateWaitsForConfirmationBeforeSync() throws {
        let service = FakeGatewayService()
        service.autoDeliverDeviceInfoOnConnect = false
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )

        model.connectAndSync(context: container.mainContext)
        let candidate = service.emitEnrollmentCandidate(nodeUID: "shore-a", epoch: "epoch-a")
        service.emit(["kind": "event", "record_id": 1, "node_uid": "untrusted", "nonce": "1"])

        XCTAssertEqual(model.enrollmentCandidate, candidate)
        XCTAssertEqual(model.connectionState, .confirming)
        XCTAssertNil(model.gatewayIdentity)
        XCTAssertNil(model.lastError)
        XCTAssertTrue(try container.mainContext.fetch(FetchDescriptor<BoatSnapshot>()).isEmpty)
        XCTAssertTrue(service.syncRequests.isEmpty)
        XCTAssertTrue(service.confirmedCandidates.isEmpty)
    }

    @MainActor
    func testEnrollmentCandidateRetiresRecoveredConnectionTimeoutBeforeConfirmation() throws {
        let service = FakeGatewayService()
        service.autoDeliverDeviceInfoOnConnect = false
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )

        model.connectAndSync(context: container.mainContext)
        service.onError?(String(localized: "Gateway connection timed out. Tap Connect to retry."))
        let candidate = service.emitEnrollmentCandidate(nodeUID: "shore-a", epoch: "epoch-a")

        XCTAssertEqual(model.enrollmentCandidate, candidate)
        XCTAssertEqual(model.connectionState, .confirming)
        XCTAssertNil(model.lastError)

        model.confirmEnrollment(candidate)

        XCTAssertEqual(model.gatewayIdentity, "shore-a:epoch-a")
        XCTAssertEqual(service.syncRequests.count, 1)
        XCTAssertNil(model.lastError)
    }

    @MainActor
    func testCommandsStayDisabledUntilGatewayIdentityIsTrusted() throws {
        let service = FakeGatewayService()
        service.autoDeliverDeviceInfoOnConnect = false
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        let untrustedAlert = MarineAlert(
            eventKey: "untrusted",
            gatewayIdentity: "shore-a:epoch-a",
            recordID: 1,
            nodeUID: "boat",
            title: "SOS",
            detail: ""
        )

        model.connectAndSync(context: container.mainContext)
        XCTAssertEqual(model.connectionState, .connected)
        XCTAssertFalse(model.canSendAlertCommand)

        model.resolve(untrustedAlert)

        XCTAssertTrue(service.alertCommands.isEmpty)
    }

    @MainActor
    func testConfirmEnrollmentPinsExactCandidateAndBeginsSync() throws {
        let service = FakeGatewayService()
        service.autoDeliverDeviceInfoOnConnect = false
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )

        model.connectAndSync(context: container.mainContext)
        let candidate = service.emitEnrollmentCandidate(nodeUID: "shore-a", epoch: "epoch-a")
        model.confirmEnrollment(candidate)

        XCTAssertEqual(service.confirmedCandidates, [candidate])
        XCTAssertNil(model.enrollmentCandidate)
        XCTAssertEqual(model.gatewayIdentity, "shore-a:epoch-a")
        XCTAssertEqual(model.gatewayFirmware, "test-firmware")
        XCTAssertEqual(service.syncRequests.count, 1)
        XCTAssertEqual(service.statusReadRequests, 1)
    }

    @MainActor
    func testCancelEnrollmentLeavesNoPinAndDoesNotSync() throws {
        let service = FakeGatewayService()
        service.autoDeliverDeviceInfoOnConnect = false
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )

        model.connectAndSync(context: container.mainContext)
        let candidate = service.emitEnrollmentCandidate(nodeUID: "shore-a", epoch: "epoch-a")
        model.cancelEnrollment(candidate)

        XCTAssertEqual(service.cancelledCandidates, [candidate])
        XCTAssertTrue(service.confirmedCandidates.isEmpty)
        XCTAssertTrue(service.syncRequests.isEmpty)
        XCTAssertNil(model.enrollmentCandidate)
        XCTAssertNil(model.gatewayIdentity)
    }

    @MainActor
    func testStaleEnrollmentCandidateCannotBeConfirmed() throws {
        let service = FakeGatewayService()
        service.autoDeliverDeviceInfoOnConnect = false
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )

        model.connectAndSync(context: container.mainContext)
        let stale = service.emitEnrollmentCandidate(nodeUID: "shore-a", epoch: "epoch-a")
        let current = service.emitEnrollmentCandidate(nodeUID: "shore-b", epoch: "epoch-b")
        model.confirmEnrollment(stale)

        XCTAssertEqual(model.enrollmentCandidate, current)
        XCTAssertTrue(service.confirmedCandidates.isEmpty)
        XCTAssertTrue(service.syncRequests.isEmpty)

        model.confirmEnrollment(current)
        XCTAssertEqual(service.confirmedCandidates, [current])
        XCTAssertEqual(service.syncRequests.count, 1)
    }

    @MainActor
    func testGatewayTrustPinsOnlyAfterExplicitConfirmationAndRequiresExplicitForget() {
        let defaults = testDefaults()
        let firstPeripheralID = UUID()
        let otherPeripheralID = UUID()
        let expectedInfo = GatewayDeviceInfo(
            name: "CoastCue Shore",
            nodeUID: "shore-a",
            journalEpoch: "epoch-a",
            firmware: "test-firmware"
        )
        let changedInfo = GatewayDeviceInfo(
            name: "CoastCue Shore",
            nodeUID: "shore-a",
            journalEpoch: "different-epoch",
            firmware: "test-firmware"
        )
        let initial = GatewayTrustStore(defaults: defaults)

        XCTAssertTrue(initial.canConnect(to: firstPeripheralID))
        XCTAssertTrue(initial.canConnect(to: otherPeripheralID))
        XCTAssertFalse(initial.isTrusted(peripheralID: firstPeripheralID, info: expectedInfo))
        XCTAssertTrue(initial.pinIfUnpinned(peripheralID: firstPeripheralID, info: expectedInfo))
        XCTAssertEqual(initial.pinnedGatewayIdentity, expectedInfo.identity)

        let restored = GatewayTrustStore(defaults: defaults)
        XCTAssertEqual(restored.pinnedGatewayIdentity, expectedInfo.identity)
        XCTAssertTrue(restored.canConnect(to: firstPeripheralID))
        XCTAssertFalse(restored.canConnect(to: otherPeripheralID))
        XCTAssertTrue(restored.isTrusted(peripheralID: firstPeripheralID, info: expectedInfo))
        XCTAssertFalse(restored.isTrusted(peripheralID: firstPeripheralID, info: changedInfo))
        XCTAssertFalse(restored.pinIfUnpinned(peripheralID: firstPeripheralID, info: changedInfo))

        restored.forget()
        XCTAssertNil(restored.pinnedGatewayIdentity)
        XCTAssertTrue(restored.canConnect(to: otherPeripheralID))
        XCTAssertFalse(restored.isTrusted(peripheralID: firstPeripheralID, info: expectedInfo))
    }

    @MainActor
    func testGatewayTrustMigratesLegacyPinToCurrentStorageKey() throws {
        let defaults = testDefaults()
        let peripheralID = UUID()
        let legacyPin = try JSONSerialization.data(withJSONObject: [
            "peripheralID": peripheralID.uuidString,
            "gatewayIdentity": "shore-a:epoch-a",
        ])
        defaults.set(legacyPin, forKey: "coastcue.gateway-pin.v1")

        let restored = GatewayTrustStore(defaults: defaults)

        XCTAssertEqual(restored.pinnedPeripheralID, peripheralID)
        XCTAssertEqual(restored.pinnedGatewayIdentity, "shore-a:epoch-a")
        XCTAssertNotNil(defaults.data(forKey: "coastcue.gateway-pin"))
        XCTAssertNil(defaults.object(forKey: "coastcue.gateway-pin.v1"))
    }

    @MainActor
    func testStatusExpiresRecoversAndAvoidsUnchangedDatabaseWrites() throws {
        let service = FakeGatewayService()
        var saves = 0
        let model = AppModel(ble: service, defaults: testDefaults(), saveAction: {
            saves += 1
            try $0.save()
        })
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos"))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])
        let status = try JSONDecoder().decode(GatewayStatus.self, from: Data(
            #"{"kind":"status","f":true,"s":"acknowledged","i":1}"#.utf8
        ))
        service.onStatus?(status)
        XCTAssertEqual(saves, 2)
        service.onStatus?(status)
        XCTAssertEqual(saves, 2, "Unchanged heartbeats must not save again")
        XCTAssertTrue(model.hasCurrentGatewaySnapshot)
        model.checkGatewayStatusTimeout(now: .now.addingTimeInterval(11))
        XCTAssertTrue(model.isGatewayStatusStale)
        XCTAssertNil(model.currentGatewayStatus)
        XCTAssertFalse(model.hasCurrentGatewaySnapshot)
        service.onStatus?(status)
        XCTAssertTrue(model.hasCurrentGatewaySnapshot)
        service.emitDeviceInfo(nodeUID: "replacement", epoch: "fresh")
        XCTAssertNil(model.currentGatewayStatus)
    }

    func testJournalGapValidationIsBoundedAndRejectsUnexplainedGaps() {
        XCTAssertTrue(journalPageIDsAreValid([2_000_000_000], after: 0, first: 2_000_000_000, last: 2_000_000_000, truncatedBelow: nil, missing: [[1, 1_999_999_999]]))
        XCTAssertTrue(journalPageIDsAreValid([4, 8], after: 0, first: 4, last: 8, truncatedBelow: 4, missing: [[7, 7], [5, 6]]))
        XCTAssertFalse(journalPageIDsAreValid([4, 8], after: 0, first: 4, last: 8, truncatedBelow: 4, missing: [[5, 6]]))
        XCTAssertFalse(journalPageIDsAreValid([1, 1], after: 0, first: 1, last: 1, truncatedBelow: nil, missing: []))
        XCTAssertFalse(journalPageIDsAreValid([1], after: 0, first: 1, last: 1, truncatedBelow: nil, missing: [[2, 1]]))
        XCTAssertFalse(journalPageIDsAreValid([Int64.max], after: 0, first: Int64.max, last: Int64.max, truncatedBelow: nil, missing: []))
    }

    func testFramerRejectsResourceAbuseAndRecoversAfterConflictingFragment() throws {
        var framer = GatewayMessageFramer()
        XCTAssertThrowsError(try framer.ingest(Data([0x53, 0x4D, 1])))
        XCTAssertThrowsError(try framer.ingest(fragment(id: 7, index: 0, count: .max, crc: 0, payload: Data([1]))))
        let payload = Data("{}".utf8)
        let crc = CRC32.checksum(payload)
        let first = fragment(id: 7, index: 0, count: 2, crc: crc, payload: Data("{".utf8))
        XCTAssertNil(try framer.ingest(first))
        XCTAssertNil(try framer.ingest(first), "Identical retransmission is harmless")
        XCTAssertThrowsError(try framer.ingest(fragment(id: 7, index: 0, count: 2, crc: crc, payload: Data("!".utf8))))
        XCTAssertNil(try framer.ingest(first))
        XCTAssertEqual(try framer.ingest(fragment(id: 7, index: 1, count: 2, crc: crc, payload: Data("}".utf8))), payload)
        let oversized = Data(("{\"data\":\"" + String(repeating: "x", count: 8192) + "\"}").utf8)
        XCTAssertThrowsError(try GatewayMessageFramer.frames(for: oversized, messageID: 9).forEach { _ = try framer.ingest($0) })
    }

    func testSmallJSONMessagePassesThroughFramer() throws {
        var framer = GatewayMessageFramer()
        let data = Data("{\"kind\":\"event\",\"record_id\":1}".utf8)
        XCTAssertEqual(try framer.ingest(data), data)
    }

    func testUnframedJSONOver160BytesIsRejected() throws {
        var framer = GatewayMessageFramer()
        let data = Data("{\"kind\":\"\(String(repeating: "x", count: 150))\"}".utf8)

        XCTAssertEqual(data.count, 161)
        XCTAssertThrowsError(try framer.ingest(data))
    }

    func testUnframedJSONAt160BytesPassesThroughFramer() throws {
        var framer = GatewayMessageFramer()
        let data = Data("{\"kind\":\"\(String(repeating: "x", count: 149))\"}".utf8)

        XCTAssertEqual(data.count, 160)
        XCTAssertEqual(try framer.ingest(data), data)
    }

    func testFragmentReassemblesAndChecksCRC() throws {
        let payload = Data("{\"kind\":\"sync_complete\",\"count\":1}".utf8)
        let crc = CRC32.checksum(payload)
        let chunks = [Data(payload.prefix(12)), Data(payload.dropFirst(12))]
        var framer = GatewayMessageFramer()
        XCTAssertNil(try framer.ingest(fragment(id: 7, index: 0, count: 2, crc: crc, payload: chunks[0])))
        XCTAssertEqual(try framer.ingest(fragment(id: 7, index: 1, count: 2, crc: crc, payload: chunks[1])), payload)
    }

    func testFramedJSONOver160BytesReassembles() throws {
        let payload = Data("{\"kind\":\"sync_complete\",\"note\":\"\(String(repeating: "x", count: 180))\"}".utf8)
        var framer = GatewayMessageFramer()

        XCTAssertGreaterThan(payload.count, 160)
        let frames = GatewayMessageFramer.frames(for: payload, messageID: 8)
        XCTAssertEqual(frames.count, 2)
        XCTAssertNil(try framer.ingest(frames[0]))
        XCTAssertEqual(try framer.ingest(frames[1]), payload)
    }

    func testBadFragmentCRCIsRejected() throws {
        var framer = GatewayMessageFramer()
        XCTAssertThrowsError(try framer.ingest(fragment(id: 7, index: 0, count: 1, crc: 42, payload: Data("{}".utf8))))
    }

    func testGatewayEventDecodesNormalizedLocationAndEmergencyStatus() throws {
        let data = Data(#"{"kind":"event","record_id":17,"node_uid":"boat-A","boat_id":"B-17","nonce":"99","status":"sos","latitude":31.1,"longitude":121.2,"gps_age_s":11,"incident_id":"0123abcd4567ef89"}"#.utf8)
        let event = try JSONDecoder().decode(GatewayEvent.self, from: data)
        XCTAssertEqual(event.status, "sos")
        XCTAssertEqual(event.boatID, "B-17")
        XCTAssertEqual(event.latitude, 31.1)
        XCTAssertEqual(event.longitude, 121.2)
        XCTAssertEqual(event.gpsAgeSeconds, 11)
        XCTAssertEqual(event.incidentID, "0123abcd4567ef89")
    }

    func testGatewayEventAcceptsAdditiveUnknownFields() throws {
        let data = Data(#"{"kind":"event","record_id":17,"node_uid":"boat-A","nonce":"99","future_optional_field":{"value":1}}"#.utf8)

        let event = try JSONDecoder().decode(GatewayEvent.self, from: data)

        XCTAssertEqual(event.recordID, 17)
        XCTAssertEqual(event.nodeUID, "boat-A")
    }

    func testIncidentLocationUsesTheExactIncidentSnapshotInsteadOfLaterBoatPosition() {
        let incident = BoatSnapshot(eventKey: "incident", gatewayIdentity: "shore", recordID: 1, nodeUID: "boat", nonce: "1", receivedAt: .now, timeQuality: "gps", latitude: 31, longitude: 121, batteryPercent: nil, rssi: nil, snr: nil, payload: "")
        incident.gpsAgeSeconds = 14
        let laterReport = BoatSnapshot(eventKey: "later", gatewayIdentity: "shore", recordID: 2, nodeUID: "boat", nonce: "2", receivedAt: .now, timeQuality: "gps", latitude: 32, longitude: 122, batteryPercent: nil, rssi: nil, snr: nil, payload: "")

        let result = incidentLocationSnapshot(gatewayIdentity: "shore", eventKey: "incident", snapshots: [laterReport, incident])

        XCTAssertEqual(result?.recordID, 1)
        XCTAssertEqual(result?.latitude, 31)
        XCTAssertEqual(result?.longitude, 121)
        XCTAssertEqual(result?.gpsAgeSeconds, 14)
    }

    func testPostAlertLocationUsesOnlyLaterReportsFromTheSameGatewayAndBoat() {
        let alertTime = Date(timeIntervalSince1970: 100)
        let alert = MarineAlert(
            eventKey: "shore:boat:alert",
            gatewayIdentity: "shore",
            recordID: 10,
            nodeUID: "boat",
            title: "SOS",
            detail: "",
            occurredAt: alertTime,
            syncedAt: alertTime
        )
        let beforeAlert = BoatSnapshot(eventKey: "before", gatewayIdentity: "shore", recordID: 9, nodeUID: "boat", nonce: "before", occurredAt: Date(timeIntervalSince1970: 90), syncedAt: Date(timeIntervalSince1970: 90), timeQuality: "gps", latitude: 30, longitude: 120, batteryPercent: nil, rssi: nil, snr: nil, payload: "")
        let laterSameBoat = BoatSnapshot(eventKey: "later", gatewayIdentity: "shore", recordID: 11, nodeUID: "boat", nonce: "later", occurredAt: Date(timeIntervalSince1970: 110), syncedAt: Date(timeIntervalSince1970: 110), timeQuality: "gps", latitude: 31, longitude: 121, batteryPercent: nil, rssi: nil, snr: nil, payload: "")
        let otherBoat = BoatSnapshot(eventKey: "other-boat", gatewayIdentity: "shore", recordID: 12, nodeUID: "other-boat", nonce: "other", occurredAt: Date(timeIntervalSince1970: 120), syncedAt: Date(timeIntervalSince1970: 120), timeQuality: "gps", latitude: 32, longitude: 122, batteryPercent: nil, rssi: nil, snr: nil, payload: "")
        let otherGateway = BoatSnapshot(eventKey: "other-gateway", gatewayIdentity: "other-shore", recordID: 13, nodeUID: "boat", nonce: "other-gateway", occurredAt: Date(timeIntervalSince1970: 130), syncedAt: Date(timeIntervalSince1970: 130), timeQuality: "gps", latitude: 33, longitude: 123, batteryPercent: nil, rssi: nil, snr: nil, payload: "")

        let result = latestValidLocationAfterAlert(
            alert,
            snapshots: [beforeAlert, otherBoat, otherGateway, laterSameBoat]
        )

        XCTAssertEqual(result?.eventKey, "later")
        XCTAssertEqual(result?.latitude, 31)
        XCTAssertEqual(result?.longitude, 121)
    }

    func testPostAlertLocationDoesNotUseAnEarlierBoatPosition() {
        let alert = MarineAlert(eventKey: "shore:boat:alert", gatewayIdentity: "shore", recordID: 10, nodeUID: "boat", title: "Help", detail: "", occurredAt: Date(timeIntervalSince1970: 100), syncedAt: Date(timeIntervalSince1970: 100))
        let earlier = BoatSnapshot(eventKey: "earlier", gatewayIdentity: "shore", recordID: 9, nodeUID: "boat", nonce: "earlier", occurredAt: Date(timeIntervalSince1970: 90), syncedAt: Date(timeIntervalSince1970: 90), timeQuality: "gps", latitude: 30, longitude: 120, batteryPercent: nil, rssi: nil, snr: nil, payload: "")

        XCTAssertNil(latestValidLocationAfterAlert(alert, snapshots: [earlier]))
    }

    func testIncidentRouteSurvivesLiveRecordPromotion() {
        let alert = MarineAlert(eventKey: "shore:boat:checkpoint", gatewayIdentity: "shore", recordID: 2_147_483_650, nodeUID: "boat", title: "sos", detail: "")
        let route = AppRoute.incident(alert.gatewayIdentity, alert.eventKey)

        alert.recordID = 2

        XCTAssertEqual(route, .incident("shore", "shore:boat:checkpoint"))
        XCTAssertEqual(
            incidentAlert(gatewayIdentity: "shore", eventKey: "shore:boat:checkpoint", alerts: [alert])?.recordID,
            2
        )
    }

    func testInvalidCoordinatesAreNotEligibleForMaps() {
        XCTAssertFalse(isValidCoordinate(latitude: .infinity, longitude: 121))
        XCTAssertFalse(isValidCoordinate(latitude: 91, longitude: 121))
        XCTAssertFalse(isValidCoordinate(latitude: 31, longitude: -181))
        XCTAssertTrue(isValidCoordinate(latitude: 31, longitude: 121))
    }

    func testShoreReferenceRejectsInvalidCoordinatesAndAccuracy() {
        XCTAssertNil(ShoreReference(latitude: 91, longitude: 121, capturedAt: .now, horizontalAccuracyMeters: 5))
        XCTAssertNil(ShoreReference(latitude: 31, longitude: 121, capturedAt: .now, horizontalAccuracyMeters: -.infinity))
        XCTAssertNotNil(ShoreReference(latitude: 31, longitude: 121, capturedAt: .now, horizontalAccuracyMeters: 100))
        XCTAssertNil(ShoreReference(latitude: 31, longitude: 121, capturedAt: .now, horizontalAccuracyMeters: 101))
        XCTAssertNotNil(ShoreReference(latitude: 31, longitude: 121, capturedAt: .now, horizontalAccuracyMeters: 5))
    }

    @MainActor
    func testShoreReferencePersistsAcrossReconnectsAndGatewaySwitches() throws {
        let defaults = testDefaults()
        let firstService = FakeGatewayService()
        let firstModel = AppModel(ble: firstService, defaults: defaults)
        let container = try ModelContainer(
            for: BoatSnapshot.self,
            MarineAlert.self,
            configurations: ModelConfiguration(isStoredInMemoryOnly: true)
        )
        firstModel.connectAndSync(context: container.mainContext)
        let firstReference = try XCTUnwrap(ShoreReference(latitude: 31, longitude: 121, capturedAt: Date(timeIntervalSince1970: 1), horizontalAccuracyMeters: 3))
        XCTAssertTrue(firstModel.saveShoreReference(firstReference))
        XCTAssertEqual(firstModel.currentShoreReference, firstReference)

        firstService.emitDeviceInfo(nodeUID: "shore-b", epoch: "fresh")
        XCTAssertNil(firstModel.currentShoreReference)
        let secondReference = try XCTUnwrap(ShoreReference(latitude: 32, longitude: 122, capturedAt: Date(timeIntervalSince1970: 2), horizontalAccuracyMeters: 5))
        XCTAssertTrue(firstModel.saveShoreReference(secondReference))
        XCTAssertEqual(firstModel.shoreReference(for: "gw:epoch"), firstReference)
        firstService.emitDeviceInfo(nodeUID: "gw", epoch: "epoch")
        XCTAssertEqual(firstModel.currentShoreReference, firstReference)

        let coldService = FakeGatewayService()
        let coldModel = AppModel(ble: coldService, defaults: defaults)
        coldModel.connectAndSync(context: container.mainContext)

        XCTAssertEqual(coldModel.currentShoreReference, firstReference)
        XCTAssertEqual(coldModel.shoreReference(for: "shore-b:fresh"), secondReference)
    }

    func testGreatCircleDistanceMetersHandlesKnownAndInvalidCoordinates() {
        let oneDegreeAtEquator = try! XCTUnwrap(greatCircleDistanceMeters(fromLatitude: 0, fromLongitude: 0, toLatitude: 0, toLongitude: 1))
        XCTAssertEqual(oneDegreeAtEquator, 111_195, accuracy: 1)
        XCTAssertEqual(greatCircleDistanceMeters(fromLatitude: 31, fromLongitude: 121, toLatitude: 31, toLongitude: 121), 0)
        XCTAssertNil(greatCircleDistanceMeters(fromLatitude: .nan, fromLongitude: 121, toLatitude: 31, toLongitude: 121))
    }

    func testFormattedApproximateDistanceUsesMetersBelowOneKilometer() {
        XCTAssertEqual(formattedApproximateDistance(999), "999 m")
        XCTAssertEqual(formattedApproximateDistance(1_000), "1.0 km")
        XCTAssertEqual(formattedApproximateDistance(1_550), "1.6 km")
        XCTAssertEqual(formattedApproximateDistance(.infinity), "—")
    }

    func testActiveAlertsAreScopedToTheCurrentGateway() {
        let current = MarineAlert(eventKey: "current", gatewayIdentity: "current", recordID: 1, nodeUID: "boat", title: "SOS", detail: "", status: .open)
        let oldGateway = MarineAlert(eventKey: "old", gatewayIdentity: "old", recordID: 2, nodeUID: "boat", title: "Help", detail: "", status: .acknowledged)
        let historical = MarineAlert(eventKey: "history", gatewayIdentity: "old", recordID: 3, nodeUID: "boat", title: "SOS", detail: "", status: .resolved)

        XCTAssertEqual(activeAlertsForCurrentGateway(gatewayIdentity: "current", alerts: [current, oldGateway, historical]).map(\.recordID), [1])
        XCTAssertTrue(activeAlertsForCurrentGateway(gatewayIdentity: nil, alerts: [current, oldGateway]).isEmpty)
        XCTAssertTrue(pastAlertsForCurrentGateway(gatewayIdentity: "current", alerts: [current, oldGateway, historical]).isEmpty)
        XCTAssertEqual(pastAlertsForCurrentGateway(gatewayIdentity: "old", alerts: [current, oldGateway, historical]).map(\.recordID), [3])
    }

    func testBoatReportsAreScopedToTheCurrentGateway() {
        let current = BoatSnapshot(eventKey: "current", gatewayIdentity: "current", recordID: 1, nodeUID: "boat", nonce: "1", receivedAt: .now, timeQuality: "gps", latitude: nil, longitude: nil, batteryPercent: nil, rssi: nil, snr: nil, payload: "")
        let oldGateway = BoatSnapshot(eventKey: "old", gatewayIdentity: "old", recordID: 2, nodeUID: "boat", nonce: "2", receivedAt: .now, timeQuality: "gps", latitude: nil, longitude: nil, batteryPercent: nil, rssi: nil, snr: nil, payload: "")

        XCTAssertEqual(boatSnapshotsForCurrentGateway(gatewayIdentity: "current", snapshots: [oldGateway, current]).map(\.recordID), [1])
        XCTAssertTrue(boatSnapshotsForCurrentGateway(gatewayIdentity: nil, snapshots: [current]).isEmpty)
    }

    @MainActor
    func testIncidentIDPersistsOnSnapshotAndAlert() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)
        service.emit(event(recordID: 1, syncID: syncID, status: "sos", extras: ["incident_id": "0123abcd4567ef89"]))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])

        XCTAssertEqual(try container.mainContext.fetch(FetchDescriptor<BoatSnapshot>()).first?.incidentID, "0123abcd4567ef89")
        XCTAssertEqual(try container.mainContext.fetch(FetchDescriptor<MarineAlert>()).first?.incidentID, "0123abcd4567ef89")
    }

    func testLatestValidLocationUsesJournalOrderButNotRandomVolatileIDs() {
        let laterTimeLowerDurableID = BoatSnapshot(eventKey: "d1", gatewayIdentity: "shore", recordID: 1, nodeUID: "boat", nonce: "d1", occurredAt: Date(timeIntervalSince1970: 300), syncedAt: Date(timeIntervalSince1970: 300), timeQuality: "phone_set", latitude: 31, longitude: 121, batteryPercent: nil, rssi: nil, snr: nil, payload: "")
        let higherDurableID = BoatSnapshot(eventKey: "d2", gatewayIdentity: "shore", recordID: 2, nodeUID: "boat", nonce: "d2", occurredAt: Date(timeIntervalSince1970: 100), syncedAt: Date(timeIntervalSince1970: 100), timeQuality: "phone_set", latitude: 32, longitude: 122, batteryPercent: nil, rssi: nil, snr: nil, payload: "")
        let laterTimeLowerVolatileID = BoatSnapshot(eventKey: "v1", gatewayIdentity: "shore", recordID: 2_147_483_649, nodeUID: "boat", nonce: "v1", occurredAt: Date(timeIntervalSince1970: 300), syncedAt: Date(timeIntervalSince1970: 300), timeQuality: "phone_set", latitude: 33, longitude: 123, batteryPercent: nil, rssi: nil, snr: nil, payload: "", durable: false)
        let higherVolatileID = BoatSnapshot(eventKey: "v2", gatewayIdentity: "shore", recordID: 2_147_483_650, nodeUID: "boat", nonce: "v2", occurredAt: Date(timeIntervalSince1970: 100), syncedAt: Date(timeIntervalSince1970: 100), timeQuality: "phone_set", latitude: 34, longitude: 124, batteryPercent: nil, rssi: nil, snr: nil, payload: "", durable: false)

        XCTAssertEqual(latestValidLocationSnapshot(gatewayIdentity: "shore", snapshots: [laterTimeLowerDurableID, higherDurableID])?.recordID, 2)
        XCTAssertEqual(latestValidLocationSnapshot(gatewayIdentity: "shore", snapshots: [laterTimeLowerVolatileID, higherVolatileID])?.recordID, 2_147_483_649)
    }

    func testPostAlertVolatileLocationDoesNotUseRandomRecordIDAcrossReboot() {
        let alert = MarineAlert(
            eventKey: "alert",
            gatewayIdentity: "shore",
            recordID: 2_147_483_700,
            nodeUID: "boat",
            title: "SOS",
            detail: "",
            occurredAt: nil,
            syncedAt: Date(timeIntervalSince1970: 200)
        )
        let olderButNumericallyHigher = BoatSnapshot(
            eventKey: "old-boot",
            gatewayIdentity: "shore",
            recordID: 2_147_483_900,
            nodeUID: "boat",
            nonce: "old",
            occurredAt: nil,
            syncedAt: Date(timeIntervalSince1970: 100),
            timeQuality: "uptime_only",
            latitude: 31,
            longitude: 121,
            batteryPercent: nil,
            rssi: nil,
            snr: nil,
            payload: "",
            durable: false
        )
        let newerButNumericallyLower = BoatSnapshot(
            eventKey: "new-boot",
            gatewayIdentity: "shore",
            recordID: 2_147_483_650,
            nodeUID: "boat",
            nonce: "new",
            occurredAt: nil,
            syncedAt: Date(timeIntervalSince1970: 300),
            timeQuality: "uptime_only",
            latitude: 32,
            longitude: 122,
            batteryPercent: nil,
            rssi: nil,
            snr: nil,
            payload: "",
            durable: false
        )

        XCTAssertEqual(
            latestValidLocationAfterAlert(alert, snapshots: [olderButNumericallyHigher, newerButNumericallyLower])?.eventKey,
            "new-boot"
        )
    }

    func testLatestValidLocationUsesNewerVolatileReportDuringStorageOutage() {
        let olderDurable = BoatSnapshot(eventKey: "durable", gatewayIdentity: "shore", recordID: 12, nodeUID: "boat", nonce: "d", occurredAt: Date(timeIntervalSince1970: 100), syncedAt: Date(timeIntervalSince1970: 400), timeQuality: "phone_set", latitude: 31, longitude: 121, batteryPercent: nil, rssi: nil, snr: nil, payload: "")
        let newerVolatile = BoatSnapshot(eventKey: "volatile", gatewayIdentity: "shore", recordID: 2_147_483_650, nodeUID: "boat", nonce: "v", occurredAt: Date(timeIntervalSince1970: 200), syncedAt: Date(timeIntervalSince1970: 200), timeQuality: "phone_set", latitude: 32, longitude: 122, batteryPercent: nil, rssi: nil, snr: nil, payload: "", durable: false)

        XCTAssertEqual(latestValidLocationSnapshot(gatewayIdentity: "shore", snapshots: [olderDurable, newerVolatile])?.eventKey, "volatile")
    }

    func testLatestValidLocationUsesLaterDurableReportAfterRecovery() {
        let outageReport = BoatSnapshot(eventKey: "volatile", gatewayIdentity: "shore", recordID: 2_147_483_650, nodeUID: "boat", nonce: "v", occurredAt: nil, syncedAt: Date(timeIntervalSince1970: 200), timeQuality: "uptime_only", latitude: 31, longitude: 121, batteryPercent: nil, rssi: nil, snr: nil, payload: "", durable: false)
        let recoveredReport = BoatSnapshot(eventKey: "durable", gatewayIdentity: "shore", recordID: 13, nodeUID: "boat", nonce: "d", occurredAt: nil, syncedAt: Date(timeIntervalSince1970: 300), timeQuality: "uptime_only", latitude: 32, longitude: 122, batteryPercent: nil, rssi: nil, snr: nil, payload: "")

        XCTAssertEqual(latestValidLocationSnapshot(gatewayIdentity: "shore", snapshots: [outageReport, recoveredReport])?.eventKey, "durable")
    }

    @MainActor
    func testLiveVolatileLocationDuringInitialSyncBeatsAmbiguousDurableHistory() throws {
        let service = FakeGatewayService()
        let model = AppModel(ble: service, defaults: testDefaults())
        let container = try ModelContainer(for: BoatSnapshot.self, MarineAlert.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
        model.connectAndSync(context: container.mainContext)
        let syncID = try XCTUnwrap(service.syncRequests.last?.syncID)

        service.emit(["kind": "event", "record_id": 2_147_483_650, "node_uid": "boat", "nonce": "live", "latitude": 32.0, "longitude": 122.0, "time_quality": "uptime_only", "durable": false])
        service.emit(event(recordID: 1, syncID: syncID, extras: ["latitude": 31.0, "longitude": 121.0, "time_quality": "uptime_only"]))
        service.emit(["kind": "sync_complete", "sync_id": syncID, "first_record_id": 1, "last_record_id": 1, "count": 1, "has_more": false])

        let snapshots = try container.mainContext.fetch(FetchDescriptor<BoatSnapshot>())
        XCTAssertEqual(latestValidLocationSnapshot(gatewayIdentity: "gw:epoch", snapshots: snapshots)?.eventKey, "gw:epoch:boat:live")
    }

    func testSyncCompleteUsesNumericCursorAndHasMore() throws {
        let data = Data("{\"kind\":\"sync_complete\",\"first_record_id\":1,\"last_record_id\":20,\"count\":20,\"has_more\":true}".utf8)
        let result = try JSONDecoder().decode(SyncComplete.self, from: data)
        XCTAssertEqual(result.lastRecordID, 20)
        XCTAssertTrue(result.hasMore)
    }

    func testOfflineLifecycleTransitionTargetsOriginalAlert() throws {
        let data = Data("{\"kind\":\"transition\",\"record_id\":22,\"target_record_id\":17,\"state\":\"resolved\",\"source\":\"button\"}".utf8)
        let transition = try JSONDecoder().decode(GatewayTransition.self, from: data)
        XCTAssertEqual(transition.recordID, 22)
        XCTAssertEqual(transition.targetRecordID, 17)
        XCTAssertEqual(transition.state, .resolved)
    }

    func testAcknowledgingDoesNotResolveAndResolvePreservesRecord() {
        let alert = MarineAlert(eventKey: "a:1", recordID: 1, nodeUID: "a", title: "SOS", detail: "", createdAt: .now)
        alert.status = .acknowledged
        XCTAssertEqual(alert.status, .acknowledged)
        alert.status = .resolved
        XCTAssertEqual(alert.status, .resolved)
        XCTAssertEqual(alert.recordID, 1)
    }

    func testAlertDisplayTitleUsesCanonicalAndPreviouslyStoredValues() {
        let sos = MarineAlert(eventKey: "a:2", recordID: 2, nodeUID: "a", title: "sos", detail: "", createdAt: .now)
        let help = MarineAlert(eventKey: "a:3", recordID: 3, nodeUID: "a", title: "help", detail: "", createdAt: .now)
        let previouslyStoredChineseHelp = MarineAlert(eventKey: "a:4", recordID: 4, nodeUID: "a", title: "求助", detail: "", createdAt: .now)

        XCTAssertEqual(sos.displayTitle, "SOS")
        XCTAssertEqual(help.displayTitle, String(localized: "Help"))
        XCTAssertEqual(previouslyStoredChineseHelp.displayTitle, String(localized: "Help"))
    }

    func testCompactGatewayStatusFitsNotificationAndDecodes() throws {
        let json = #"{"f":true,"c":18,"n":"esp32-a1b2c3d4e5f6","a":2_000,"h":"recent","r":-112.5,"q":-7.3,"s":"open","i":17,"e":"sos","p":false}"#
            .replacingOccurrences(of: "_", with: "")
        let data = Data(json.utf8)

        XCTAssertLessThanOrEqual(data.count, 182)
        let status = try JSONDecoder().decode(GatewayStatus.self, from: data)
        XCTAssertEqual(status.storageReady, true)
        XCTAssertEqual(status.journalCount, 18)
        XCTAssertEqual(status.latestNodeUID, "esp32-a1b2c3d4e5f6")
        XCTAssertEqual(status.lastPacketAgeMS, 2_000)
        XCTAssertEqual(status.radioHealth, "recent")
        XCTAssertEqual(status.rssi, -112.5)
        XCTAssertEqual(status.snr, -7.3)
        XCTAssertEqual(status.alertState, "open")
        XCTAssertEqual(status.activeRecordID, 17)
        XCTAssertEqual(status.severity, "sos")
        XCTAssertEqual(status.pairingOpen, false)
    }

    func testGatewayStatusIgnoresHarmlessExtraFields() throws {
        let data = Data(#"{"v":"removed","f":true,"s":"resolved","p":false}"#.utf8)
        let status = try JSONDecoder().decode(GatewayStatus.self, from: data)
        XCTAssertEqual(status.storageReady, true)
        XCTAssertEqual(status.alertState, "resolved")
        XCTAssertEqual(status.pairingOpen, false)
    }

    func testCurrentDeviceInfoDecodesWithoutProtocolField() throws {
        let data = Data(#"{"name":"CoastCue Shore","node_uid":"shore-a","journal_epoch":"epoch-a","firmware":"current"}"#.utf8)
        let info = try JSONDecoder().decode(GatewayDeviceInfo.self, from: data)

        XCTAssertEqual(info.identity, "shore-a:epoch-a")
        XCTAssertEqual(info.firmware, "current")
    }

    func testDeviceInfoIgnoresHarmlessExtraFields() throws {
        let data = Data(#"{"name":"CoastCue Shore","node_uid":"shore-a","journal_epoch":"epoch-a","firmware":"current","protocol":1}"#.utf8)
        let info = try JSONDecoder().decode(GatewayDeviceInfo.self, from: data)
        XCTAssertEqual(info.identity, "shore-a:epoch-a")
        XCTAssertEqual(info.firmware, "current")
    }

    func testRescuePresentationNeverShowsAllClearBeforeCurrentSync() throws {
        let openData = Data(#"{"s":"open","i":17,"e":"sos","p":false}"#.utf8)
        let resolvedData = Data(#"{"s":"resolved","i":17,"e":"none","p":false}"#.utf8)
        let open = try JSONDecoder().decode(GatewayStatus.self, from: openData)
        let resolved = try JSONDecoder().decode(GatewayStatus.self, from: resolvedData)

        XCTAssertEqual(
            rescuePresentationState(status: open, hasLocalAlert: false, isCurrent: true),
            .activeDetailsPending
        )
        XCTAssertEqual(
            rescuePresentationState(status: resolved, hasLocalAlert: false, isCurrent: false),
            .awaitingCurrentStatus
        )
        XCTAssertEqual(
            rescuePresentationState(status: resolved, hasLocalAlert: true, isCurrent: false),
            .activeAlert
        )
        XCTAssertEqual(
            rescuePresentationState(status: resolved, hasLocalAlert: true, isCurrent: true),
            .activeAlert
        )
        XCTAssertEqual(
            rescuePresentationState(status: resolved, hasLocalAlert: true, localAlertRecordID: 17, isCurrent: true),
            .noActiveRescue
        )
        XCTAssertEqual(
            rescuePresentationState(status: resolved, hasLocalAlert: false, isCurrent: true),
            .noActiveRescue
        )
    }

    func testResolvedStatusWithoutRecordIDCannotClearLocalOpenAlert() throws {
        let resolvedWithoutID = try JSONDecoder().decode(
            GatewayStatus.self,
            from: Data(#"{"s":"resolved","e":"none","p":false}"#.utf8)
        )
        let local = MarineAlert(
            eventKey: "shore:boat:incident", gatewayIdentity: "shore", recordID: 17,
            nodeUID: "boat", title: "SOS", detail: "", status: .open
        )

        XCTAssertEqual(
            activeRescueAlert(
                status: resolvedWithoutID, isCurrent: true,
                gatewayIdentity: "shore", alerts: [local]
            )?.recordID,
            17
        )
        XCTAssertEqual(
            rescuePresentationState(
                status: resolvedWithoutID, hasLocalAlert: true,
                localAlertRecordID: 17, isCurrent: true
            ),
            .activeAlert
        )

        let resolvedOtherIncident = GatewayStatus(
            storageReady: true, journalCount: 1, latestNodeUID: "boat",
            lastPacketAgeMS: 0, radioHealth: "recent", rssi: -80, snr: 5,
            alertState: "resolved", activeRecordID: 99, severity: "none", pairingOpen: false
        )
        XCTAssertEqual(
            activeRescueAlert(
                status: resolvedOtherIncident, isCurrent: true,
                gatewayIdentity: "shore", alerts: [local]
            )?.recordID,
            17
        )
        XCTAssertEqual(
            rescuePresentationState(
                status: resolvedOtherIncident, hasLocalAlert: true,
                localAlertRecordID: 17, isCurrent: true
            ),
            .activeAlert
        )
    }

    func testGatewayEventDurabilityDefaultsByDeliveryContext() throws {
        let live = try JSONDecoder().decode(
            GatewayEvent.self,
            from: Data(#"{"kind":"event","record_id":1,"node_uid":"boat","nonce":"live"}"#.utf8)
        )
        let sync = try JSONDecoder().decode(
            GatewayEvent.self,
            from: Data(#"{"kind":"event","record_id":2,"sync_id":"page","node_uid":"boat","nonce":"sync"}"#.utf8)
        )
        let explicitLiveDurable = try JSONDecoder().decode(
            GatewayEvent.self,
            from: Data(#"{"kind":"event","record_id":3,"node_uid":"boat","nonce":"live-durable","durable":true}"#.utf8)
        )

        XCTAssertFalse(live.isDurable)
        XCTAssertTrue(sync.isDurable)
        XCTAssertTrue(explicitLiveDurable.isDurable)
    }

    func testGPSQualityIsAValidEventTimestamp() {
        let gps = BoatSnapshot(
            eventKey: "gps", recordID: 1, nodeUID: "boat", nonce: "gps",
            occurredAt: Date(timeIntervalSince1970: 1), syncedAt: Date(timeIntervalSince1970: 2),
            timeQuality: "gps", latitude: nil, longitude: nil,
            batteryPercent: nil, rssi: nil, snr: nil, payload: ""
        )
        let uptimeOnly = BoatSnapshot(
            eventKey: "uptime", recordID: 2, nodeUID: "boat", nonce: "uptime",
            occurredAt: Date(timeIntervalSince1970: 1), syncedAt: Date(timeIntervalSince1970: 2),
            timeQuality: "uptime_only", latitude: nil, longitude: nil,
            batteryPercent: nil, rssi: nil, snr: nil, payload: ""
        )

        XCTAssertTrue(gps.hasReliableEventTime)
        XCTAssertFalse(uptimeOnly.hasReliableEventTime)
    }

    func testSuccessfulConnectionResetsSoftTimeoutBudget() {
        var budget = ConnectionRetryBudget()
        XCTAssertTrue(budget.consumeSoftRetry())
        XCTAssertFalse(budget.consumeSoftRetry())
        budget.reset()
        XCTAssertTrue(budget.consumeSoftRetry())
    }

    func testCommandWriteQueueAllowsExactlyOneInFlightWrite() {
        var queue = CommandWriteQueue()
        let first = commandWrite("sync-1", kind: .other)
        let second = commandWrite("sync-2", kind: .other)
        queue.enqueue(first)
        queue.enqueue(second)

        XCTAssertEqual(queue.startNext(), first)
        XCTAssertNil(queue.startNext())
        XCTAssertEqual(queue.inFlight, first)
        XCTAssertEqual(queue.pending, [second])

        XCTAssertEqual(queue.completeInFlight(), first)
        XCTAssertEqual(queue.startNext(), second)
    }

    func testCommandWriteQueueRunsAlertImmediatelyAfterCurrentSync() {
        var queue = CommandWriteQueue()
        let currentSync = commandWrite("sync-current", kind: .other)
        let queuedSync = commandWrite("sync-queued", kind: .other)
        let alert = commandWrite("ack", kind: .alert("command-ack"))
        queue.enqueue(currentSync)
        XCTAssertEqual(queue.startNext(), currentSync)
        queue.enqueue(queuedSync)
        queue.enqueue(alert)

        XCTAssertEqual(queue.completeInFlight(), currentSync)
        XCTAssertEqual(queue.startNext(), alert)
        XCTAssertEqual(queue.completeInFlight(), alert)
        XCTAssertEqual(queue.startNext(), queuedSync)
    }

    func testCommandWriteQueuePreservesExactAlertMetadataOnCompletion() {
        var queue = CommandWriteQueue()
        let alert = commandWrite("resolve", kind: .alert("exact-command-id"))
        queue.enqueue(alert)

        XCTAssertEqual(queue.startNext(), alert)
        guard case .alert(let commandID) = queue.completeInFlight()?.kind else {
            return XCTFail("Expected the completed in-flight write to retain alert metadata")
        }
        XCTAssertEqual(commandID, "exact-command-id")
        XCTAssertNil(queue.inFlight)
    }

    func testCommandWriteQueueResetClearsPendingAndInFlightWrites() {
        var queue = CommandWriteQueue()
        queue.enqueue(commandWrite("in-flight", kind: .other))
        _ = queue.startNext()
        queue.enqueue(commandWrite("pending", kind: .alert("pending-alert")))

        queue.reset()

        XCTAssertNil(queue.inFlight)
        XCTAssertTrue(queue.pending.isEmpty)
        XCTAssertNil(queue.startNext())
    }

    private func fragment(id: UInt32, index: UInt16, count: UInt16, crc: UInt32, payload: Data) -> Data {
        var bytes: [UInt8] = [0x53, 0x4D, 1, 1]
        bytes += littleEndian(id)
        bytes += littleEndian(index)
        bytes += littleEndian(count)
        bytes += littleEndian(crc)
        return Data(bytes) + payload
    }

    private func littleEndian(_ value: UInt16) -> [UInt8] { [UInt8(value & 0xFF), UInt8(value >> 8)] }
    private func littleEndian(_ value: UInt32) -> [UInt8] { [UInt8(value & 0xFF), UInt8((value >> 8) & 0xFF), UInt8((value >> 16) & 0xFF), UInt8((value >> 24) & 0xFF)] }

    private func commandWrite(_ value: String, kind: CommandWriteKind) -> QueuedCommandWrite {
        QueuedCommandWrite(data: Data(value.utf8), kind: kind)
    }

    @MainActor private func testDefaults() -> UserDefaults {
        let name = "CoastCueTests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defaults.removePersistentDomain(forName: name)
        return defaults
    }

    @MainActor private func event(recordID: Int64, syncID: String, nodeUID: String = "boat", nonce: String? = nil, status: String? = nil, extras: [String: Any] = [:]) -> [String: Any] {
        var value: [String: Any] = ["kind": "event", "record_id": recordID, "sync_id": syncID, "node_uid": nodeUID, "nonce": nonce ?? String(recordID)]
        if let status { value["status"] = status }
        extras.forEach { value[$0.key] = $0.value }
        return value
    }

    private func commandResult(
        for command: FakeGatewayService.AlertCommand,
        ok: Bool,
        error: String? = nil
    ) -> [String: Any] {
        var value: [String: Any] = [
            "kind": "command_result",
            "command_id": command.commandID,
            "op": command.op,
            "record_id": command.recordID,
            "source": "ble",
            "ok": ok,
        ]
        if let error {
            value["error"] = error
        }
        return value
    }

    @MainActor
    private func assertLinkAndSyncPresentationAreConsistent(_ model: AppModel, file: StaticString = #filePath, line: UInt = #line) {
        let terminalFailure = String(localized: "Sync failed. Tap Sync to retry.")
        let showsSyncFailed = model.syncMessage == terminalFailure || model.lastError == terminalFailure
        if showsSyncFailed {
            XCTAssertNotEqual(model.connectionState, .connected, "Sync failed must not share the Connected link state", file: file, line: line)
            XCTAssertNotEqual(model.connectionState.displayName, String(localized: "Connected"), file: file, line: line)
        }
        if model.connectionState == .connected {
            XCTAssertNotEqual(model.syncMessage, terminalFailure, file: file, line: line)
            XCTAssertNotEqual(model.lastError, terminalFailure, file: file, line: line)
        }
    }
}

@MainActor
private final class FakeGatewayService: @preconcurrency GatewayBluetoothService {
    struct SyncRequest { let after: Int64; let limit: Int; let syncID: String }
    struct AlertCommand { let op: String; let recordID: Int64; let commandID: String }
    var onStatus: (@MainActor (GatewayStatus) -> Void)?
    var onEventData: (@MainActor (Data) -> Void)?
    var onConnectionChanged: (@MainActor (BluetoothGatewayService.ConnectionState) -> Void)?
    var onError: (@MainActor (String) -> Void)?
    var onAlertCommandWriteFailed: (@MainActor (String) -> Void)?
    var onDeviceInfo: (@MainActor (GatewayDeviceInfo) -> Void)?
    var onEnrollmentCandidate: (@MainActor (GatewayEnrollmentCandidate) -> Void)?
    var savedGatewayIdentity: String?
    var syncRequests: [SyncRequest] = []
    var alertCommands: [AlertCommand] = []
    var confirmedCandidates: [GatewayEnrollmentCandidate] = []
    var cancelledCandidates: [GatewayEnrollmentCandidate] = []
    var statusOnRead: GatewayStatus?
    var statusReadRequests = 0
    var finishSyncRequests = 0
    var forgetRequests = 0
    var autoDeliverDeviceInfoOnConnect = true
    var alertCommandSendShouldFail = false
    var completeAlertCommandDuringSend = false
    private var nextMessageID: UInt32 = 1
    private var candidateGeneration = 0

    func connect() {
        onConnectionChanged?(.connected)
        if autoDeliverDeviceInfoOnConnect {
            emitDeviceInfo(nodeUID: "gw", epoch: "epoch")
        }
    }
    func sync(after: Int64, syncID: String) {
        sync(after: after, limit: AppModel.syncPageLimit, syncID: syncID)
    }
    func sync(after: Int64, limit: Int, syncID: String) {
        syncRequests.append(.init(after: after, limit: limit, syncID: syncID))
        onConnectionChanged?(.syncing)
    }
    func finishSync() {
        finishSyncRequests += 1
        onConnectionChanged?(.connected)
    }
    func readStatus() {
        statusReadRequests += 1
        if let statusOnRead { onStatus?(statusOnRead) }
    }
    @discardableResult
    func acknowledge(recordID: Int64, commandID: String) -> Bool {
        guard !alertCommandSendShouldFail else {
            onError?(String(localized: "The gateway command channel is not ready."))
            return false
        }
        alertCommands.append(.init(op: "ack", recordID: recordID, commandID: commandID))
        if completeAlertCommandDuringSend {
            emit([
                "kind": "command_result",
                "command_id": commandID,
                "op": "ack",
                "record_id": recordID,
                "source": "ble",
                "ok": true,
            ])
        }
        return true
    }
    @discardableResult
    func resolve(recordID: Int64, commandID: String) -> Bool {
        guard !alertCommandSendShouldFail else {
            onError?(String(localized: "The gateway command channel is not ready."))
            return false
        }
        alertCommands.append(.init(op: "resolve", recordID: recordID, commandID: commandID))
        return true
    }
    func failAlertCommandWrite(commandID: String) {
        onAlertCommandWriteFailed?(commandID)
    }
    func disconnect() { onConnectionChanged?(.idle) }
    func forgetGateway() {
        forgetRequests += 1
        savedGatewayIdentity = nil
        disconnect()
    }
    func confirmEnrollment(_ candidate: GatewayEnrollmentCandidate) {
        confirmedCandidates.append(candidate)
        onDeviceInfo?(candidate.info)
    }
    func cancelEnrollment(_ candidate: GatewayEnrollmentCandidate) {
        cancelledCandidates.append(candidate)
        disconnect()
    }
    func emitDeviceInfo(nodeUID: String, epoch: String) {
        onDeviceInfo?(
            GatewayDeviceInfo(
                name: "CoastCue Shore",
                nodeUID: nodeUID,
                journalEpoch: epoch,
                firmware: "test-firmware"
            )
        )
    }
    func emitEnrollmentCandidate(nodeUID: String, epoch: String) -> GatewayEnrollmentCandidate {
        candidateGeneration += 1
        let candidate = GatewayEnrollmentCandidate(
            peripheralID: UUID(),
            info: GatewayDeviceInfo(
                name: "CoastCue Shore",
                nodeUID: nodeUID,
                journalEpoch: epoch,
                firmware: "test-firmware"
            ),
            connectionGeneration: candidateGeneration
        )
        onEnrollmentCandidate?(candidate)
        return candidate
    }
    func emit(_ object: [String: Any]) {
        let data = try! JSONSerialization.data(withJSONObject: object)
        let messageID = nextMessageID
        nextMessageID &+= 1
        GatewayMessageFramer.frames(for: data, messageID: messageID).forEach { onEventData?($0) }
    }
}

private enum TestSaveError: Error { case failed }

private extension BoatSnapshot {
    convenience init(eventKey: String, gatewayIdentity: String, recordID: Int64, nodeUID: String, nonce: String, receivedAt: Date, timeQuality: String, latitude: Double?, longitude: Double?, batteryPercent: Double?, rssi: Double?, snr: Double?, payload: String) {
        self.init(eventKey: eventKey, recordID: recordID, nodeUID: nodeUID, nonce: nonce, receivedAt: receivedAt, timeQuality: timeQuality, latitude: latitude, longitude: longitude, batteryPercent: batteryPercent, rssi: rssi, snr: snr, payload: payload)
        self.gatewayIdentity = gatewayIdentity
    }
}
