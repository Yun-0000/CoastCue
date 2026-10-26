import pathlib
import secrets
import sys
import tempfile
import unittest
from uuid import uuid4


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import coastcue_deploy_config_check as config_check  # noqa: E402


class DeploymentConfigTests(unittest.TestCase):
    def write_config(self, text):
        directory = tempfile.TemporaryDirectory()
        path = pathlib.Path(directory.name) / "coastcue_config.h"
        path.write_text(text, encoding="utf-8")
        self.addCleanup(directory.cleanup)
        return path

    def test_obsolete_config_is_reported_without_echoing_values(self):
        path = self.write_config('#define COASTCUE_BOAT_ID "obsolete-value"\n')

        with self.assertRaisesRegex(ValueError, "obsolete schema"):
            config_check.load_required_defines(path)

    def test_valid_dummy_config_passes_validation(self):
        path = self.write_config(
            f'#define COASTCUE_BOAT_ID "{uuid4()}"\n'
            '#define COASTCUE_LORA_AUTH_KEY '
            f'"{secrets.token_hex(32)}"\n'
        )

        values = config_check.load_required_defines(path)
        config_check.validate_boat_id(values["COASTCUE_BOAT_ID"])
        config_check.validate_auth_key(values["COASTCUE_LORA_AUTH_KEY"])

    def test_documented_demo_key_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "placeholder"):
            config_check.validate_auth_key(
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            )

    def test_ci_fixture_key_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "placeholder"):
            config_check.validate_auth_key("f" * 64)

    def test_ci_fixture_keeps_template_defaults_without_becoming_deployable(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        template = pathlib.Path(directory.name) / "example.h"
        fixture = pathlib.Path(directory.name) / "coastcue_config.h"
        template.write_text(
            '#pragma once\n'
            '#define COASTCUE_BOAT_ID "00000000-0000-0000-0000-000000000000"\n'
            '#define COASTCUE_LORA_AUTH_KEY "replace-me"\n'
            '#define COASTCUE_REPORT_PERIOD_MS 300000\n',
            encoding="utf-8",
        )

        config_check.write_ci_fixture(template, fixture)

        self.assertIn("COASTCUE_REPORT_PERIOD_MS 300000", fixture.read_text())
        values = config_check.load_required_defines(fixture)
        with self.assertRaisesRegex(ValueError, "placeholder"):
            config_check.validate_auth_key(values["COASTCUE_LORA_AUTH_KEY"])


if __name__ == "__main__":
    unittest.main()
