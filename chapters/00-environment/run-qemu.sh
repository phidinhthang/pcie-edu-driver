#!/usr/bin/env bash
# ============================================================================
# run-qemu.sh — boot the educational guest VM with the QEMU `edu` PCI device.
#
# Run this on the WSL2 HOST (host$). It launches QEMU, which boots a guest
# Ubuntu that HAS the edu device on its virtual PCI bus. Inside that guest we
# will build and insmod our driver.
#
# Quit the VM from the serial console with:  Ctrl-a  then  x
# (Ctrl-a is QEMU's escape key in -nographic mode; x = quit.)
# ============================================================================
set -euo pipefail

# --- Locate paths relative to THIS script, so it works from any directory ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"   # chapters/00-environment -> repo root
VM_DIR="$REPO_ROOT/vm"                         # where we keep the disk + seed images

DISK="$VM_DIR/edu-guest.qcow2"   # guest root disk: an overlay on the cloud image
SEED="$VM_DIR/seed.img"          # cloud-init seed: injects the login password etc.

# --- Fail early with a clear message if setup wasn't done yet ----------------
for f in "$DISK" "$SEED"; do
  if [[ ! -f "$f" ]]; then
    echo "ERROR: missing '$f'." >&2
    echo "       Run the Chapter 0 setup steps (download image, make seed) first." >&2
    exit 1
  fi
done

# --- Use KVM if available (near-native speed); else fall back to emulation ---
# /dev/kvm is writable when nested virtualization is on (it is, on this host).
ACCEL=()
if [[ -w /dev/kvm ]]; then
  ACCEL=(-enable-kvm -cpu host)            # -cpu host = expose the real CPU to the guest
  echo "[run-qemu] KVM acceleration: ON"
else
  ACCEL=(-cpu max)                         # TCG software emulation: correct but slow
  echo "[run-qemu] KVM unavailable -> slow software emulation (TCG)"
fi

# --- Launch QEMU. Each flag explained: --------------------------------------
#   -m 2048 -smp 2          : 2 GB RAM, 2 vCPUs for the guest
#   -drive ...edu-guest...  : the root disk (virtio = fast paravirtual disk)
#   -drive ...seed.img...   : the cloud-init seed disk (read on first boot)
#   -device edu             : *** attaches the educational PCI device ***
#   -netdev user,...hostfwd : user-mode NAT networking; forward host:2222 -> guest:22
#   -device virtio-net-pci  : the guest-visible NIC for that netdev
#   -virtfs local,...       : share the host repo into the guest over 9p
#                             (mount tag 'eduhost' — we mount it inside the guest)
#   -nographic              : no GUI window; console + monitor on this terminal
exec qemu-system-x86_64 \
  "${ACCEL[@]}" \
  -m 2048 -smp 2 \
  -drive file="$DISK",if=virtio,format=qcow2 \
  -drive file="$SEED",if=virtio,format=raw \
  -device edu \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -virtfs local,path="$REPO_ROOT",mount_tag=eduhost,security_model=none,id=host0 \
  -nographic
