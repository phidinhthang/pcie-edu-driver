# Kickoff Prompt — PCIe Driver Development (QEMU `edu`, Educational)

> Paste the block below as your first message in a **new session inside a dedicated project
> directory** (not this sandbox). It carries all the context we established here so you can start
> building immediately.

---

## Copy-paste prompt

```
I'm doing a hands-on, EDUCATIONAL project to learn PCIe DMA driver development from scratch,
with no physical FPGA board. Please teach as we go (explain the "why", build incrementally,
verify each step) rather than dumping large amounts of code at once.

## My background
- FPGA developer for HFT/trading (latency-focused); strong with HDL/hardware concepts.
- Comfortable with C; new to Linux kernel/driver development and PCIe software side.
- Goal: deeply understand the host-software side of PCIe DMA (BARs/MMIO, MSI interrupts,
  DMA buffers, descriptor-style programming, completion handling) so it transfers later to
  real FPGA PCIe DMA cores and AWS F1/F2 XDMA shells.

## Environment
- Windows 11 host, with Ubuntu under WSL2 already installed.
- Plan: run a QEMU guest VM *inside* WSL2 with QEMU's built-in `edu` PCI device attached
  (we can't attach edu to WSL2's own VM, so we boot our own guest). Build in WSL2/guest,
  insmod in the guest, watch dmesg. Crashes only affect the throwaway guest.

## What I'm using to learn
- QEMU's `edu` educational PCI device (ships inside the qemu-system-x86 package; source at
  hw/misc/edu.c, docs at docs/specs/edu.rst). It exposes: BAR0 (1MB MMIO), a liveness register
  (write X -> read ~X), an async factorial unit (write N -> read N!, with a busy status bit and
  optional completion interrupt), and a DMA engine with a 4KB internal buffer.
- edu identifiers: PCI vendor 0x1234, device 0x11e8.
- edu register map (offsets in BAR0, little-endian):
    0x00 RO  Identification (version magic ~0x010000ed)
    0x04 RW  Liveness (write X, read ~X)
    0x08 RW  Factorial (write N, read N!)
    0x20 RW  Status (bit0=computing/busy, bit7=raise IRQ when factorial done)
    0x24 RO  Interrupt status
    0x60 WO  Interrupt raise
    0x64 WO  Interrupt ACK
    0x80 RW  DMA source address (8 bytes)
    0x88 RW  DMA destination address (8 bytes)
    0x90 RW  DMA count in bytes (8 bytes)
    0x98 RW  DMA command (bit0=start, bit1=direction 0:RAM->dev 1:dev->RAM, bit2=raise IRQ done)
  Device-internal DMA buffer lives at device addresses [0x40000, 0x41000). Device uses a
  28-bit DMA address space (driver must set a 28-bit DMA mask).

## Plan (staged — one concept per stage, verify via dmesg before moving on)
- Stage 0: skeleton pci_driver matching 1234:11e8, probe/remove printk. Confirm probe fires.
- Stage 1: pci_enable_device, map BAR0 (pci_iomap), read ID register, write/read liveness (~X).
- Stage 2: factorial via polling the status busy bit; read result.
- Stage 3: MSI interrupts — pci_alloc_irq_vectors, request_irq, raise-IRQ-on-factorial, handle + ACK.
- Stage 4: DMA — set 28-bit DMA mask, dma_alloc_coherent, program src/dst/count/command,
  round-trip host RAM <-> device buffer, completion via interrupt.
- Stage 5 (optional): expose to userspace via a char device / sysfs.

## What I want you to do first
1. Set up the WSL2 build environment and a QEMU guest image with `-device edu` attached
   (recommend a small Ubuntu cloud image to start; we can move to a buildroot/initramfs later
   for faster iteration). Give me exact commands and explain each.
2. Verify the edu device is visible in the guest (lspci shows 1234:11e8).
3. Then walk me through Stage 0 -> build, insmod, confirm "probe" in dmesg.

Teach in the incremental style above. Ask me before assuming anything about my setup.
```

---

## Notes for me (the human)
- Related reference notes already saved in the sandbox: `qemu-edu-driver-guide.md` (full
  concept + flow writeup), `pcie-dma-learning-without-board.md` (broader options incl. cocotb
  RTL sim, LitePCIe, AWS cloud FPGA), and `claude-code-usage-advice.md` (general usage advice).
- Bigger picture progression after edu: cocotb + cocotbext-pcie (RTL side, no board) ->
  LitePCIe (end-to-end, simulatable + deployable) -> AWS F1/F2 (real silicon).
```
