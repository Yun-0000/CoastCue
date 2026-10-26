import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import serial_capture as capture  # noqa: E402


class SerialSmokeTests(unittest.TestCase):
    def test_fatal_output_fails(self):
        self.assertEqual(capture.validate_smoke(["Guru Meditation Error"], "shore"), 2)

    def test_missing_boot_marker_fails(self):
        self.assertEqual(capture.validate_smoke(["[CoastCue] boot role=boat"], "boat"), 3)

    def test_complete_boot_passes(self):
        lines = ["[CoastCue] boot role=boat", "[CoastCue] transport=LoRa",
                 "[CoastCue] pmu=AXP2101 ready=yes", "[LORA] ready"]
        self.assertEqual(capture.validate_smoke(lines, "boat"), 0)


if __name__ == "__main__":
    unittest.main()
