import hashlib
import struct
import sys
import tempfile
import unittest
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from clock_host_evidence import collect_evidence


def fixture():
    """构造可核对的 PE 数据，不加载或执行其中的代码。"""
    data = bytearray(0xA00)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x8664, 2, 0x12345678, 0, 0, 240, 0)
    opt = 0x98
    struct.pack_into("<H", data, opt, 0x20B)
    struct.pack_into("<Q", data, opt + 24, 0x180000000)
    struct.pack_into("<I", data, opt + 56, 0x3000)
    struct.pack_into("<II", data, opt + 112 + 8, 0x2000, 40)
    struct.pack_into("<II", data, opt + 112 + 6 * 8, 0x2100, 28)
    for i, (name, va, rawptr, size) in enumerate(((b".text", 0x1000, 0x400, 0x200), (b".rdata", 0x2000, 0x600, 0x400))):
        pos = opt + 240 + i * 40
        data[pos:pos + 8] = name.ljust(8, b"\0")
        struct.pack_into("<IIII", data, pos + 8, size, va, size, rawptr)
    struct.pack_into("<IIIII", data, 0x600, 0x2080, 0, 0, 0x20C0, 0x20A0)
    struct.pack_into("<Q", data, 0x680, 0x20E0)
    struct.pack_into("<Q", data, 0x6A0, 0x20E0)
    data[0x6C0:0x6CD] = b"KERNEL32.dll\0"
    data[0x6E2:0x6EF] = b"GetLocalTime\0"
    data[0x410:0x412] = b"\xff\x15"
    struct.pack_into("<i", data, 0x412, 0x20A0 - 0x1016)
    rsds = b"RSDS" + uuid.UUID("B3CC4C19-DFDC-4131-BBF8-F213369DA32F").bytes_le + struct.pack("<I", 1) + b"SystemTray.pdb\0"
    struct.pack_into("<IIII", data, 0x700 + 12, 2, len(rsds), 0x2140, 0x740)
    data[0x740:0x740 + len(rsds)] = rsds
    return bytes(data)


class EvidenceTests(unittest.TestCase):
    def test_records_identity_import_call_and_pdb_without_changing_file(self):
        raw = fixture()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "SystemTray.dll"
            path.write_bytes(raw)
            result = collect_evidence(path)
            self.assertEqual(path.read_bytes(), raw)
        self.assertIn("sha256", result)
        self.assertEqual(result["sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(result["pe"]["timestamp"], 0x12345678)
        self.assertEqual(result["pe"]["size_of_image"], 0x3000)
        self.assertEqual(result["imports"][0]["iat_rva"], 0x20A0)
        self.assertEqual(result["call_sites"]["GetLocalTime"][0]["return_rva"], 0x1016)
        self.assertEqual(result["pdb"]["guid"], "B3CC4C19DFDC4131BBF8F213369DA32F")
        self.assertIn("B3CC4C19DFDC4131BBF8F213369DA32F1", result["pdb"]["symbol_url"])

    def test_rejects_non_pe_input(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.dll"
            path.write_bytes(b"not a PE file")
            with self.assertRaises(ValueError):
                collect_evidence(path)

    def test_records_a_valid_image_with_no_imports(self):
        raw = bytearray(fixture())
        struct.pack_into("<II", raw, 0x98 + 112 + 8, 0, 0)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "NoImports.dll"
            path.write_bytes(raw)
            result = collect_evidence(path)
        self.assertEqual(result["imports"], [])
        self.assertEqual(result["call_sites"]["GetLocalTime"], [])


if __name__ == "__main__":
    unittest.main()
