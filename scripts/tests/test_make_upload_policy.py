import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def make(*arguments):
    return subprocess.run(["make", "--no-print-directory", *arguments], cwd=ROOT,
                          capture_output=True, text=True, timeout=10)


class MakeUploadPolicyTests(unittest.TestCase):
    def test_storage_erase_needs_confirmation(self):
        result = make("init-shore-storage", "PORT=/dev/cu.policy-test")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CONFIRM_STORAGE_FORMAT=ERASE", result.stderr)
        self.assertNotIn("platformio", result.stdout)

    def test_us_upload_needs_915_confirmation(self):
        for env in ("boat", "shore"):
            self.assertNotEqual(make("require-role", f"PIO_ENV={env}").returncode, 0)
            self.assertEqual(make("require-role", f"PIO_ENV={env}", "CONFIRM_US_915_UPLOAD=US_915").returncode, 0)
        for env in ("unknown", "boat-cn", "shore-cn"):
            self.assertNotEqual(make("require-role", f"PIO_ENV={env}", "CONFIRM_US_915_UPLOAD=US_915").returncode, 0)

    def test_flash_captures_serial_for_its_role(self):
        for role in ("boat", "shore"):
            result = make("--just-print", "flash", f"PIO_ENV={role}", "PORT=/dev/cu.policy-test",
                          "CONFIRM_US_915_UPLOAD=US_915")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"ROLE={role}", result.stdout)


if __name__ == "__main__":
    unittest.main()
