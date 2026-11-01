import math
import socket
import struct
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import smoother_core as m  # noqa: E402


def quat_yaw(deg):
    h = math.radians(deg) / 2
    return [math.cos(h), 0.0, 0.0, math.sin(h)]


def yaw_of(q):
    return math.degrees(2 * math.atan2(q[3], q[0]))


class ParseTests(unittest.TestCase):
    def test_roundtrip(self):
        msg = m.build_message("/Virtuoso/quat", [1.0, 0.0, 0.0, 0.0])
        self.assertEqual(m.parse_numbers(msg), ("/Virtuoso/quat", [1.0, 0.0, 0.0, 0.0]))

    def test_bundle(self):
        inner = m.build_message("/a", [1.0, 2.0, 3.0])
        bundle = b"#bundle\0" + b"\0" * 7 + b"\1" + struct.pack(">i", len(inner)) + inner
        self.assertEqual(m.split_packet(bundle), [inner])

    def test_other_messages_ignored(self):
        msg = m._osc_string("/x") + m._osc_string(",i") + struct.pack(">i", 3)
        self.assertIsNone(m.parse_numbers(msg))


class EngineTests(unittest.TestCase):
    def run_engine(self, messages, bypass=False):
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rx.bind(("127.0.0.1", 0))
        rx.settimeout(0.5)
        out_port = rx.getsockname()[1]
        engine = m.Engine()
        engine.bypass = bypass
        engine.start(0, out_port)
        in_port = engine._rx.getsockname()[1]
        got = []

        def reader():
            end = time.time() + 1.6
            while time.time() < end:
                try:
                    got.append(rx.recvfrom(4096)[0])
                except OSError:
                    break

        th = threading.Thread(target=reader)
        th.start()
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for msg in messages:
            tx.sendto(msg, ("127.0.0.1", in_port))
            time.sleep(0.04)  # 25 Hz wie Bridgehead
        th.join()
        engine.stop()
        rx.close()
        return got

    def test_quaternion_is_smoothed_and_reaches_target(self):
        msgs = [m.build_message("/Virtuoso/quat", quat_yaw(45 * i / 24)) for i in range(25)]
        got = [m.parse_numbers(d)[1] for d in self.run_engine(msgs)]
        yaws = [yaw_of(q) for q in got]
        self.assertGreater(len(yaws), 150)  # deutlich mehr als 25 Eingangswerte
        self.assertAlmostEqual(yaws[-1], 45.0, places=1)
        self.assertLess(max(abs(b - a) for a, b in zip(yaws, yaws[1:])), 1.0)  # Eingang: 1,9 Grad

    def test_angles_wrap_shortest_way(self):
        msgs = []
        for i in range(25):
            yaw = (170 + 20 * i / 24 + 180) % 360 - 180
            msgs.append(m.build_message("/ypr", [yaw, 0.0, 0.0]))
        got = [m.parse_numbers(d)[1][0] for d in self.run_engine(msgs)]
        steps = [abs((b - a + 180) % 360 - 180) for a, b in zip(got, got[1:])]
        self.assertLess(max(steps), 1.0)
        self.assertAlmostEqual(got[-1], -170.0, places=1)

    def test_bypass_passes_raw_and_other_messages(self):
        other = m._osc_string("/x") + m._osc_string(",i") + struct.pack(">i", 3)
        quat = m.build_message("/Virtuoso/quat", quat_yaw(10))
        got = self.run_engine([other, quat], bypass=True)
        self.assertEqual(got, [other, quat])


if __name__ == "__main__":
    unittest.main()
