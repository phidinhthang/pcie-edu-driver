# QEMU `edu` PCIe Driver Development — Educational Guide

> Saved 2026-05-29. Concepts + concrete flow for learning PCIe DMA driver development
> using QEMU's built-in `edu` educational PCI device. For learning purposes only.

---

# Part 1 - The idea behind it

## 1.1 What a PCIe device looks like to software

Every PCIe driver is built on four mechanisms the device exposes to the OS:

| Mechanism | What it is | Direction |
|---|---|---|
| **Config space** | Standardized 256B/4KB register area the OS reads at boot to *discover* the device (vendor/device ID, BAR sizes, capabilities). | OS reads it to enumerate |
| **BARs (Base Address Registers)** | Device requests N bytes mapped into physical address space; OS picks an address; CPU then reads/writes device registers as memory. This is **MMIO**. | CPU <-> device |
| **Interrupts** | Device signals the CPU ("done / event happened"). Modern style = **MSI/MSI-X** (a memory write turned into an interrupt). | device -> CPU |
| **DMA (bus mastering)** | Device reads/writes **host RAM directly**, no CPU byte-copying. The point of high-throughput hardware. | device <-> host RAM |

A driver's job: discover the device, map its registers, hand it buffer addresses, kick off
work, and react to interrupts.

## 1.2 What the QEMU `edu` device gives you

`edu` is a **fake PCI device built into QEMU** (`hw/misc/edu.c`, docs `docs/specs/edu.rst`).
Minimal but exercises all four mechanisms. Not a model of any real chip - a teaching rig with a
tiny made-up datasheet.

It presents:
- **One BAR (BAR0)**, a 1 MB MMIO region holding its registers.
- A **liveness register** (write X, read back ~X) - the "hello world" of MMIO.
- A **factorial unit**: write N, computes N! *asynchronously in a background thread*, sets a
  "busy" bit while working, can **raise an interrupt when done**. Stand-in for "device does slow
  work, then signals completion."
- A **DMA engine** with a 4 KB internal buffer: give it a host RAM address, count, and
  direction; it copies between host RAM and its internal buffer, optionally raising an interrupt
  on completion.

### The edu "datasheet" (register map, offsets within BAR0, little-endian)

| Offset | Size | Access | Meaning |
|---|---|---|---|
| `0x00` | 4 | RO | **Identification** - reads a version magic (`0x010000ed`). First MMIO sanity check. |
| `0x04` | 4 | RW | **Liveness** - write X, read returns `~X`. |
| `0x08` | 4 | RW | **Factorial** - write N; when done, read N!. |
| `0x20` | 4 | RW | **Status** - bit 0 = computing (busy); bit 7 = raise IRQ when factorial finishes. |
| `0x24` | 4 | RO | **Interrupt status** - which interrupt is pending. |
| `0x60` | 4 | WO | **Interrupt raise** - write a value -> device raises IRQ carrying that value. |
| `0x64` | 4 | WO | **Interrupt ACK** - write to clear a pending IRQ. |
| `0x80` | 8 | RW | **DMA source address**. |
| `0x88` | 8 | RW | **DMA destination address**. |
| `0x90` | 8 | RW | **DMA count** (bytes). |
| `0x98` | 4 | RW | **DMA command** - bit 0 = start; bit 1 = direction (0: host RAM->device buffer, 1: device buffer->host RAM); bit 2 = raise IRQ on completion. |

Two DMA details that matter:
- The device's internal buffer lives at device-side addresses `[0x40000, 0x41000)` (4 KB). A
  host->device transfer copies from a *host RAM* address into that internal buffer.
- The device documents a **28-bit DMA address space**, so the driver must set a DMA mask
  ("only give me DMA buffers reachable in 28 bits"). A real-world lesson - every device has
  addressing limits.

**Identifiers:** edu appears as **vendor `0x1234`, device `0x11e8`**. Our driver matches those.

## 1.3 What we can learn (the mapping that makes it worth it)

| edu feature | Real-world skill it teaches |
|---|---|
| Probe matching vendor/device ID | PCI enumeration, `pci_driver` registration |
| Map BAR0, read ID register | `pci_iomap`, MMIO `ioread`/`iowrite`, endianness |
| Liveness register | Read/write register protocol, verifying device is alive |
| Factorial + status bit | Command/status register pattern, polling for completion |
| Factorial + interrupt | MSI setup, `request_irq`, interrupt handlers, ACKing |
| DMA engine | `dma_alloc_coherent`, DMA masks, descriptor-style programming, completion via IRQ |
| (optional) char device | Exposing the device to userspace |

Writing a driver for a real FPGA PCIe DMA core (or AWS's XDMA shell) has the **same structure** -
only the register map changes. That's why this is the right first rung.

---

# Part 2 - The flow / steps

## 2.1 Architecture of the setup (and one important "why")

You cannot just load the driver in WSL2 directly: WSL2's VM is managed by Microsoft, so you
can't attach arbitrary virtual devices (no edu device on its PCI bus) and can't easily load
out-of-tree modules against its kernel.

So we run **a VM inside WSL2**:

```
Windows
  +- WSL2 (Ubuntu)               <- build host: editor, compiler, QEMU
       +- QEMU  (-device edu)    <- boots a *guest* Linux that HAS the edu device
            +- Guest Linux       <- here we insmod our driver and read dmesg
```

Build in WSL2 (or in the guest), run in the guest, watch `dmesg`. If the driver crashes the
guest kernel, just reboot the VM - the host is never at risk.

## 2.2 Tools to install (in WSL2 Ubuntu)

```bash
sudo apt update
sudo apt install -y \
  qemu-system-x86 qemu-utils \    # QEMU + the edu device (built in - no repo to clone)
  build-essential bc bison flex \ # compiler toolchain for kernel modules
  libelf-dev libssl-dev \         # kernel build deps
  wget cpio                       # fetching images / building initramfs
```

Note on "cloning repos": the edu device **ships inside `qemu-system-x86`** - nothing to clone to
use it. Clone QEMU source only if you want to *read* `hw/misc/edu.c` (optional). The only code we
author is the driver.

## 2.3 Choosing the guest (two options)

- **Option A - Ubuntu cloud image (recommended for learning).** Boot real Ubuntu in QEMU,
  install kernel headers inside, build and load there. Familiar, has `apt`, easy. Boots slower.
- **Option B - Buildroot / minimal initramfs.** Tiny custom Linux booting in ~1s. Blazing fast
  edit-build-test loop, authentic embedded feel. More upfront setup.

Start with A to learn; switch to B later when fast iteration matters. (Nested virtualization is
on by default in recent WSL2, so QEMU can use KVM; otherwise software emulation still works.)

Typical launch line (note how `-device edu` attaches the device):

```bash
qemu-system-x86_64 \
  -enable-kvm -m 2048 -smp 2 \
  -drive file=ubuntu.qcow2,if=virtio \
  -device edu \                       # <- attaches the educational PCI device
  -nographic                          # serial console so we stay in the terminal
```

Confirm inside the guest:
```bash
lspci -nn | grep 1234:11e8   # our device should appear
```

## 2.4 What we implement - staged, one idea per stage

Build in small verifiable stages. Each stage ends with "load it, check `dmesg`, confirm one new
thing works." Never write 300 lines and hope.

- **Stage 0 - Skeleton.** Register a `pci_driver` matching `1234:11e8`; `probe`/`remove` that
  just `printk`. Goal: see probe fire. Teaches module lifecycle + PCI matching.
- **Stage 1 - MMIO.** In probe: `pci_enable_device`, request BAR0, `pci_iomap`. Read the
  identification register (expect version magic), then write/read liveness and confirm `~X`.
  Goal: prove you can talk to device registers.
- **Stage 2 - Polled completion.** Write N to factorial, poll the status busy bit, read back N!.
  Goal: the command/status pattern.
- **Stage 3 - Interrupts (MSI).** `pci_alloc_irq_vectors` for MSI, `request_irq`. Set
  "raise IRQ when done", trigger a factorial, let the interrupt wake you. In the handler, read
  interrupt-status and ACK it. Goal: the full interrupt path.
- **Stage 4 - DMA (main event).** Set the 28-bit DMA mask, `dma_alloc_coherent` a buffer, fill
  it, program src/dst/count/command to push it into the device's internal buffer, then DMA it
  back and verify the round-trip - with completion via interrupt. Goal: the full DMA lifecycle.
- **Stage 5 (optional) - Userspace.** Expose a char device or sysfs so a user program can
  trigger a DMA. Goal: connecting kernel to userspace.

### Stage 0 skeleton (teaching style)

```c
#include <linux/module.h>
#include <linux/pci.h>

/* The edu device advertises these on the PCI bus. We match on them. */
#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

/* Tells the kernel: "this driver handles that vendor:device pair." */
static const struct pci_device_id edu_ids[] = {
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

/* Called by the kernel when a matching device is found on the bus. */
static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    dev_info(&pdev->dev, "edu: probe! found our device\n");
    return 0;  /* 0 = "I claimed this device successfully" */
}

static void edu_remove(struct pci_dev *pdev)
{
    dev_info(&pdev->dev, "edu: remove\n");
}

static struct pci_driver edu_driver = {
    .name     = "edu",
    .id_table = edu_ids,
    .probe    = edu_probe,
    .remove   = edu_remove,
};
module_pci_driver(edu_driver);  /* registers/unregisters on insmod/rmmod */

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Educational driver for the QEMU edu PCI device");
```

Load in the guest, `dmesg | tail`, expect `edu: probe! found our device`. That one line proves
enumeration, matching, and the build pipeline all work. Each stage then adds exactly one concept.

---

## Suggested next steps

1. Set up the WSL2 build environment + a guest image with the edu device attached.
2. Walk Stages 0->4, building and running each one and watching `dmesg` change.
