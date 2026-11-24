#!/usr/bin/env python3
"""
decode_nvenc_trace.py - Phase 0 Ground-Truth GP104 NVENC RM & Pushbuffer Decoder

Decodes captures produced by rm_nvenc_trace.c (NV_ESC_RM_* ioctls, mmap snapshots,
NVC06F GPFIFO ring entries, pushbuffer method streams, and 0xc2b70006 pic_setup structs)
and computes byte/dword differential tables across differential test cases.
"""

import argparse
import glob
import json
import os
import struct
import sys


RM_CLASS_NAMES = {
    0x0002: "NV01_CONTEXT_DMA",
    0x003e: "NV01_MEMORY_SYSTEM",
    0x0040: "NV01_MEMORY_LOCAL_USER",
    0x0041: "NV01_ROOT_USER",
    0x0070: "NV01_MEMORY_VIRTUAL",
    0x0079: "NV01_EVENT_OS_EVENT",
    0x0080: "NV01_DEVICE_0",
    0x2080: "NV20_SUBDEVICE_0",
    0x50a0: "NV50_MEMORY_VIRTUAL",
    0x90f1: "FERMI_VASPACE_A",
    0xa06c: "KEPLER_CHANNEL_GROUP_A",
    0xa0bc: "NVENC_SW_SESSION",
    0xc06f: "PASCAL_CHANNEL_GPFIFO_A",
    0xc0b5: "PASCAL_DMA_COPY_A",
    0xc0b7: "NVC0B7_VIDEO_ENCODER",
    0xc1b0: "NVC1B0_VIDEO_DECODER",
    0xc1b5: "NVC1B5_DMA_COPY",
    0xc1b7: "NVC1B7_VIDEO_ENCODER",
    0xc1c0: "PASCAL_COMPUTE_A",
    0xc2b7: "NVC2B7_VIDEO_ENCODER",
}

NVC2B7_METHODS = {
    0x0000: "NVC06F_SET_OBJECT",
    0x0080: "NVC06F_YIELD_OR_SET_SEMA_CTX",
    0x0081: "NVC06F_UNK_0204",
    0x0090: "NVC06F_SEMAPHOREA",
    0x0091: "NVC06F_SEMAPHOREB",
    0x0092: "NVC06F_SEMAPHOREC",
    0x0093: "NVC06F_SEMAPHORED",
    0x0200: "NV_SEMA_OR_NOP_0200",
    0x0204: "NV_APP_ID_OR_CTX_0204",
    0x0240: "NV_SEMAPHORE_A_HI",
    0x0244: "NV_SEMAPHORE_B_LO",
    0x0248: "NV_SEMAPHORE_C_PAYLOAD",
    0x0300: "NVC2B7_EXECUTE",
    0x0304: "NVC2B7_SEMAPHORE_D_LAUNCH",
    **{0x0400 + i * 4: f"NVC2B7_SET_REF_PIC_LUMA_{i}" for i in range(16)},
    **{0x0440 + i * 4: f"NVC2B7_SET_REF_PIC_CHROMA_{i}" for i in range(16)},
    0x0700: "NVC2B7_SET_APPLICATION_ID",
    0x0704: "NVC2B7_SET_CONTROL_PARAMS",
    0x070c: "NVC2B7_SET_SLICE_OR_IO_OFFSET",
    0x0710: "NVC2B7_SET_DRV_PIC_SETUP_OFFSET",
    0x0718: "NVC2B7_SET_STATUS_BUF_OFFSET",
    0x071c: "NVC2B7_SET_OUT_BITSTREAM_OFFSET",
    0x0720: "NVC2B7_SET_SCRATCH_VIDMEM_OFFSET",
    0x0724: "NVC2B7_SET_QM_OR_RC_OFFSET",
    0x0730: "NVC2B7_SET_RECON_PIC_LUMA_OFFSET",
    0x0734: "NVC2B7_SET_CUR_PIC_LUMA_OFFSET",
    0x0738: "NVC2B7_SET_COLOC_MV_BUF_0_OFFSET",
    0x073c: "NVC2B7_SET_COLOC_MV_BUF_1_OFFSET",
    0x0740: "NVC2B7_SET_CUR_PIC_CHROMA_U_OFFSET",
    0x0744: "NVC2B7_SET_CUR_PIC_CHROMA_V_OFFSET",
    0x074c: "NVC2B7_SET_RECON_PIC_CHROMA_OFFSET",
}


def decode_pb_words(words, base_gpu_va):
    """Decode NVC06F pushbuffer dwords into a list of (gpu_va, subch, byte_off, val, mode)."""
    ops = []
    i = 0
    n = len(words)
    while i < n:
        w = words[i]
        if w == 0:
            i += 1
            continue
        sec_op = (w >> 29) & 0x7
        count = (w >> 16) & 0x1fff
        subch = (w >> 13) & 0x7
        m_dword = w & 0xfff
        m_byte = m_dword * 4
        pb_va = base_gpu_va + i * 4
        if sec_op == 1:  # INC_METHOD
            for k in range(count):
                if i + 1 + k < n:
                    ops.append((pb_va, subch, m_byte + k * 4, words[i + 1 + k], "INC"))
            i += 1 + count
        elif sec_op == 3:  # NON_INC_METHOD
            for k in range(count):
                if i + 1 + k < n:
                    ops.append((pb_va, subch, m_byte, words[i + 1 + k], "NON_INC"))
            i += 1 + count
        elif sec_op == 5:  # ONE_INC
            for k in range(count):
                if i + 1 + k < n:
                    ops.append((pb_va, subch, m_byte if k == 0 else m_byte + 4, words[i + 1 + k], "ONE_INC"))
            i += 1 + count
        elif sec_op == 4:  # IMMD_DATA
            ops.append((pb_va, subch, m_byte, count, "IMMD"))
            i += 1
        elif sec_op == 0:  # GRP0
            tert = (w >> 16) & 0x3
            if tert == 0:
                cnt_old = (w >> 18) & 0x7ff
                for k in range(cnt_old):
                    if i + 1 + k < n:
                        ops.append((pb_va, subch, m_byte + k * 4, words[i + 1 + k], "OLD_INC"))
                i += 1 + cnt_old
            else:
                ops.append((pb_va, subch, m_byte, w, f"GRP0_T{tert}"))
                i += 1
        else:
            ops.append((pb_va, subch, m_byte, w, f"OP{sec_op}"))
            i += 1
    return ops


def load_case(case_dir):
    trace_path = os.path.join(case_dir, "trace.jsonl")
    events = [json.loads(line) for line in open(trace_path, "r", encoding="utf-8")]

    nvenc_clients = set()
    channels = {}
    engines = []
    ch_eng_class = {}
    dma_map = []  # list of (gpu_va, length, hMemory, hClass, hClient)
    alloc_mem = {}

    for e in events:
        t = e["type"]
        if t == "rm_alloc64":
            cls = int(e["hClass"], 16)
            if cls in (0xc2b7, 0xc0b7, 0xc1b7, 0xc0b5):
                ch_eng_class[e["hParent"]] = cls
            if cls == 0xc2b7 or cls == 0xc0b7 or cls == 0xc1b7:
                nvenc_clients.add(e["hRoot"])
                engines.append({
                    "hClient": e["hRoot"],
                    "hChannel": e["hParent"],
                    "hEngine": e["hNew"],
                    "hClass": e["hClass"],
                    "className": RM_CLASS_NAMES.get(cls, e["hClass"]),
                })
            if cls == 0xc06f:
                channels[e["hNew"]] = {
                    "hClient": e["hRoot"],
                    "hParent": e["hParent"],
                    "hChannel": e["hNew"],
                    "hClass": e["hClass"],
                    "paramsHex": e.get("params", ""),
                }
        elif t == "rm_alloc_mem":
            alloc_mem[e["hNew"]] = {
                "hClient": e["hRoot"],
                "hClass": e["hClass"],
                "flags": e["flags"],
                "limit": int(e["limit"], 16),
            }
        elif t == "rm_map_dma" and e["status"] == "0x0":
            va = int(e["gpu_va"], 16)
            ln = int(e["len"], 16)
            dma_map.append((va, ln, e["hMemory"], e["hClass"], e["hClient"]))

    # Map GPU VA lookup helper
    def lookup_va(va):
        for b, ln, hmem, hcls, hc in dma_map:
            if b <= va < b + ln:
                return f"{hmem} ({RM_CLASS_NAMES.get(int(hcls,16), hcls)}) +0x{va-b:x} [len=0x{ln:x}]"
        return None

    # Find latest snapshots per hMemory
    latest_snaps = {}
    all_snaps_by_mem = {}
    for e in events:
        if e["type"] == "snap_map":
            hm = e["hMemory"]
            latest_snaps[hm] = e
            all_snaps_by_mem.setdefault(hm, []).append(e)

    # Decode pushbuffer channels belonging to NVENC clients (matched via gpFifoOffset from paramsHex)
    decoded_channels = []
    for ch_id, ch in channels.items():
        if ch["hClient"] not in nvenc_clients:
            continue
        phex = ch["paramsHex"]
        gp_va = None
        gp_entries = 0
        if len(phex) >= 40:
            (gp_va,) = struct.unpack("<Q", bytes.fromhex(phex[16:32]))
            (gp_entries,) = struct.unpack("<I", bytes.fromhex(phex[32:40]))
        pb_snap = None
        if gp_va is not None:
            for b, ln, hmem, hcls, hc in dma_map:
                if hc == ch["hClient"] and b <= gp_va < b + ln and hmem in latest_snaps:
                    snap = latest_snaps[hmem]
                    fpath = os.path.join(case_dir, snap["file"])
                    data = open(fpath, "rb").read()
                    pb_snap = (b, gp_va - b, gp_entries, hmem, snap["file"], data)
                    break
        if not pb_snap:
            continue
        base_va, gp_fifo_off, gp_entries, pb_hmem, pb_file, data = pb_snap
        gpfifo = []
        pb_ops = []
        seen_ranges = set()
        max_gp = min(gp_entries or 0x100, (len(data) - gp_fifo_off) // 8)
        for idx in range(max_gp):
            off = gp_fifo_off + idx * 8
            e0, e1 = struct.unpack_from("<II", data, off)
            if e0 == 0 and e1 == 0:
                break
            va = ((e1 & 0xff) << 32) | (e0 & ~3)
            dw_cnt = (e1 >> 10) & 0x1fffff
            sync = (e1 >> 31) & 1
            pb_off = va - base_va
            gpfifo.append({
                "idx": idx,
                "gpu_va": f"0x{va:x}",
                "dwords": dw_cnt,
                "sync": sync,
                "pb_off": f"0x{pb_off:x}",
            })
            if 0 <= pb_off and pb_off + dw_cnt * 4 <= len(data) and (va, dw_cnt) not in seen_ranges:
                seen_ranges.add((va, dw_cnt))
                words = struct.unpack_from(f"<{dw_cnt}I", data, pb_off)
                eng_cls = ch_eng_class.get(ch_id)
                is_nvenc = eng_cls in (0xc2b7, 0xc1b7, 0xc0b7)
                for pva, subch, mbyte, val, mode in decode_pb_words(words, va):
                    target = None
                    if is_nvenc:
                        mname = NVC2B7_METHODS.get(mbyte, f"METH_0x{mbyte:04x}")
                        if mbyte in (
                            0x070c, 0x0710, 0x0718, 0x071c, 0x0720, 0x0724,
                            0x0730, 0x0734, 0x0738, 0x073c, 0x0740, 0x0744, 0x074c,
                        ) or (0x0400 <= mbyte <= 0x047c):
                            # NVENC uses >> 8 shifted 40-bit addresses in 32-bit methods!
                            target = lookup_va(val << 8)
                    elif eng_cls == 0xc0b5:
                        mname = {
                            0x0000: "NVC06F_SET_OBJECT",
                            0x0300: "NVC0B5_LAUNCH_DMA",
                            0x0586: "NVC0B5_SET_REMAP_CONST_A",
                        }.get(mbyte, f"NVC0B5_0x{mbyte:04x}")
                    else:
                        mname = f"METH_0x{mbyte:04x}"
                    pb_ops.append({
                        "gp_idx": idx,
                        "subch": subch,
                        "method": f"0x{mbyte:04x}",
                        "name": mname,
                        "val": f"0x{val:08x}",
                        "mode": mode,
                        "target": target,
                    })
        decoded_channels.append({
            "hChannel": ch_id,
            "engineClass": f"0x{ch_eng_class[ch_id]:04x}" if ch_id in ch_eng_class else None,
            "gp_fifo_off": f"0x{gp_fifo_off:x}",
            "pb_hMemory": pb_hmem,
            "pb_gpu_va": f"0x{base_va:x}",
            "gpfifo": gpfifo,
            "methods": pb_ops,
        })

    # Collect pic_setup snapshots (magic 0xc2b70006 at offset 0)
    pic_setups = []
    for hm, snaps in sorted(all_snaps_by_mem.items()):
        # Pick last snap for each hMemory that has 0xc2b70006
        last = snaps[-1]
        fpath = os.path.join(case_dir, last["file"])
        raw = open(fpath, "rb").read()
        if len(raw) >= 4 and struct.unpack_from("<I", raw, 0)[0] == 0xc2b70006:
            nz = {f"0x{off:05x}": f"0x{struct.unpack_from('<I', raw, off)[0]:08x}"
                  for off in range(0, len(raw), 4)
                  if struct.unpack_from("<I", raw, off)[0] != 0}
            pic_setups.append({
                "hMemory": hm,
                "gpu_va": last["gpu_va"],
                "file": last["file"],
                "raw": raw,
                "nonzero": nz,
            })

    phases = [e for e in events if e["type"] == "nvenc_phase"]
    return {
        "case_dir": os.path.basename(case_dir),
        "nvenc_clients": sorted(nvenc_clients),
        "engines": engines,
        "phases": phases,
        "dma_regions": [
            {"gpu_va": f"0x{b:x}", "len": f"0x{ln:x}", "hMemory": hm, "hClass": cls}
            for b, ln, hm, cls, hc in dma_map if hc in nvenc_clients
        ],
        "channels": decoded_channels,
        "pic_setups": pic_setups,
    }


def diff_pic_setups(label_a, raw_a, label_b, raw_b):
    diffs = []
    n = min(len(raw_a), len(raw_b))
    for off in range(0, n, 4):
        va = struct.unpack_from("<I", raw_a, off)[0]
        vb = struct.unpack_from("<I", raw_b, off)[0]
        if va != vb:
            diffs.append((off, va, vb))
    print(f"\n=== PIC_SETUP DIFF: {label_a} vs {label_b} ({len(diffs)} dwords differ) ===")
    for off, va, vb in diffs:
        print(f"  +0x{off:05x}: 0x{va:08x} -> 0x{vb:08x}  (xor=0x{va^vb:08x})")
    return diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case_dirs", nargs="+", help="Trace directories (e.g. case_A case_B ...)")
    ap.add_argument("--json-out", help="Optional path to write structured JSON summary")
    args = ap.parse_args()

    cases = {}
    for d in args.case_dirs:
        res = load_case(d)
        cases[res["case_dir"]] = res
        print(f"================ {res['case_dir']} ================")
        print("NVENC clients:", res["nvenc_clients"])
        print("NVENC engines:", res["engines"])
        print(f"DMA regions ({len(res['dma_regions'])}):")
        for r in res["dma_regions"][:30]:
            print(f"  {r['gpu_va']:>12s} len={r['len']:>8s} hMem={r['hMemory']} cls={r['hClass']}")
        for ch in res["channels"]:
            print(f"\nChannel {ch['hChannel']} (PB={ch['pb_hMemory']} @ {ch['pb_gpu_va']}), GPFIFO entries={len(ch['gpfifo'])}:")
            for g in ch["gpfifo"]:
                print(f"  GP[{g['idx']}] va={g['gpu_va']} pb_off={g['pb_off']} dwords={g['dwords']} sync={g['sync']}")
            print(f"  Decoded pushbuffer methods ({len(ch['methods'])}):")
            for m in ch["methods"]:
                tgt = f"  --> {m['target']}" if m["target"] else ""
                print(f"    GP[{m['gp_idx']}] subch={m['subch']} {m['method']} ({m['name']:36s}) = {m['val']}{tgt}")
        print(f"\npic_setup buffers found: {len(res['pic_setups'])}")
        for ps in res["pic_setups"]:
            print(f"  {ps['hMemory']} @ {ps['gpu_va']} ({len(ps['nonzero'])} nonzero dwords, w0={ps['nonzero'].get('0x00000')})")

    # Perform differential pic_setup comparisons when expected cases exist
    if "case_A" in cases and "case_B" in cases and cases["case_A"]["pic_setups"] and cases["case_B"]["pic_setups"]:
        diff_pic_setups("case_A (4:2:0 8b 256x256)", cases["case_A"]["pic_setups"][0]["raw"],
                        "case_B (4:2:0 10b 256x256)", cases["case_B"]["pic_setups"][0]["raw"])
    if "case_B" in cases and "case_C" in cases and cases["case_B"]["pic_setups"] and cases["case_C"]["pic_setups"]:
        diff_pic_setups("case_B (4:2:0 10b 256x256)", cases["case_B"]["pic_setups"][0]["raw"],
                        "case_C (4:4:4 10b 256x256)", cases["case_C"]["pic_setups"][0]["raw"])
    if "case_D" in cases and len(cases["case_D"]["pic_setups"]) >= 2:
        diff_pic_setups("case_D f0 (IDR)", cases["case_D"]["pic_setups"][0]["raw"],
                        "case_D f1 (P-frame)", cases["case_D"]["pic_setups"][1]["raw"])
    if "case_A" in cases and "case_E" in cases and cases["case_A"]["pic_setups"] and cases["case_E"]["pic_setups"]:
        diff_pic_setups("case_A (256x256)", cases["case_A"]["pic_setups"][0]["raw"],
                        "case_E (320x240)", cases["case_E"]["pic_setups"][0]["raw"])

    if args.json_out:
        out_obj = {}
        for k, v in cases.items():
            v_clean = dict(v)
            v_clean["pic_setups"] = [
                {kk: vv for kk, vv in ps.items() if kk != "raw"}
                for ps in v["pic_setups"]
            ]
            out_obj[k] = v_clean
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(out_obj, f, separators=(",", ":"))
        print(f"\nWrote JSON summary to {args.json_out}")


if __name__ == "__main__":
    main()
