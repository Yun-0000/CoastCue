#!/usr/bin/env python3
"""Validate local-only CoastCue firmware values without echoing secrets."""

import argparse
import os
import re
import sys
from pathlib import Path
from uuid import UUID


DEFINE_PATTERN = re.compile(
    r'^\s*#define\s+(COASTCUE_BOAT_ID|COASTCUE_LORA_AUTH_KEY)\s+"([^"]*)"\s*$',
    re.MULTILINE,
)
PLACEHOLDER_BOAT_IDS = {
    "00000000-0000-0000-0000-000000000000",
    "11111111-1111-4111-8111-111111111111",
    "22222222-2222-4222-8222-222222222222",
}
AUTH_KEY_PATTERN = re.compile(r"^[0-9a-f]{64}$")
PLACEHOLDER_AUTH_KEYS = {
    "0" * 64,
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "f" * 64,
}
CI_BOAT_ID = "22222222-2222-4222-8222-222222222222"
CI_AUTH_KEY = "f" * 64


def load_required_defines(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise ValueError("local firmware config is missing")
    os.chmod(path, 0o600)
    values = dict(DEFINE_PATTERN.findall(path.read_text(encoding="utf-8")))
    missing = sorted({"COASTCUE_BOAT_ID", "COASTCUE_LORA_AUTH_KEY"} - values.keys())
    if missing:
        raise ValueError(f"local firmware config is missing {', '.join(missing)}; "
                         "copy it from coastcue_config.example.h")
    return values


def check_schema(path: Path) -> None:
    if not path.is_file():
        return
    load_required_defines(path)


def write_ci_fixture(template: Path, destination: Path) -> None:
    """Create a build fixture from the current template without a usable key."""
    if not template.is_file():
        raise ValueError("firmware config template is missing")
    text = template.read_text(encoding="utf-8")
    for name, value in (("COASTCUE_BOAT_ID", CI_BOAT_ID), ("COASTCUE_LORA_AUTH_KEY", CI_AUTH_KEY)):
        text, replacements = re.subn(
            rf'(^\s*#define\s+{name}\s+)"[^"]*"(\s*$)',
            rf'\1"{value}"\2',
            text,
            flags=re.MULTILINE,
        )
        if replacements != 1:
            raise ValueError(f"firmware config template has no usable {name} define")
    destination.write_text(text, encoding="utf-8")


def validate_boat_id(value: str) -> None:
    try:
        parsed = UUID(value)
    except (TypeError, ValueError) as exc:
        raise ValueError("COASTCUE_BOAT_ID must be a UUID") from exc
    if str(parsed) in PLACEHOLDER_BOAT_IDS:
        raise ValueError("COASTCUE_BOAT_ID is a placeholder")


def validate_auth_key(value: str) -> None:
    if not AUTH_KEY_PATTERN.fullmatch(value):
        raise ValueError(
            "COASTCUE_LORA_AUTH_KEY must be 64 lowercase hexadecimal characters"
        )
    if value in PLACEHOLDER_AUTH_KEYS:
        raise ValueError("COASTCUE_LORA_AUTH_KEY is a placeholder")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate CoastCue firmware deployment config without printing secrets."
    )
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--template", type=Path)
    parser.add_argument(
        "--write-ci-fixture",
        action="store_true",
        help="Create a schema-valid, non-deployable fixture from a config template.",
    )
    parser.add_argument(
        "--check-schema",
        action="store_true",
        help="Check only that an existing private config uses the current schema.",
    )
    args = parser.parse_args()

    try:
        if args.write_ci_fixture:
            if not args.template:
                raise ValueError("--write-ci-fixture requires --template")
            write_ci_fixture(args.template, args.config)
            check_schema(args.config)
            print("CI deployment config fixture created.")
            return 0
        if args.check_schema:
            check_schema(args.config)
            print("Deployment config schema check passed.")
            return 0
        values = load_required_defines(args.config)
        validate_boat_id(values["COASTCUE_BOAT_ID"])
        validate_auth_key(values["COASTCUE_LORA_AUTH_KEY"])
    except (OSError, ValueError) as exc:
        print(f"Deployment config check failed: {exc}", file=sys.stderr)
        return 1

    print("Deployment config check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
