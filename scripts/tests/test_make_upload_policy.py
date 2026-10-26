import subprocess
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]


class MakeUploadPolicyTests(unittest.TestCase):
    def run_make(self, *arguments):
        return subprocess.run(
            ["make", "--no-print-directory", *arguments],
            cwd=REPOSITORY_ROOT,
            capture_output=True,
            text=True,
            timeout=10,
        )

    def assert_rejected_before_hardware_access(self, result):
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("platformio run", output)
        self.assertNotIn("port_guard.py", output)

    def test_direct_upload_entry_point_is_rejected_before_hardware_access(self):
        result = self.run_make(
                "upload",
                "PIO_ENV=boat",
                "PORT=/dev/cu.coastcue-policy-test",
        )

        output = result.stdout + result.stderr
        self.assert_rejected_before_hardware_access(result)
        self.assertIn("Direct upload is disabled", output)

    def test_removed_internal_upload_target_cannot_be_revived_with_a_make_variable(self):
        result = self.run_make(
            "upload-internal",
            "COASTCUE_BOARD_LOOP=1",
            "PIO_ENV=boat",
            "PORT=/dev/cu.coastcue-policy-test",
        )

        self.assert_rejected_before_hardware_access(result)

    def test_shore_storage_initialization_requires_explicit_erase_confirmation(self):
        result = self.run_make(
                "init-shore-storage",
                "PORT=/dev/cu.coastcue-policy-test",
        )

        output = result.stdout + result.stderr
        self.assert_rejected_before_hardware_access(result)
        self.assertIn("CONFIRM_STORAGE_FORMAT=ERASE", output)

    def test_removed_internal_storage_target_cannot_bypass_erase_confirmation(self):
        result = self.run_make(
            "init-shore-storage-internal",
            "COASTCUE_STORAGE_INIT=1",
            "PORT=/dev/cu.coastcue-policy-test",
        )

        self.assert_rejected_before_hardware_access(result)

    def test_generic_board_loop_derives_role_specific_serial_smoke(self):
        for environment, expected_role in (("boat", "boat"), ("shore-us", "shore")):
            with self.subTest(environment=environment):
                result = self.run_make(
                    "--just-print",
                    "board-loop",
                    f"PIO_ENV={environment}",
                    "PORT=/dev/cu.coastcue-policy-test",
                    "SERIAL_SECONDS=7",
                )

                output = result.stdout + result.stderr
                self.assertEqual(result.returncode, 0, output)
                self.assertIn(f"--smoke-role {expected_role}", output)

    def test_us_915_upload_role_requires_frequency_confirmation(self):
        rejected = self.run_make("require-role", "PIO_ENV=boat-us")
        accepted = self.run_make(
            "require-role",
            "PIO_ENV=boat-us-axp192",
            "CONFIRM_US_915_UPLOAD=US_915",
        )

        rejected_output = rejected.stdout + rejected.stderr
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("CONFIRM_US_915_UPLOAD=US_915", rejected_output)
        self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)


if __name__ == "__main__":
    unittest.main()
