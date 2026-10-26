#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys


ESP32_HINTS = (
    "10C4:EA60",  # Silicon Labs CP210x
    "1A86:7523",  # WCH CH340
    "1A86:5523",  # WCH CH341 variants
    "1A86:55D4",  # WCH USB serial variants
    "0403:6001",  # FTDI
    "0403:6015",  # FTDI
    "303A:0002",  # Espressif USB JTAG/serial variants
    "303A:1001",
    "303A:1002",
)

def load_devices():
    platformio = os.environ.get("COASTCUE_PLATFORMIO", "pio").split()
    result = subprocess.run(
        [*platformio, "device", "list", "--json-output"],
        check=True,
        capture_output=True,
        text=True,
        env=os.environ.copy(),
    )
    return json.loads(result.stdout)


def is_likely_esp32(device):
    hwid = device.get("hwid", "")
    description = device.get("description", "")
    combined = f"{hwid} {description}".upper()

    # Device names such as /dev/cu.usbmodem* and /dev/cu.usbserial* are
    # generic. Only a recognized USB identity may bypass explicit confirmation.
    return any(hint in combined for hint in ESP32_HINTS)


def main():
    parser = argparse.ArgumentParser(
        description="Prevent flashing ESP32 firmware to an obviously wrong port."
    )
    parser.add_argument("--port", required=True)
    parser.add_argument(
        "--allow-unknown",
        action="store_true",
        help="Allow a non-ESP32-looking port after explicit user confirmation.",
    )
    args = parser.parse_args()

    devices = load_devices()
    device = next((item for item in devices if item.get("port") == args.port), None)
    if not device:
        print(f"Port not found: {args.port}", file=sys.stderr)
        return 2

    if is_likely_esp32(device):
        print(f"Port accepted for ESP32 upload: {args.port}")
        return 0

    print(
        "Port does not look like an ESP32 USB serial adapter:\n"
        f"  port: {device.get('port')}\n"
        f"  hwid: {device.get('hwid')}\n"
        f"  description: {device.get('description')}",
        file=sys.stderr,
    )
    if args.allow_unknown:
        print("ALLOW_UNKNOWN_PORT enabled; continuing after explicit override.")
        return 0

    print(
        "Refusing upload. Connect the ESP32 or rerun with ALLOW_UNKNOWN_PORT=1 only after confirming the board.",
        file=sys.stderr,
    )
    return 3


if __name__ == "__main__":
    raise SystemExit(main())
