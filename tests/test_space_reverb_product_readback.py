import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("product_reader", ROOT / "tools/space_reverb_product_readback.py")
reader = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(reader)


def workload(version, count, space_start, higher=False):
    """Synthetic reader geometry only; never native or serialized-codec evidence."""
    values = [0.0] * count
    values[758] = int(higher)
    for layer in range(4):
        values[12 + 4 * layer] = 1
        values[13 + 4 * layer] = layer % 2
        for route in range(16):
            values[252 + 80 * layer + 5 * route] = 1
            values[252 + 80 * layer + 5 * route + 4] = .55
    for slot in range(8):
        for band in range(6):
            values[space_start + 96 * slot + 12 + 6 * band] = 1
            values[space_start + 96 * slot + 48 + 8 * band] = 1
    body = bytearray(struct.pack("<4sIII", b"ARSI", version, count, 0))
    for index, value in enumerate(values):
        body += struct.pack("<If", index, value)
    layout = bytearray(78)
    layout[:9] = bytes([1, 1, 1, 1, 0, 1, 2, 3, 4])
    layout[9:33] = bytes([1, 16, 4] * 8)
    layout[73:78] = bytes([0, 0, 0, 0, 8])
    body += layout
    return bytes(body) + hashlib.sha256(body).digest()


def changed(data, offset, replacement):
    body = bytearray(data[:-32])
    body[offset:offset + len(replacement)] = replacement
    return bytes(body) + hashlib.sha256(body).digest()


class ProductWorkloadGeometryTests(unittest.TestCase):
    def test_all_realtime_gate_rejects_each_remaining_miss(self):
        clean = dict(cases=24, heap_audit=False, all_nominal_targets_met=True,
                     processing_deadline_misses=0, release_completion_deadline_misses=0)
        reader.require_all_realtime(clean)
        for field, value in (("cases", 23), ("heap_audit", True),
                             ("all_nominal_targets_met", False),
                             ("processing_deadline_misses", 1),
                             ("release_completion_deadline_misses", 1)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                reader.require_all_realtime(dict(clean, **{field: value}))

    def test_explicit_historical_and_current_geometry(self):
        for version, count, start in ((21, 2831, 2063), (23, 2983, 2215), (24, 3303, 2535), (25, 3303, 2535), (26, 3503, 2735)):
            for higher in (False, True):
                with self.subTest(version=version, higher=higher):
                    data = workload(version, count, start, higher)
                    self.assertEqual(reader.validate_workload_state(data, higher), hashlib.sha256(data).hexdigest())

    def test_wrong_version_count_pair_or_header_rejected(self):
        original = workload(23, 2983, 2215)
        for header in ((b"ARSI", 21, 2983, 0), (b"ARSI", 23, 2831, 0),
                       (b"ARSI", 24, 2983, 0), (b"ARSI", 23, 3303, 0),
                       (b"ARSI", 26, 3303, 0), (b"ARSI", 23, 2983, 1), (b"WRNG", 23, 2983, 0)):
            with self.subTest(header=header), self.assertRaises(ValueError):
                reader.validate_workload_state(changed(original, 0, struct.pack("<4sIII", *header)), False)

    def test_space_offset_is_tied_to_geometry(self):
        with self.assertRaisesRegex(ValueError, "decay/post band"):
            reader.validate_workload_state(workload(23, 2983, 2063), False)
        with self.assertRaisesRegex(ValueError, "decay/post band"):
            reader.validate_workload_state(workload(24, 3303, 2215), False)

    def test_original_workload_rejections_survive_all_geometries(self):
        for version, count, start in ((21, 2831, 2063), (23, 2983, 2215), (24, 3303, 2535), (25, 3303, 2535), (26, 3503, 2735)):
            original = workload(version, count, start)
            for index, value in ((758, 1), (757, 1), (12, 0), (13, 1), (252, 0), (256, .5),
                                 (572, 1), (start + 12, 0), (start + 48, 0), (200, float("nan"))):
                with self.subTest(version=version, index=index), self.assertRaises(ValueError):
                    reader.validate_workload_state(changed(original, 16 + index * 8 + 4, struct.pack("<f", value)), False)
            with self.subTest(version=version, topology=True), self.assertRaisesRegex(ValueError, "topology"):
                reader.validate_workload_state(changed(original, 16 + count * 8 + 9, bytes([0])), False)

    def test_digest_and_truncation_remain_rejected(self):
        original = workload(23, 2983, 2215)
        bad = bytearray(original)
        bad[-1] ^= 1
        for data in (bytes(bad), original[:12], original[:100]):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                reader.validate_workload_state(data, False)

    def test_historical_native_packets_keep_actual_outcomes(self):
        for label, expected in (("release", "NOT_PASS"), ("debug-heap", "PASS_ACTUAL_PRODUCT_HEAP_AUDIT")):
            archive = ROOT / "docs/verification/space-reverb-review" / f"capacity-{label}-data.zip"
            with self.subTest(packet=label), tempfile.TemporaryDirectory() as temporary:
                with zipfile.ZipFile(archive) as packet:
                    for name in packet.namelist():
                        if "/" not in name:
                            (Path(temporary) / name).write_bytes(packet.read(name))
                result = reader.readback(temporary)
                self.assertEqual(result["status"], expected)
                self.assertEqual(result["cases"], 24)


if __name__ == "__main__":
    unittest.main()
