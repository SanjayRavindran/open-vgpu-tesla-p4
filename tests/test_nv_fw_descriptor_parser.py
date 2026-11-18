import struct
import unittest
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import nv_fw_descriptor_parser as dp


class TestNvFwDescriptorParser(unittest.TestCase):
    def test_lsf_xor_footer_decodes_real_and_synthetic_fids(self):
        for fid, expected_name in [(0, "PMU"), (2, "FECS"), (3, "GPCCS"), (4, "NVENC"), (7, "SEC2")]:
            buf = bytearray(192)
            struct.pack_into(
                "<4I",
                buf,
                176,
                dp.LSF_XOR_FOOTER[0] ^ fid,
                dp.LSF_XOR_FOOTER[1],
                dp.LSF_XOR_FOOTER[2],
                dp.LSF_XOR_FOOTER[3],
            )
            parsed = dp.parse_lsf_sig_wrapper(bytes(buf))
            self.assertEqual(parsed.falcon_id, fid)
            self.assertEqual(parsed.falcon_name, expected_name)

    def test_prom_jffs_synthetic_reassembly(self):
        prom = bytearray(0x400)
        prom[0x000:0x00C] = struct.pack("<4sII", b"JFFS", 2, 0x300)
        # Write 2 chunks for OBD object across 128B blocks
        part1 = b"Tesla P4"
        part2 = b"-0200"
        struct.pack_into("<4sIII", prom, 0x080, b"\x02OBD", (len(part1) << 16) | 0, 1, 0)
        prom[0x090:0x090 + len(part1)] = part1
        struct.pack_into("<4sIII", prom, 0x100, b"\x02OBD", (len(part2) << 16) | len(part1), 1, 0)
        prom[0x110:0x110 + len(part2)] = part2
        res = dp.parse_prom_inforom(bytes(prom))
        self.assertEqual(res["version"], 2)
        self.assertEqual(res["records"][0]["name"], "OBD")
        self.assertIn("Tesla P4-0200", res["records"][0]["ascii_strings"])

    def test_real_extracted_gp104_vgpu_535_bundle_if_present(self):
        blob_root = Path(__file__).resolve().parents[1] / "firmware-blobs" / "gp104-vgpu-535"
        if not (blob_root / "pmu" / "gp104_pmu_ls_sig.bin").exists():
            self.skipTest("Isolated GP104 vGPU 535 firmware-blobs bundle not extracted yet")
        for rel, expected_fid in [
            ("pmu/gp104_pmu_ls_sig.bin", 0),
            ("gr/gp104_fecs_ls_sig.bin", 2),
            ("gr/gp104_gpccs_ls_sig.bin", 3),
            ("nvenc/gp104_nvenc_ls_sig.bin", 4),
            ("sec2/gp104_sec2_rtos/entry_02_id_0x00.bin", 7),
        ]:
            sig = dp.parse_lsf_sig_wrapper((blob_root / rel).read_bytes())
            self.assertEqual(sig.falcon_id, expected_fid)
        bl = dp.parse_flcn_bl_desc((blob_root / "pmu" / "gp104_pmu_bl" / "entry_01_id_0x00.bin").read_bytes())
        self.assertGreater(bl.code_dma_base_or_size, 0)
        rtos = dp.parse_rtos_desc((blob_root / "sec2" / "gp104_sec2_rtos" / "entry_01_id_0x00.bin").read_bytes())
        self.assertEqual(rtos.app_entry, 0xFD00)
        netlist = (blob_root / "gr" / "gp104_gr_netlist.bin").read_bytes()
        magic, cnt = struct.unpack_from("<II", netlist, 0)
        self.assertEqual((magic, cnt), (0, 32))


if __name__ == "__main__":
    unittest.main()
