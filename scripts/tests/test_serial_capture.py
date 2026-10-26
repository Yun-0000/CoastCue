import contextlib
import io
import pathlib
import sys
import types
import unittest


# The capture module imports PySerial today.  Keep these unit tests independent
# from the host interpreter so the project tooling is the only runtime
# dependency for actual serial access.
sys.modules.setdefault("serial", types.SimpleNamespace(Serial=object))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import serial_capture as capture  # noqa: E402


class FakeClock:
    def __init__(self):
        self.value = 0.0

    def monotonic(self):
        return self.value

    def sleep(self, seconds):
        self.value += seconds


class FakeSerial:
    def __init__(self, clock):
        self.clock = clock

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def readline(self):
        self.clock.value += 0.2
        return b"[CoastCue] boot role=shore\n"


class SerialCaptureTests(unittest.TestCase):
    def test_capture_budget_starts_after_serial_warmup(self):
        clock = FakeClock()
        opened = []

        def serial_factory(port, baud, timeout):
            opened.append((port, baud, timeout))
            return FakeSerial(clock)

        with contextlib.redirect_stdout(io.StringIO()):
            capture.capture_lines(
                port="/dev/fake",
                baud=115200,
                seconds=2,
                serial_factory=serial_factory,
                monotonic=clock.monotonic,
                sleep=clock.sleep,
            )

        self.assertEqual(opened, [("/dev/fake", 115200, 0.2)])
        self.assertGreaterEqual(clock.value, 3.5)

    def test_smoke_validation_rejects_fatal_output(self):
        self.assertEqual(capture.validate_smoke(["Guru Meditation Error"], "shore"), 2)


if __name__ == "__main__":
    unittest.main()
