import configparser
import math
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def platformio():
    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / "ESP-32-CoastCue/pio/platformio.ini")
    return config


def flags(text):
    return dict(flag.removeprefix("-D").split("=", 1) for flag in text.split() if "=" in flag)


class FirmwareProfileTests(unittest.TestCase):
    def test_shore_uses_t3_pins(self):
        # The pin header has no guard for this: a wrong profile still compiles.
        self.assertIn("COASTCUE_BOARD_LILYGO_T3_V1_6", platformio()["env:shore"]["build_flags"])

    def test_maximum_us_packet_airtime_fits_receive_window(self):
        # Semtech explicit-header LoRa formula: CRC on, 8-symbol preamble, DE=0.
        radio = flags(platformio()["region"]["us"])
        sf = int(radio["COASTCUE_LORA_SPREADING_FACTOR"])
        bw = float(radio["COASTCUE_LORA_BANDWIDTH_KHZ"]) * 1000
        cr = int(radio["COASTCUE_LORA_CODING_RATE"])
        symbols = 8 + math.ceil((8 * 255 - 4 * sf + 28 + 16) / (4 * sf)) * cr
        airtime = (8 + 4.25 + symbols) * 2 ** sf / bw
        self.assertAlmostEqual(airtime, 0.429312)
        firmware = (ROOT / "ESP-32-CoastCue/pio/src/main.cpp").read_text(encoding="utf-8")
        window_ms = int(re.search(r"LORA_RESPONSE_WINDOW_MS = (\d+);", firmware).group(1))
        self.assertLess(3 * airtime + 1, window_ms / 1000)  # 1 s reserved for turnaround


if __name__ == "__main__":
    unittest.main()
