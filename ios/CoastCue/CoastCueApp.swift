import SwiftData
import SwiftUI

@main
struct CoastCueApp: App {
  @State private var model: AppModel

  init() {
    #if DEBUG
    let demo = ProcessInfo.processInfo.arguments.contains("-CoastCueDemo")
    _model = State(initialValue: AppModel(ble: demo ? DemoGatewayService() : BluetoothGatewayService()))
    #else
    _model = State(initialValue: AppModel(ble: BluetoothGatewayService()))
    #endif
  }

  var body: some Scene {
    WindowGroup {
      AppShell(model: model)
    }
    .modelContainer(for: [BoatSnapshot.self, MarineAlert.self])
  }
}
