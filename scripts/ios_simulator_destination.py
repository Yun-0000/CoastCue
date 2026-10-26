#!/usr/bin/env python3
"""Print an Xcode-eligible iOS Simulator destination, or nothing when absent."""

import argparse
import json
import re
import subprocess


def select_destination(devices: dict) -> str | None:
    candidates = []
    for runtime, runtime_devices in devices.items():
        match = re.fullmatch(r"com\.apple\.CoreSimulator\.SimRuntime\.iOS-(\d+)(?:-(\d+))?", runtime)
        if not match:
            continue
        version = (int(match.group(1)), int(match.group(2) or 0))
        for device in runtime_devices:
            if device.get("isAvailable", False):
                candidates.append((version, device.get("name", ""), device.get("udid", "")))

    if not candidates:
        return None
    _version, _name, udid = sorted(candidates, reverse=True)[0]
    return f"platform=iOS Simulator,id={udid}" if udid else None


def select_xcode_destination(output: str) -> str | None:
    """Read only the eligible-destination section produced by xcodebuild."""
    if "Available destinations" not in output:
        return None
    eligible = output.split("Ineligible destinations", maxsplit=1)[0]
    for entry in re.findall(r"\{([^}]*)\}", eligible):
        if "platform:iOS Simulator" not in entry or "error:" in entry:
            continue
        match = re.search(r"\bid:([^,}\s]+)", entry)
        if match and "placeholder" not in match.group(1).lower():
            return f"platform=iOS Simulator,id={match.group(1)}"
    return None


def xcode_destination(project: str, scheme: str) -> str | None:
    try:
        result = subprocess.run(
            ["xcodebuild", "-showdestinations", "-project", project, "-scheme", scheme],
            capture_output=True,
            text=True,
        )
    except FileNotFoundError:
        return None
    return select_xcode_destination(result.stdout + result.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project")
    parser.add_argument("--scheme")
    args = parser.parse_args()

    if bool(args.project) != bool(args.scheme):
        parser.error("--project and --scheme must be supplied together")
    if args.project:
        destination = xcode_destination(args.project, args.scheme)
        if destination:
            print(destination)
        return 0

    try:
        result = subprocess.run(
            ["xcrun", "simctl", "list", "devices", "available", "--json"],
            check=True,
            capture_output=True,
            text=True,
        )
        devices = json.loads(result.stdout).get("devices", {})
    except (FileNotFoundError, subprocess.CalledProcessError, json.JSONDecodeError):
        return 0

    destination = select_destination(devices)
    if destination:
        print(destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
