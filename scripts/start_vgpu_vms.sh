#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# 50/50 CPU (4 vCPU each on 8-thread host) + 50/50 GPU (GRID P4-4Q / nvidia-65, 4096 MiB VRAM each)
# Launches AlmaLinux 10.3 Pure Wayland (192.168.1.68) and Windows 11 24H2 WDDM (192.168.1.74 DHCP / 192.168.1.65 static fallback)
# on the UEFI Coreboot Tesla P4 KVM host (192.168.1.24, 580.196.09-vgpu-kvm).
set -euo pipefail

PCI_BDF="${PCI_BDF:-0000:01:00.0}"
MDEV_TYPE="${MDEV_TYPE:-nvidia-65}"
MDEV_CREATE="/sys/bus/pci/devices/${PCI_BDF}/mdev_supported_types/${MDEV_TYPE}/create"
VM_BASE="${VM_BASE:-/home/zero/vms}"
BRIDGE="${BRIDGE:-br0}"

ensure_mdev() {
  local uuid="$1"
  if [[ ! -e "/sys/bus/mdev/devices/${uuid}" ]]; then
    echo "${uuid}" > "${MDEV_CREATE}"
  fi
}

launch_alma_vm() {
  local name="vm-alma" dir="${VM_BASE}/alma" mem="5120" mac="52:54:00:11:22:03"
  local uuid="11111111-0000-0000-0000-000000000003" vnc="13"
  if [[ -f "${dir}/qemu.pid" ]] && kill -0 "$(cat "${dir}/qemu.pid")" 2>/dev/null; then
    echo "${name} already running (PID $(cat "${dir}/qemu.pid"))"
    return 0
  fi
  ensure_mdev "${uuid}"
  qemu-system-x86_64 -name "${name}" -machine q35,accel=kvm -cpu host,kvm=on \
    -m "${mem}" -smp 4,sockets=1,cores=2,threads=2 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file="${dir}/OVMF_VARS.fd" \
    -drive file="${dir}/disk.qcow2",if=virtio,cache=writeback \
    -drive file="${dir}/seed.iso",media=cdrom,if=virtio \
    -netdev "bridge,id=net0,br=${BRIDGE}" -device "virtio-net-pci,netdev=net0,mac=${mac}" \
    -device "vfio-pci,sysfsdev=/sys/bus/mdev/devices/${uuid},display=off" \
    -vga std -vnc "0.0.0.0:${vnc}" \
    -serial "file:${dir}/serial.log" \
    -qmp "unix:${dir}/qmp.sock,server,nowait" \
    -pidfile "${dir}/qemu.pid" -daemonize
  echo "Started ${name} (4 vCPU, ${mem}M RAM, GRID P4-4Q 4096M VRAM)"
}

launch_win11_vm() {
  local name="vm-win11" dir="${VM_BASE}/win11" mem="6144" mac="52:54:00:11:22:05"
  local uuid="11111111-0000-0000-0000-000000000005" vnc="15"
  if [[ -f "${dir}/qemu.pid" ]] && kill -0 "$(cat "${dir}/qemu.pid")" 2>/dev/null; then
    echo "${name} already running (PID $(cat "${dir}/qemu.pid"))"
    return 0
  fi
  ensure_mdev "${uuid}"
  mkdir -p "${dir}/tpm"
  if ! pgrep -f "swtpm.*${dir}/tpm/swtpm.sock" >/dev/null 2>&1; then
    rm -f "${dir}/tpm/swtpm.sock"
    swtpm socket --tpmstate dir="${dir}/tpm" --ctrl type=unixio,path="${dir}/tpm/swtpm.sock" --tpm2 --daemon
    sleep 1
  fi
  qemu-system-x86_64 -name "${name}" -machine q35,accel=kvm,smm=on \
    -cpu host,kvm=on,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time,hv_vpindex,hv_synic,hv_stimer,hv_reset,hv_runtime \
    -m "${mem}" -smp 4,sockets=1,cores=2,threads=2 \
    -global driver=cfi.pflash01,property=secure,value=on \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.secboot.fd \
    -drive if=pflash,format=raw,file="${dir}/OVMF_VARS.fd" \
    -chardev "socket,id=chrtpm,path=${dir}/tpm/swtpm.sock" \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis,tpmdev=tpm0 \
    -drive "file=${dir}/disk.qcow2,if=none,id=hd0,cache=none" \
    -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
    -netdev "bridge,id=net0,br=${BRIDGE}" -device "e1000e,netdev=net0,mac=${mac}" \
    -device "vfio-pci,sysfsdev=/sys/bus/mdev/devices/${uuid},display=off" \
    -vga std -vnc "0.0.0.0:${vnc}" \
    -serial "file:${dir}/serial.log" \
    -qmp "unix:${dir}/qmp.sock,server,nowait" \
    -pidfile "${dir}/qemu.pid" -daemonize
  echo "Started ${name} (4 vCPU, ${mem}M RAM, GRID P4-4Q 4096M VRAM)"
}

launch_alma_vm
launch_win11_vm
