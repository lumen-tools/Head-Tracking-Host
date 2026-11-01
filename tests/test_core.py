import socket
import struct
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import tracker_core as m  # noqa: E402


def value(msg):
    return msg[1] | (msg[2] << 7)


class OscTests(unittest.TestCase):
    def test_roundtrip(self):
        msg = m.build_message("/ypr", [10.0, -5.0, 2.0])
        self.assertEqual(m.parse_floats(msg), ("/ypr", [10.0, -5.0, 2.0]))

    def test_bundle(self):
        inner = m.build_message("/ypr", [1.0, 2.0, 3.0])
        bundle = b"#bundle\0" + b"\0" * 7 + b"\1" + struct.pack(">i", len(inner)) + inner
        self.assertEqual(m.split_packet(bundle), [inner])

    def test_non_float_ignored(self):
        msg = m._osc_string("/x") + m._osc_string(",i") + struct.pack(">i", 3)
        self.assertIsNone(m.parse_floats(msg))


class MidiTests(unittest.TestCase):
    def test_encoding(self):
        self.assertEqual(m.angle_to_14bit(0, -180, 180), 8192)
        self.assertEqual(m.angle_to_14bit(-180, -180, 180), 0)
        self.assertEqual(m.angle_to_14bit(180, -180, 180), 16383)
        self.assertEqual(m.angle_to_14bit(500, -180, 180), 16383)
        self.assertEqual(m.pitch_bend(1, 8192), [0xE1, 0x00, 0x40])

    def test_engine_sends_each_update_immediately(self):
        sent = []
        engine = m.Engine()
        engine.midi_send = sent.append
        engine.invert = [False, True, False]
        engine.start(0)
        port = engine._rx.getsockname()[1]
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        tx.sendto(m.build_message("/Virtuoso/quat", [1.0, 0.0, 0.0, 0.0]), ("127.0.0.1", port))  # ignored
        for i in range(10):
            tx.sendto(m.build_message("/ypr", [9.0 * i, 10.0, -5.0]), ("127.0.0.1", port))
            time.sleep(0.01)
        time.sleep(0.2)
        engine.stop()
        tx.close()
        yaw = [value(x) for x in sent if x[0] == 0xE0]
        pitch = [value(x) for x in sent if x[0] == 0xE1]
        roll = [value(x) for x in sent if x[0] == 0xE2]
        self.assertEqual(yaw, [m.angle_to_14bit(9.0 * i, -180, 180) for i in range(10)])  # no smoothing
        self.assertEqual(pitch, [m.angle_to_14bit(-10.0, -90, 90)])  # inverted, sent once (unchanged)
        self.assertEqual(roll, [m.angle_to_14bit(-5.0, -90, 90)])
        self.assertEqual(engine.addresses, ["/Virtuoso/quat", "/ypr"])


if __name__ == "__main__":
    unittest.main()
