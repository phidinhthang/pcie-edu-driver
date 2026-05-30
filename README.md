# Learning PCIe DMA Driver Development with QEMU's `edu` device

A hands-on, from-scratch course on the **host-software side of PCIe**: how a Linux driver
discovers a PCIe device, maps its registers (MMIO via BARs), handles MSI interrupts, and moves
data with DMA — built incrementally against QEMU's built-in `edu` educational PCI device.

No physical FPGA board required. Everything runs in a throwaway QEMU guest VM, so a kernel panic
just means "reboot the VM," never "reboot your machine."

## Who this is for

Someone comfortable with **C** and **hardware concepts** (registers, buses, bit fields) but **new
to Linux kernel / driver development**. If you come from the FPGA/HDL side, this teaches the
*software* half of the PCIe link you already understand at the wire level.

## What you'll be able to do at the end

Write — and understand every line of — a Linux kernel driver that:

1. Matches and claims a PCIe device by vendor/device ID (`pci_driver`, probe/remove).
2. Maps a BAR and talks to device registers over MMIO (`ioread`/`iowrite`).
3. Drives a command/status register and polls for completion.
4. Sets up **MSI interrupts** and handles + ACKs them.
5. Performs **DMA** between host RAM and the device, with completion via interrupt.
6. Exposes the device to userspace through a character device.

The same structure carries over to a real FPGA PCIe DMA core or the AWS F1/F2 XDMA shell — only
the register map changes.

## How the course is organized

Each chapter lives in `chapters/NN-name/` and contains:

- `README.md` — the teaching text: the *why*, the theory, and **every command to run** (clearly
  marked host vs. guest), with expected output.
- Heavily-commented code (`.c`, `Makefile`, scripts) you build and load.

Chapters build on each other. Do them in order.

| Ch | Folder | What you add |
|----|--------|--------------|
| 0 | `chapters/00-environment` | Boot a QEMU guest with `-device edu`; confirm `lspci` sees it |
| 1 | `chapters/01-skeleton`    | A `pci_driver` skeleton; see `probe` fire |
| 2 | `chapters/02-mmio`        | Map BAR0; read the ID register; liveness read/write |
| 3 | `chapters/03-factorial-polling` | Command/status register; poll for completion |
| 4 | `chapters/04-interrupts-msi` | MSI setup, interrupt handler, ACK |
| 5 | `chapters/05-dma`         | DMA mask, coherent buffer, round-trip transfer |
| 6 | `chapters/06-userspace`   | Character device to drive it all from userspace |

## How we work (the teaching method)

- **You run the commands.** The course hands you each command, explained, with expected output —
  you type it yourself to build real muscle memory. Report back what you see.
- **One concept per step, verified before moving on.** We never write 300 lines and hope.
- **Pause-and-reflect.** At natural breakpoints, the course stops to surface the questions,
  pitfalls, and misconceptions a newcomer usually hits right there.

See `COURSE-CONTRACT.md` for the conventions (naming, register macros, prompt markers) that every
chapter follows.

## Start here

Open [`chapters/00-environment/README.md`](chapters/00-environment/README.md).
