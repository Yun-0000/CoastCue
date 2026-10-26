#!/usr/bin/env python3
import argparse
import time


FATAL_MARKERS = (
    "[FATAL]",
    "Guru Meditation Error",
    "panic'ed",
    "abort() was called",
)


def validate_smoke(lines, role):
    if not role:
        return 0

    output = "\n".join(lines)
    fatal = next((marker for marker in FATAL_MARKERS if marker in output), None)
    if fatal:
        print(f"Serial smoke failed: fatal marker observed ({fatal}).")
        return 2

    expected = [
        f"[CoastCue] boot role={role}",
        "[CoastCue] transport=LoRa",
        "[CoastCue] pmu=",
        "ready=yes",
        "[LORA] ready",
    ]
    missing = [marker for marker in expected if marker not in output]
    if missing:
        print("Serial smoke failed: expected boot markers were not observed: " + ", ".join(missing))
        return 3

    print(f"Serial smoke passed for {role} role.")
    return 0


def capture_lines(port, baud, seconds, serial_factory, monotonic=time.monotonic, sleep=time.sleep):
    """Capture decoded lines for the requested duration after the board reset pause."""
    lines = []
    with serial_factory(port, baud, timeout=0.2) as ser:
        # Opening a USB serial port often resets ESP32 boards.  The requested
        # capture duration starts only after that reset window has elapsed.
        sleep(1.5)
        deadline = monotonic() + seconds
        while monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            lines.append(line)
            print(line, flush=True)
    return lines


def main():
    parser = argparse.ArgumentParser(
        description="Capture bounded serial output for hardware loops."
    )
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument(
        "--smoke-role",
        choices=("boat", "shore"),
        help="Require expected boot, PMU, and LoRa markers; ordinary capture stays permissive.",
    )
    args = parser.parse_args()

    if args.seconds <= 0:
        parser.error("--seconds must be greater than zero")

    try:
        import serial
    except ImportError as exc:
        parser.error(
            "PySerial is unavailable in this interpreter. Run this through "
            "`make tooling-setup` or `make serial`, not system python."
        )

    print(
        f"Capturing serial from {args.port} at {args.baud} baud for {args.seconds:g}s.",
        flush=True,
    )

    lines = capture_lines(args.port, args.baud, args.seconds, serial.Serial)

    print("Serial capture complete.", flush=True)
    return validate_smoke(lines, args.smoke_role)


if __name__ == "__main__":
    raise SystemExit(main())
