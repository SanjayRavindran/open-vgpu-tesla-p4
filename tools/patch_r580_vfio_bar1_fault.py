#!/usr/bin/env python3
"""
Patch upstream 580.196.09-vgpu-kvm nvidia-vgpu-vfio.c to serialize BAR1 fault
validation and PTE insertion under ops_lock and populate PTEs on concurrent
duplicate faults cleanly using vmf_insert_pfn_prot().

Idempotent: safe to re-run on an already-patched file.
"""

from __future__ import annotations

import pathlib
import sys


MARKER = "/* open-vgpu: serialize BAR1 fault validation + PTE insert under ops_lock */"

OLD_VALIDATE_HEAD = """\
    if (!vgpu_dev || !virt_addr || !host_addr || !size)
        return NV_ERR_INVALID_ARGUMENT;

    down(&vgpu_dev->ops_lock);
"""

NEW_VALIDATE_HEAD = """\
    if (!vgpu_dev || !virt_addr || !host_addr || !size)
        return NV_ERR_INVALID_ARGUMENT;
"""

OLD_VALIDATE_DUP = """\
    if (bar1_node->is_validated == NV_TRUE)
    {
        *dup_fault = NV_TRUE;
        NV_VGPU_DEV_LOG(VGPU_ERR, vgpu_dev,
                        "BAR1 guest address already mapped. guest_addr: 0x%llx"
                        "size: 0x%llx\\n", bar1_node->guest_addr, bar1_node->size);
        goto map_exit;
    }"""

NEW_VALIDATE_DUP = """\
    if (bar1_node->is_validated == NV_TRUE)
    {
        *dup_fault = NV_TRUE;
        *host_addr = bar1_node->host_addr + (guest_addr - bar1_node->guest_addr);
        *size = PAGE_SIZE;
        *prot = pgprot_noncached(*prot);
        goto map_exit;
    }"""

OLD_VALIDATE_TAIL = """\
map_exit:
    if (status != NV_OK)
        NV_VGPU_DEV_LOG(VGPU_ERR, vgpu_dev,
                        "Validate map request failed. status: 0x%x\\n", status);

    up(&vgpu_dev->ops_lock);
    return status;"""

NEW_VALIDATE_TAIL = """\
map_exit:
    if (status != NV_OK)
        NV_VGPU_DEV_LOG(VGPU_ERR, vgpu_dev,
                        "Validate map request failed. status: 0x%x\\n", status);

    return status;"""

OLD_FAULT_FN = """\
static vm_fault_t vgpu_mmio_fault(struct vm_fault *vmf)
{
    int ret;
    vgpu_dev_t *vgpu_dev = NULL;
    NV_STATUS status = NV_OK;
    NvU64 virtaddr = 0, host_addr = 0, req_size = 0;
    NvBool dup_fault = NV_FALSE;
    pgprot_t pg_prot;
    NvU64 guest_offset;
    loff_t pos;
    struct vm_area_struct *vma = vmf->vma;

    if (!vma || !vmf)
        return -EINVAL;

    virtaddr = vmf->address;

    vgpu_dev = vma->vm_private_data;
    if (!vgpu_dev)
        return -ENODEV;

    pg_prot = vma->vm_page_prot;
    guest_offset = virtaddr - vma->vm_start;
    pos = (vma->vm_pgoff << PAGE_SHIFT) + guest_offset;

    status = nv_vgpu_vfio_validate_map_request(vgpu_dev, pos, &virtaddr, &host_addr,
                                               &req_size, &pg_prot, &dup_fault);
    if (status != NV_OK)
        return -EIO;

    if (dup_fault == NV_TRUE)
        return VM_FAULT_NOPAGE;

    /*
     * Verify pgoff and req_size are valid and virtaddr is within
     * vma range
     */
    if (!host_addr || !req_size || (virtaddr < vma->vm_start) ||
        ((virtaddr + req_size) >= vma->vm_end))
        return -EINVAL;

    pg_prot = nv_adjust_pgprot(pg_prot);

    ret = remap_pfn_range(vma, virtaddr, (host_addr >> PAGE_SHIFT),
                          req_size, pg_prot);

    if (ret != 0)
        NV_VGPU_DEV_LOG(VGPU_ERR, vgpu_dev,
                        "Failed to do mmap for fault handler. ret: %d\\n", ret);

    return ret | VM_FAULT_NOPAGE;
}"""

NEW_FAULT_FN = """\
/* open-vgpu: serialize BAR1 fault validation + PTE insert under ops_lock */
static vm_fault_t vgpu_mmio_fault(struct vm_fault *vmf)
{
    int ret;
    vm_fault_t fault_ret = VM_FAULT_NOPAGE;
    vgpu_dev_t *vgpu_dev = NULL;
    NV_STATUS status = NV_OK;
    unsigned long fault_addr = 0;
    NvU64 virtaddr = 0, host_addr = 0, req_size = 0;
    NvBool dup_fault = NV_FALSE;
    pgprot_t pg_prot;
    NvU64 guest_offset;
    loff_t pos;
    struct vm_area_struct *vma = vmf ? vmf->vma : NULL;

    if (!vma || !vmf)
        return VM_FAULT_SIGBUS;

    fault_addr = vmf->address & PAGE_MASK;
    virtaddr = fault_addr;

    vgpu_dev = vma->vm_private_data;
    if (!vgpu_dev)
        return VM_FAULT_SIGBUS;

    pg_prot = vma->vm_page_prot;
    guest_offset = virtaddr - vma->vm_start;
    pos = (vma->vm_pgoff << PAGE_SHIFT) + guest_offset;

    down(&vgpu_dev->ops_lock);

    status = nv_vgpu_vfio_validate_map_request(vgpu_dev, pos, &virtaddr, &host_addr,
                                               &req_size, &pg_prot, &dup_fault);
    if (status != NV_OK)
    {
        fault_ret = VM_FAULT_SIGBUS;
        goto fault_unlock;
    }

    if (dup_fault == NV_TRUE)
    {
        if (host_addr)
        {
            pg_prot = nv_adjust_pgprot(pg_prot);
            fault_ret = vmf_insert_pfn_prot(vma, fault_addr,
                                            (host_addr >> PAGE_SHIFT), pg_prot);
        }
        goto fault_unlock;
    }

    /*
     * Verify pgoff and req_size are valid and virtaddr is within
     * vma range
     */
    if (!host_addr || !req_size || (virtaddr < vma->vm_start) ||
        ((virtaddr + req_size) > vma->vm_end))
    {
        fault_ret = VM_FAULT_SIGBUS;
        goto fault_unlock;
    }

    pg_prot = nv_adjust_pgprot(pg_prot);

    ret = remap_pfn_range(vma, virtaddr, (host_addr >> PAGE_SHIFT),
                          req_size, pg_prot);
    if (ret != 0)
    {
        NvU64 page_host = host_addr + (fault_addr - virtaddr);
        fault_ret = vmf_insert_pfn_prot(vma, fault_addr,
                                        (page_host >> PAGE_SHIFT), pg_prot);
        if (fault_ret & VM_FAULT_ERROR)
            NV_VGPU_DEV_LOG(VGPU_ERR, vgpu_dev,
                            "Failed to map BAR1 fault addr 0x%lx (remap ret=%d, fault_ret=0x%x)\\n",
                            fault_addr, ret, fault_ret);
    }

fault_unlock:
    up(&vgpu_dev->ops_lock);
    return fault_ret;
}"""

OLD_MMAP_BAR1 = """\
    if (index == nv_bar1_lo_index(pdev->is_virtfn, vgpu_dev->is_bar0_64b))
    {
        vma->vm_ops = &vgpu_mmio_ops;
        ret = 0;
        vgpu_dev->phys_mappings.bar1_vma = vma;
    }"""

NEW_MMAP_BAR1 = """\
    if (index == nv_bar1_lo_index(pdev->is_virtfn, vgpu_dev->is_bar0_64b))
    {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
        vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
#else
        vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
#endif
        vma->vm_ops = &vgpu_mmio_ops;
        ret = 0;
        vgpu_dev->phys_mappings.bar1_vma = vma;
    }"""


def patch_text(src: str) -> tuple[str, bool]:
    """Return (patched_text, changed). Raises ValueError on unexpected upstream code."""
    if MARKER in src:
        return src, False

    replacements = [
        ("nv_vgpu_vfio_validate_map_request head", OLD_VALIDATE_HEAD, NEW_VALIDATE_HEAD),
        ("nv_vgpu_vfio_validate_map_request dup_fault", OLD_VALIDATE_DUP, NEW_VALIDATE_DUP),
        ("nv_vgpu_vfio_validate_map_request tail", OLD_VALIDATE_TAIL, NEW_VALIDATE_TAIL),
        ("vgpu_mmio_fault", OLD_FAULT_FN, NEW_FAULT_FN),
        ("nv_vgpu_vfio_mmap BAR1", OLD_MMAP_BAR1, NEW_MMAP_BAR1),
    ]

    out = src
    for name, old, new in replacements:
        count = out.count(old)
        if count != 1:
            raise ValueError(f"Expected 1 occurrence of '{name}', found {count}")
        out = out.replace(old, new, 1)

    return out, True


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"Usage: {argv[0]} <path/to/nvidia-vgpu-vfio.c>", file=sys.stderr)
        return 2

    target = pathlib.Path(argv[1])
    if not target.is_file():
        print(f"ERROR: file not found: {target}", file=sys.stderr)
        return 1

    text = target.read_text(encoding="utf-8")
    try:
        patched, changed = patch_text(text)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if not changed:
        print(f"[OK] Already patched: {target}")
        return 0

    target.write_text(patched, encoding="utf-8")
    print(f"[OK] Patched BAR1 fault serialization in {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
