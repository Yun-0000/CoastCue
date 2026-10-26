import pathlib
import sys
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import ios_simulator_destination as simulator  # noqa: E402


class SimulatorDestinationTests(unittest.TestCase):
    def test_selects_newest_available_ios_runtime_numerically(self):
        devices = {
            "com.apple.CoreSimulator.SimRuntime.iOS-9-3": [
                {"name": "iPhone 6", "udid": "ios-9", "isAvailable": True}
            ],
            "com.apple.CoreSimulator.SimRuntime.iOS-18-5": [
                {"name": "iPhone 16", "udid": "ios-18", "isAvailable": True}
            ],
        }

        self.assertEqual(
            simulator.select_destination(devices), "platform=iOS Simulator,id=ios-18"
        )

    def test_ignores_unavailable_and_non_ios_devices(self):
        devices = {
            "com.apple.CoreSimulator.SimRuntime.tvOS-18-0": [
                {"name": "Apple TV", "udid": "tv", "isAvailable": True}
            ],
            "com.apple.CoreSimulator.SimRuntime.iOS-18-5": [
                {"name": "iPhone", "udid": "old", "isAvailable": False}
            ],
        }

        self.assertIsNone(simulator.select_destination(devices))

    def test_extracts_only_an_xcode_eligible_simulator(self):
        output = """
        Available destinations for the \"CoastCue\" scheme:
            { platform:iOS Simulator, arch:arm64, id:ABC-123, OS:26.5, name:iPhone 16 }
        """

        self.assertEqual(
            simulator.select_xcode_destination(output),
            "platform=iOS Simulator,id=ABC-123",
        )

    def test_rejects_xcode_placeholder_destination(self):
        output = """
        Available destinations for the \"CoastCue\" scheme:
            { platform:iOS Simulator, id:dvtdevice-DVTiOSDeviceSimulatorPlaceholder-iphonesimulator:placeholder, name:Any iOS Simulator Device, error:iOS 26.5 is not installed. }
        """

        self.assertIsNone(simulator.select_xcode_destination(output))


if __name__ == "__main__":
    unittest.main()
