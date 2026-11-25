#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Idempotently ensure Tesla P4 (10de:1bb3, SUBSYS_11D810DE) profiles exist in R580 vgpuConfig.xml."""

import argparse
import pathlib
import sys
import xml.etree.ElementTree as ET

TESLA_P4_VGPU_TYPES_XML = [
    """  <vgpuType id="71" name="GRID P4-1B" class="NVS">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x40000000</profileSize>
    <framebuffer>0x38000000</framebuffer>
    <fbReservation>0x8000000</fbReservation>
    <numHeads>4</numHeads>
    <display width="5120" height="2880" />
    <maxPixels>16384000</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x2d</frlConfig>
    <cudaEnabled>0</cudaEnabled>
    <eccSupported>0</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>VPC;QVDWS;VWS;VWSExt</license>
  </vgpuType>""",
    """  <vgpuType id="63" name="GRID P4-1Q" class="Quadro">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x40000000</profileSize>
    <framebuffer>0x38000000</framebuffer>
    <fbReservation>0x8000000</fbReservation>
    <numHeads>4</numHeads>
    <display width="5120" height="2880" />
    <maxPixels>18432000</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x3c</frlConfig>
    <cudaEnabled>1</cudaEnabled>
    <eccSupported>1</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>QVDWS;VWS;VWSExt</license>
  </vgpuType>""",
    """  <vgpuType id="64" name="GRID P4-2Q" class="Quadro">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x80000000</profileSize>
    <framebuffer>0x74000000</framebuffer>
    <fbReservation>0xC000000</fbReservation>
    <numHeads>4</numHeads>
    <display width="7680" height="4320" />
    <maxPixels>36864000</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x3c</frlConfig>
    <cudaEnabled>1</cudaEnabled>
    <eccSupported>1</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>QVDWS;VWS;VWSExt</license>
  </vgpuType>""",
    """  <vgpuType id="65" name="GRID P4-4Q" class="Quadro">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x100000000</profileSize>
    <framebuffer>0xEC000000</framebuffer>
    <fbReservation>0x14000000</fbReservation>
    <numHeads>4</numHeads>
    <display width="7680" height="4320" />
    <maxPixels>58982400</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x3c</frlConfig>
    <cudaEnabled>1</cudaEnabled>
    <eccSupported>1</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>QVDWS;VWS;VWSExt</license>
  </vgpuType>""",
    """  <vgpuType id="66" name="GRID P4-8Q" class="Quadro">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x200000000</profileSize>
    <framebuffer>0x1DC000000</framebuffer>
    <fbReservation>0x24000000</fbReservation>
    <numHeads>4</numHeads>
    <display width="7680" height="4320" />
    <maxPixels>66355200</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x3c</frlConfig>
    <cudaEnabled>1</cudaEnabled>
    <eccSupported>1</eccSupported>
    <multiVgpuSupported>1</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>QVDWS;VWS;VWSExt</license>
  </vgpuType>""",
    """  <vgpuType id="67" name="GRID P4-1A" class="NVS">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x40000000</profileSize>
    <framebuffer>0x38000000</framebuffer>
    <fbReservation>0x8000000</fbReservation>
    <numHeads>1</numHeads>
    <display width="1280" height="1024" />
    <maxPixels>1310720</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x3c</frlConfig>
    <cudaEnabled>0</cudaEnabled>
    <eccSupported>0</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>VApps</license>
  </vgpuType>""",
    """  <vgpuType id="68" name="GRID P4-2A" class="NVS">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x80000000</profileSize>
    <framebuffer>0x74000000</framebuffer>
    <fbReservation>0xC000000</fbReservation>
    <numHeads>1</numHeads>
    <display width="1280" height="1024" />
    <maxPixels>1310720</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x3c</frlConfig>
    <cudaEnabled>0</cudaEnabled>
    <eccSupported>0</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>VApps</license>
  </vgpuType>""",
    """  <vgpuType id="69" name="GRID P4-4A" class="NVS">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x100000000</profileSize>
    <framebuffer>0xEC000000</framebuffer>
    <fbReservation>0x14000000</fbReservation>
    <numHeads>1</numHeads>
    <display width="1280" height="1024" />
    <maxPixels>1310720</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x3c</frlConfig>
    <cudaEnabled>0</cudaEnabled>
    <eccSupported>0</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>VApps</license>
  </vgpuType>""",
    """  <vgpuType id="70" name="GRID P4-8A" class="NVS">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x200000000</profileSize>
    <framebuffer>0x1DC000000</framebuffer>
    <fbReservation>0x24000000</fbReservation>
    <numHeads>1</numHeads>
    <display width="1280" height="1024" />
    <maxPixels>1310720</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x3c</frlConfig>
    <cudaEnabled>0</cudaEnabled>
    <eccSupported>0</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>VApps</license>
  </vgpuType>""",
    """  <vgpuType id="157" name="GRID P4-2B" class="NVS">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x80000000</profileSize>
    <framebuffer>0x74000000</framebuffer>
    <fbReservation>0xC000000</fbReservation>
    <numHeads>4</numHeads>
    <display width="5120" height="2880" />
    <maxPixels>18432000</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x2d</frlConfig>
    <cudaEnabled>0</cudaEnabled>
    <eccSupported>0</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>VPC;QVDWS;VWS;VWSExt</license>
  </vgpuType>""",
    """  <vgpuType id="214" name="GRID P4-2B4" class="NVS">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x80000000</profileSize>
    <framebuffer>0x74000000</framebuffer>
    <fbReservation>0xC000000</fbReservation>
    <numHeads>4</numHeads>
    <display width="5120" height="2880" />
    <maxPixels>18432000</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x2d</frlConfig>
    <cudaEnabled>0</cudaEnabled>
    <eccSupported>0</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>VPC;QVDWS;VWS;VWSExt</license>
  </vgpuType>""",
    """  <vgpuType id="243" name="GRID P4-1B4" class="NVS">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x11D8" />
    <profileSize>0x40000000</profileSize>
    <framebuffer>0x38000000</framebuffer>
    <fbReservation>0x8000000</fbReservation>
    <numHeads>4</numHeads>
    <display width="5120" height="2880" />
    <maxPixels>16384000</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x2d</frlConfig>
    <cudaEnabled>0</cudaEnabled>
    <eccSupported>0</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <license>VPC;QVDWS;VWS;VWSExt</license>
  </vgpuType>""",
]

TESLA_P4_PGPU_XML = """  <pgpu fractionalMultiVgpu="0">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x0" />
    <supportedVgpu vgpuId="71">
      <maxVgpus>8</maxVgpus>
      <digest type="signature">0c6cf47b1c43129c33c76bb377003f55cc2bc8c40df91955dc8cdd1398001e955006343eef36d7b55b4c2bd2480790f318111afbeaaf9741e048d65e1b2c2af2c5b71ba680ad17060154cd0b339e13a10fc851266fcb4ec92a359cd8e9996eab750aae9d8c4a8ceb7dc234217c3839254b2634f3d6b8677f4817c9c261d71704</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="63">
      <maxVgpus>8</maxVgpus>
      <digest type="signature">2fb6140e01102aa17cb3c77dff092b48e351a412d4de23f10e8f7db209715727c9849116e71076d80d4dabfcee038228546812767025f0cc75e2ca45ae9b6cf00c036d93cda738de7b0f32ace9941a22f4ac0b0d745ca6a957d51b1c413aad9afb16f62bdc246fdbd529d01a444b1cc445a814ccc4fd1d28d9fb7ab0af9d651f</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="64">
      <maxVgpus>4</maxVgpus>
      <digest type="signature">2a8e7193d6dd945a2415c58b22e5685852e0b935c88ecce23a4557f03e20ad482919bca8a79da92c95c2d464ceb4140d4df34590a4d21e063c41681844027dad871cd278f80d9c8e36ebf0e06399c89eecf16688d2953e0a1fa8a26cfa8eaaad2acf71ab050c0d1bb3f4caa63b6436976d40d132a6c712227c1cdae724e843f1</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="65">
      <maxVgpus>2</maxVgpus>
      <digest type="signature">a2fbd94a8f5379159b6312b6d88729993d7c3cae86f3b7a873f9a7fc1b01b751966c1c6eca5b45931f32511ea89f4f143b1cbbb9902f6ec8bd1d16ca4c96dc8f469812fcc6d5a65a3a15788afe83eda814a6c5170f66c1325fc68dca467f9a2a4e28b384fe2893924c2d737133bf69c50551666b95bc32824133f33bb6c11a1b</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="66">
      <maxVgpus>1</maxVgpus>
      <digest type="signature">cc9b4c1ddbcc647f5f378192bc1532dbfaf8ce82767fe86412b72ef9e8e4f980457e07ce74d2bf2470f4519b2abbfcf480d057155b2aea62658b7cac8dad02ee00ec66baa077b417f4de19c8010214a741a11e8039f7dd97b67d0596832fca6400dd15e19603aa2d4af88e760cb42a6fd245bb5d140ac9a404e98ec87e778d48</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="67">
      <maxVgpus>8</maxVgpus>
      <digest type="signature">b8d334b2b68db2acd61ff033a4e6b5b501eb55de96f30a7f0d1ece52dafa3996aa35686223707a5c8820603d3a6a12e3d74c245a60953529ae8a2cee752e01d5aeecf9c7ed13f5dd32da47768e0855fd09c8b66cec33eb35d743dbbf602ef37077aad2dd5eb82b4b212b3d28d8ac55eee9395da13290dd5487c28f7c4de86d44</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="68">
      <maxVgpus>4</maxVgpus>
      <digest type="signature">2871818b2d4da0f8a4ed61772344414a9257d4a4c1f0a3b1824f49d4c6f789070acc24263599d44b17ed1472da94fd8b4e4d95308e4c387eac8caeb95a93cc40c9a47655d7d8cd0b2612be03920e7410a266e642aee39183ab4c2c5ceb8bafb9c23e52e2bfb69f121663bad396f750c4a9967aa50c6d51a8131784fe30a3f864</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="69">
      <maxVgpus>2</maxVgpus>
      <digest type="signature">d2c323be4b9d7ce728bb7646a2cd20c253d8eca8dc885c9998d48451e98e0629460d37f559012809b12ffa8b8e38e8bacc0874474c4ec9d7a4dd596ae7329ace4baee2e8d821f620233b79e881925042cde030c70c8782694312e2000d682693edb7de3c7e54e389011e6ade058c7ba420b34f83ce5a18412d21ad6b1ed8d0bc</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="70">
      <maxVgpus>1</maxVgpus>
      <digest type="signature">1d3e6dd34be62594d45cc54eec4b65ecc367c668a964c7b2a539383c37b83f3471d6473edb18b368502475a27b3abd38e1ac95f57eff65f7647a5088f72ad3a21e66fa373310b89c5e5c948b3913bf76c99f6e74795e4398f0c46df59c265f6f69bc87ccd104d44147a6e42b5e4f7d29770d9f1565fb7ce97b1a46224f253da7</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="157">
      <maxVgpus>4</maxVgpus>
      <digest type="signature">a50b94bedf7a19c06af7570a03b244b6af77270eb506c3b91dd7d31f1d2cf52c34119f484e920aabfd0d41404ae671c7e83344c045671b912721ffa9b21da8af9b19fceb2bc6313504cc707ad25da3268a55f3a3212dba75a238392bc3d44ac931662a2601245e64bfe2254db3060d150a041d90e09d8e98a0ac8c8d9606d1cb</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="214">
      <maxVgpus>4</maxVgpus>
      <digest type="signature">a9c38dffe9ba11da0fc2fe2a579e837564a788d1ff0c39789b834161fe675e1676792ee5dbe37134ab52a1ab4508d7ed3770af6e63b51400cb6c15fd1e4e2bf068c1a4014275abefd4363270a6ab14a744acca47eb030be22287eb4899bf821429d685d6644513b8d10351f7a8ba398bcbcf869e6617d0ad7348e77d3339cdfa</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="243">
      <maxVgpus>8</maxVgpus>
      <digest type="signature">715724d4a7467b2fbc386c30c7765df94036f242caf9ef5594622abed6fb8eb095aa6cb0c98ede59f62d3ff9587bbf5cffc1bc2a8d8cdedc6e547428613012d64f192c25fa961e400c7d51f20b8be070bbc1aa97fc75d052b5262443836b3520c23fef0e5c5c2678057c8d86e7a2d0931eb98b474d93ea739862ce3aec6c792e</digest>
    </supportedVgpu>
  </pgpu>"""


def patch_vgpu_config(xml_path: pathlib.Path, dry_run: bool = False) -> tuple[bool, str]:
    """Ensure all 12 Tesla P4 profiles (SUBSYS_11D8) and the 0x1BB3 pgpu entry exist."""
    raw = xml_path.read_text(encoding="utf-8")
    root = ET.fromstring(raw)

    existing_ids = {
        elem.attrib.get("id", "")
        for elem in root.findall(".//vgpuType")
    }
    has_p4_pgpu = any(
        (dev.attrib.get("deviceId", "").lower() == "0x1bb3")
        for pgpu in root.findall(".//pgpu")
        for dev in pgpu.findall("./devId")
    )

    insertions: list[str] = []
    added_types = 0
    for snippet in TESLA_P4_VGPU_TYPES_XML:
        vid = ET.fromstring(snippet).attrib["id"]
        if vid not in existing_ids:
            insertions.append(snippet)
            added_types += 1

    if not has_p4_pgpu:
        insertions.append(TESLA_P4_PGPU_XML)

    # Check if existing P4 vgpuTypes need subsystemId="0x11D8"
    updated = raw
    norm_count = 0
    for vt in root.findall(".//vgpuType"):
        if vt.attrib.get("name", "").startswith("GRID P4-"):
            dev = vt.find("./devId")
            if dev is not None and dev.attrib.get("subsystemId", "").lower() != "0x11d8":
                old_sub = dev.attrib.get("subsystemId", "")
                if old_sub:
                    target_name = vt.attrib.get("name", "")
                    # Replace subsystemId only inside that named vgpuType block
                    import re
                    pat = re.compile(
                        rf'(<vgpuType\b[^>]*name="{re.escape(target_name)}"[^>]*>\s*<devId\b[^>]*subsystemId="){re.escape(old_sub)}(")',
                        re.M,
                    )
                    updated_next, n = pat.subn(r"\g<1>0x11D8\g<2>", updated, count=1)
                    if n:
                        updated = updated_next
                        norm_count += n

    if not insertions and norm_count == 0:
        return False, "No patch needed: all 12 Tesla P4 profiles (0x1BB3 / SUBSYS_11D8) already present"

    if insertions:
        closing = "</vgpuconfig>"
        idx = updated.rfind(closing)
        if idx == -1:
            raise ValueError(f"Missing closing {closing} in {xml_path}")
        block = "\n".join(insertions) + "\n"
        updated = updated[:idx] + block + updated[idx:]

    if not dry_run:
        backup = xml_path.with_name(xml_path.name + ".orig")
        if not backup.exists():
            backup.write_text(raw, encoding="utf-8")
        xml_path.write_text(updated, encoding="utf-8")

    return True, (
        f"Patched {xml_path}: added {added_types} vgpuType entries, "
        f"added_pgpu={not has_p4_pgpu}, normalized_subsys={norm_count}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Inject/normalize Tesla P4 (10de:1bb3, SUBSYS_11D8) profiles in R580 vgpuConfig.xml"
    )
    parser.add_argument(
        "--xml",
        type=pathlib.Path,
        default=pathlib.Path("/usr/share/nvidia/vgpu/vgpuConfig.xml"),
        help="Path to vgpuConfig.xml (default: /usr/share/nvidia/vgpu/vgpuConfig.xml)",
    )
    parser.add_argument("--dry-run", "--check", action="store_true", help="Inspect only without writing")
    args = parser.parse_args()

    if not args.xml.is_file():
        print(f"ERROR: Missing vgpuConfig.xml at {args.xml}", file=sys.stderr)
        return 1

    _, msg = patch_vgpu_config(args.xml, dry_run=args.dry_run)
    print(msg)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
