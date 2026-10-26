import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / "ESP-32-CoastCue/pio/src/shore_runtime.cpp"
IOS = ROOT / "ios/CoastCue/BluetoothGatewayService.swift"
IOS_FRAMER = ROOT / "ios/CoastCue/BLEProtocol.swift"
PROTOCOL = ROOT / "docs/PROTOCOL.md"


class ProtocolContractTests(unittest.TestCase):
    def test_ble_uuids_match_firmware_ios_and_protocol(self):
        firmware = FIRMWARE.read_text(encoding="utf-8")
        ios = IOS.read_text(encoding="utf-8")
        protocol = PROTOCOL.read_text(encoding="utf-8")

        contract_labels = {
            "service": "Service",
            "status": "Status",
            "events": "Events",
            "command": "Command",
            "device_info": "Device info",
        }
        expected = {}
        for key, label in contract_labels.items():
            match = re.search(
                rf"^\| {re.escape(label)} \| `([0-9A-F-]+)` \|", protocol, re.MULTILINE
            )
            self.assertIsNotNone(match, f"Missing {label} UUID in PROTOCOL.md")
            expected[key] = match.group(1)

        firmware_names = {
            "service": "SERVICE_UUID", "status": "STATUS_UUID",
            "events": "EVENTS_UUID", "command": "COMMAND_UUID",
            "device_info": "DEVICE_INFO_UUID",
        }
        ios_names = {
            "service": "serviceUUID",
            "status": "statusUUID",
            "events": "eventsUUID",
            "command": "commandUUID",
            "device_info": "deviceInfoUUID",
        }

        for key, contract_uuid in expected.items():
            with self.subTest(uuid=key):
                firmware_match = re.search(
                    rf'{firmware_names[key]}\s*=\s*"([0-9A-F-]+)"', firmware
                )
                ios_match = re.search(
                    rf'{ios_names[key]}:\s*CBUUID\s*\{{\s*CBUUID\(string:\s*"([0-9A-F-]+)"\)',
                    ios,
                )
                self.assertIsNotNone(firmware_match)
                self.assertIsNotNone(ios_match)
                self.assertEqual(firmware_match.group(1), contract_uuid)
                self.assertEqual(ios_match.group(1), contract_uuid)

    def test_events_frame_constants_match_current_contract(self):
        firmware = FIRMWARE.read_text(encoding="utf-8")
        ios = IOS_FRAMER.read_text(encoding="utf-8")
        protocol = PROTOCOL.read_text(encoding="utf-8")

        self.assertRegex(firmware, r"EVENT_MESSAGE_TYPE\s*=\s*1;")
        self.assertRegex(firmware, r"EVENT_CHUNK_BYTES\s*=\s*160;")
        self.assertRegex(ios, r"unframedMessageLimit\s*=\s*160")
        self.assertIn("ASCII `SM`", protocol)
        self.assertIn("format marker `1`", protocol)
        self.assertIn("message type `1`", protocol)
        self.assertIn("at most 160 JSON bytes", protocol)


    def test_arduinojson_null_default_regression_stays_fixed(self):
        # This overload previously broke real command IDs and state receipts.
        for name in ("main.cpp", "shore_runtime.cpp"):
            source = (FIRMWARE.parent / name).read_text(encoding="utf-8")
            self.assertIsNone(re.search(r"\]\s*\|\s*nullptr", source), name)


if __name__ == "__main__":
    unittest.main()
