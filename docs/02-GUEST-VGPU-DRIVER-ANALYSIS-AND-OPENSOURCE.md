# 09 — Pascal (`GP104GL` / Tesla P4) vGPU Guest Driver Analysis & Open-Source Implementation

## 1. Executive Answers

1. **Do the Pascal vGPU guest drivers contain signed NVIDIA firmware blobs, or are they just a host–guest interface?**
   - Inside `NVIDIA-GRID-Linux-KVM-535.309.01-539.72/Guest_Drivers/`:
     - `NVIDIA-Linux-x86_64-535.309.01-grid.run`, `nvidia-linux-grid-535_535.309.01_amd64.deb`, `nvidia-linux-grid-535-535.309.01-1.x86_64.rpm` all package the **same** unified proprietary kernel RM binary `kernel/nvidia/nv-kernel.o_binary` (SHA-256 `8c753d0898c478c56e22958d35aa271d671b3eb58f45069369b1df19480433f0`, bit-for-byte identical to the host `vgpu-kvm` binary, holding `259` bare-metal `BindataStorage` archives / `32,498,832` B raw), plus `firmware/gsp_tu10x.bin` (`23 MiB`) and `firmware/gsp_ga10x.bin` (`37 MiB`) for bare-metal Turing+/Ampere+ GSP GPUs.
     - `539.72_grid_win10_win11_server2019_server2022_dch_64bit_international.exe` (`314` files, `1.85 GB` extracted) packages `Display.Driver/nvlddmkm.sys` (`57 MiB`, containing `"VRPC"` and `"VGPU"` markers), `gsp_tu10x.bin` / `gsp_ga10x.bin`, and Windows/WSL2 UMDs, with zero separate Pascal `.bin` blobs.
   - **In vGPU guest mode on Pascal (`GP104`), NONE of those Falcon or GSP firmware blobs are required or loaded.** All physical Falcons (`ACR`, `PMU`, `SEC2`, `FECS`, `GPCCS`, `NVENC`, `NVDEC`) and `PrivSec` Light-Secure authentication run strictly on the **host** physical GPU. The guest kernel driver is purely a **paravirtualized `0x8f000..0x8f05c` BAR0 + `"VRPC"` shared-ring interface** between the guest VM and the host hypervisor plugin.
2. **Does the guest package have `kernel-open`?**
   - `NVIDIA-Linux-x86_64-535.309.01-grid.run` ships `kernel-open/nvidia/nv-kernel.o_binary` (`9,970,456` B, SHA-256 `021c7ec28750191ae6d3f4d6a8876973f76275f70f29e336c5be8eb8106c94f3`, entropy `5.9222`, `34` Turing+/Ampere+/Ada GSP `BindataStorage` archives and zero Pascal Falcon blobs). It already includes unstripped `objvgpu.c` / `rpc.c` (`_issueRpcAsync`, `_issueRpcLarge`, `_issueRpcAndWait`, `rpcWriteCommonHeader`), `virt_mem_allocator_vgpu.c`, and Pascal kernel RM files (`kern_gpu_gp100.c`, `kernel_fifo_gp102.c`, `kgraphics_gp100.c`), **but** NVIDIA explicitly stubbed `rm_is_vgpu_supported_device` at `0xd1a20` to return `0` (`xor %eax,%eax; ret`) in R535 `kernel-open` (`README.txt`: *"vGPU, are not yet supported"*).
3. **What parts can be open source vs. proprietary?**
   - **Guest Kernel Driver (`kmods/nova_vgpu_guest/` + Rust `nova_vgpu::guest`) — 100% Open Source, Zero Signed Blobs Needed:** Full PCI probing (`10de:1bb3`), `0x8f020` virtual GPU check, `SEND_RING`/`RECV_RING`/`EVENT_RING` DMA setup, `"VRPC"` header + RPC roundtrips, MSI/INTx IRQ handling, and BAR1 `mmap()`.
   - **Userspace Scope Caveat:** While proprietary userspace libraries (`libcuda.so.535.309.01`, `libnvidia-glcore.so.535.309.01`, `libnvidia-encode.so.535.309.01`, etc.) contain no Falcon firmware blobs, they are closed-source PTX/shader/API compilers expecting `/dev/nvidiactl` + `/dev/nvidia0` RM ioctls. Full open-source guest 3D/compute would pair our open-source guest kernel driver with Mesa (`nouveau`/`nvk`/`zink`), whereas `nvidia-gridd` (`5,569,272` B, SHA-256 `318e6c34...`) + `libFlxCore64.so.2018.02` (`10.6 MB`) handles FlexNet/DLS licensing (not reimplemented or bypassed here).

---

## 2. Disassembly & REA `6.9.0` / Ghidra `12.2` Proof

Focused ELF slice `p4_guest_vgpu_535_slice.o` (`sha256: fc1f421400121784521cda52912dc6763b5fc271f634ad65ce0257f5cfd82e56`, `8 / 8` functions decompiled, `0` failed, REA evidence `ev_cc7bc0ea983f2082459867df11c104c4cab69d8f43c26cc959f82d8273f41f5b` in [evidence/vgpu-535/guest535_slice_decomp.json](../evidence/vgpu-535/guest535_slice_decomp.json)):

| Function (`535.309.01`) | Role in Guest RM (`nv-kernel.o_binary`) | Hardware Touchpoints Observed |
| :--- | :--- | :--- |
| `_nv013341rm` (`0x86d8e0`) | Initialize `SEND_RING` & `RECV_RING` | Zeroes two `4 KiB` DMA pages; encodes `(pfn << 12) \| 0xb` (or `0x80b`) + `pfn >> 20` into `0x8f000`/`0x8f004`/`0x8f008` and `0x8f010`/`0x8f014`/`0x8f01c` |
| `_nv044238rm` (`0x88de00`) & `rpcWriteCommonHeader` (`0x2a7da0` in `kernel-open`) | Construct 32-byte `"VRPC"` header | Writes `+0x00 = 0x03000000`, `+0x04 = 0x43505256` (`"VRPC"`), `+0x08 = 0x20 + len`, `+0x0c = func_id`, `+0x10 = 0xffffffff`, `+0x14 = 0xffffffff` |
| `_nv013262rm` (`0x86cc00`) | Enqueue `"VRPC"` request & ring doorbell | Assigns `seq`, writes staging `(pfn << 12) \| 0xb` & `pfn >> 20` into `SEND_RING[PUT]`, wraps `(PUT + 8) & 0xfff`, writes `0x8f008` (`SEND_PUT`) |
| `_nv013259rm` (`0x86cd60`) | Poll / drain `RECV_RING` reply | Polls `RECV_RING_GET`/`PUT` and advances `0x8f01c` (`RECV_GET`) |
| `_nv045460rm` (`0x86e6d0`) & `_nv037870rm` (`0xe9ad0`) | Read virtual vGPU capabilities | Reads `0x8f020` (`NV_VGPU_CONFIG`), tests bit 17 (`ecc`) and bit 20 |
| `_nv025857rm` (`0x356c0` / `0x360d5`) | Early virtual mode check | Reads `0x8f020` `[13:12]` to detect virtualized GPUs without booting bare-metal Falcons |

Verified `VRPC` `func_id` table (`NOP = 0x00` .. `GET_ENGINE_UTILIZATION = 0x3b` in proprietary `nv-kernel.o_binary` `.rodata+0x31dca96`; extended up to `0x79` in `kernel-open`):
- `0x01` `SET_GUEST_SYSTEM_INFO`, `0x02` `ALLOC_ROOT`, `0x03` `ALLOC_DEVICE`, `0x04` `ALLOC_MEMORY`, `0x06` `ALLOC_CHANNEL_DMA`, `0x0a` `FREE`, `0x16` `IDLE_CHANNELS`, `0x1a` `RM_CONTROL` / `DMA_CONTROL` (`cmd 0x2080...` / `0xc36f...` confirmed in `libnvidia-vgpu.so` `_nv007237vgpu`), `0x32` `GET_STATIC_INFO`, `0x3f` `SET_GUEST_SYSTEM_INFO_EXT`, `0x4c` `GET_STATIC_INFO2`, `0x4f` `GET_CONSOLIDATED_STATIC_INFO`.

---

## 3. Open-Source Deliverables Built & Verified

### 3.1 Clean-Room Linux `7.0` Guest Kernel Driver ([kmods/nova_vgpu_guest/](../kmods/nova_vgpu_guest/nova_vgpu_guest.c))
- GPLv2 PCI kernel module (`nova_vgpu_guest.ko`, [Makefile](../kmods/nova_vgpu_guest/Makefile), [nova_vgpu_guest.h](../kmods/nova_vgpu_guest/nova_vgpu_guest.h), [nova_vgpu_guest.c](../kmods/nova_vgpu_guest/nova_vgpu_guest.c)).
- **Safety guard**: Probes `10de:1bb3` only when `NV_VGPU_CONFIG` (`0x8f020`) bit `0` is `1` (`allow_baremetal_probe=0` default), refusing to bind onto bare-metal GPUs.
- Allocates `4 KiB` DMA-coherent `SEND_RING`, `RECV_RING`, `EVENT_RING`, and `"VRPC"` staging pages via `dmam_alloc_coherent`, programs `0x8f000..0x8f03c`, handles MSI/INTx `0x8f028` (`W1C 0x3c0`), exposes `/dev/nova-vgpu-guest%d` (`GET_INFO`, `VRPC`, `READ_REG` ioctls + BAR1 write-combine `mmap`), and **compiled cleanly with zero warnings against `Linux 7.0.0-43-generic`** on `192.168.1.15`.

### 3.2 Clean-Room Rust Guest Driver & Host<->Guest Loopback ([nova-vgpu/src/guest.rs](../nova-vgpu/src/guest.rs))
- Implements `GuestVgpuTransport`, `NovaGuestDriver` (`probe_and_init`, `call_vrpc`, `set_guest_system_info`, `get_static_info`, `shutdown`), ring PTE encoders (`encode_ring_pte_lo`, `encode_ring_pte_hi`, `decode_ring_pte`), and `HostLoopbackTransport` wiring `NovaGuestDriver` directly to `PascalVgpuInstance` (`VMMU` GFN pinning + `PascalBar0Emulator` + `VrpcEngine`).
- CLI runner [nova_vgpu_guest_tool.rs](../nova-vgpu/src/bin/nova_vgpu_guest_tool.rs) executes an end-to-end `GRID P4-2Q` Guest <-> Host `"VRPC"` bring-up (`SetGuestSystemInfo=0`, `GetStaticInfo535=0 (fb=0x74000000)`, `AllocRoot=0`, `AllocDevice535=0`, `AllocChannelDma=0`) with zero Falcon or proprietary binaries.

### 3.3 Live QEMU/KVM `vfio-mdev` Hardware Verification on `192.168.1.63`
To validate the guest kernel driver inside a real virtual machine backed by Linux `vfio-mdev` on the bare-metal Tesla P4 host (`192.168.1.63`, `h61`, `Linux 7.0.0-43-generic`, `0000:01:00.0` `10de:1bb3`):
- Created the GPLv2 host `vfio-mdev` mediator module [kmods/nova_vgpu_mdev/nova_vgpu_mdev.c](../kmods/nova_vgpu_mdev/nova_vgpu_mdev.c) ([Makefile](../kmods/nova_vgpu_mdev/Makefile)) implementing Linux `7.0` `mdev_register_parent`, `vfio_register_emulated_iommu_dev`, `get_region_info_caps`, MSI/INTx `eventfd_signal`, `vmalloc_user` `16 MiB` BAR1 `mmap`, and guest DMA ring transfers via `vfio_dma_rw()`.
- Instantiated mdev `aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee` under `/sys/devices/platform/nova_vgpu_parent/mdev_supported_types/platform-nvidia-64/create` (`GRID P4-2Q`, IOMMU group `10`) and booted `qemu-system-x86_64` (`10.2.1`, `-enable-kvm -cpu host -m 1024M -device vfio-pci,sysfsdev=/sys/bus/mdev/devices/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee`) with a `Linux 7.0.0-43-generic` + initramfs guest image running [test_guest_ioctl.c](../kmods/nova_vgpu_guest/test_guest_ioctl.c).
- **Observed Live Guest Output**:
  - Guest PCI enumeration discovered `0000:00:04.0: 0x10de:0x1bb3 sub=0x1205`.
  - `nova_vgpu_guest.ko` attached (`BOOT0=0x134000a1 FB=1856 MiB (zero Falcon blobs loaded)`) and created `/dev/nova-vgpu-guest0`.
  - `test_guest_ioctl` verified `GET_INFO` (`pmc_boot0=0x134000a1`, `vgpu_config=0x00040201`, `fb=0x74000000`, `bar1=0x1000000`), `READ_REG` (`0x000000`, `0x08f020`, `0x10f20c`, `0x500c30`), `"VRPC"` pre-init rejection (`0x56`) followed by `SET_GUEST_SYSTEM_INFO` (`0x0`), `GET_STATIC_INFO` (`0x0`, `fb=0x74000000`, `dev=0x1bb3`, `subsys=0x1205`), `ALLOC_ROOT`/`ALLOC_DEVICE`/`ALLOC_MEMORY`/`ALLOC_CHANNEL_DMA`/`RM_CONTROL` (`0x0`), `BAR1_MMAP_OK readback=0xdeadbeefcafebabe`, and `FINAL_STATS: rpc_completed=8 irq_count=8` (`GUEST_IOCTL_TEST_PASSED`).
- **Host Verification & Cleanup**: Host `dmesg` confirmed `close mdev (rpcs=8, bar1[0]=0xdeadbeefcafebabe)` on VM shutdown; mdev removal and `rmmod nova_vgpu_mdev` left the physical Tesla P4 `580.178.04` driver healthy (`nvidia-smi`: `Tesla P4`, `3MiB / 8192MiB`).
