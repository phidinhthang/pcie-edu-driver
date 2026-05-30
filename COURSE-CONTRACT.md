# Course Contract

The fixed conventions every chapter obeys, so 7 independently-written chapters stay coherent
(same file names, same notation, same register macros). If a chapter needs to break a rule here,
update this file in the same change.

---

## 1. Audience & goal

- **Reader:** comfortable with C and hardware/register concepts; new to Linux kernel & driver dev.
- **Goal:** deeply understand the host-software side of PCIe DMA (MMIO/BARs, MSI, DMA, descriptor
  programming, completion handling), transferable to real FPGA PCIe DMA cores and AWS XDMA.
- **Substrate:** QEMU's `edu` device (vendor `0x1234`, device `0x11e8`), driven from an Ubuntu
  guest VM running under QEMU on a WSL2 Ubuntu host.

## 2. Teaching method (hard rules)

1. **The learner runs every educational command themselves.** Chapters present commands for the
   reader to type; the author does not execute install/boot/build/`insmod`/`dmesg` commands on
   their behalf. (Repo plumbing like `git` is fine for the author to run.)
2. **Reproducible from zero.** Every command is recorded in the chapter `README.md`, explained,
   and marked host vs. guest (see prompt convention below), with **expected output** shown.
3. **One concept per step, verified before moving on.** Each step ends with "load it / run it,
   observe X in `dmesg`/output."
4. **Pause-and-reflect.** At natural breakpoints within a chapter, stop to (a) ask what is still
   unclear and (b) proactively list the questions, pitfalls, and misconceptions a newcomer hits
   at that exact spot. Marked with a `> ⏸ Pause & reflect` callout.
5. **Theory before code.** Motivate the idea, then show the mechanism, then the code.
6. **No exercises/quizzes** (reader's choice). Keep chapters to explanation + build + run.
7. **Build-along cadence:** author writes a chapter's files, then reader builds/runs/verifies it,
   before the next chapter is written.

## 3. Prompt / command conventions

In every fenced command block, the prompt prefix tells you *where* to run it:

| Prefix | Where | Who |
|--------|-------|-----|
| `host$`  | WSL2 Ubuntu host shell (build host + QEMU host) | normal user |
| `guest$` | inside the QEMU guest VM | normal user `ubuntu` |
| `guest#` | inside the QEMU guest VM | root (via `sudo`) |

Expected output is shown immediately after, in its own block or commented.

## 4. Architecture (unchanged across the course)

```
WSL2 Ubuntu (host)         <- build host: edit code, run QEMU, run git
  └─ QEMU  (-device edu)    <- boots a guest Linux that HAS the edu device
       └─ Guest Ubuntu      <- here we `insmod` the driver and read `dmesg`
```

We must boot a guest because the `edu` device cannot attach to WSL2's own managed kernel.
The host repo is shared into the guest over **9p/virtfs** (mount tag `eduhost`) so we edit on the
host and build in the guest.

## 5. The shared code project

### 5.1 Layout

```
chapters/NN-name/
  README.md          # the chapter (theory + commands + expected output)
  qemu_edu.c         # the driver source for THIS chapter (self-contained snapshot)
  Makefile           # builds qemu_edu.ko against the guest kernel
  *.sh / *.c         # any helper scripts or userspace programs for the chapter
```

Each chapter keeps a **complete, self-contained** `qemu_edu.c` (a snapshot), so the reader can
`diff` consecutive chapters to see exactly what one concept added.

### 5.2 Fixed names

- **Kernel module:** `qemu_edu` → builds `qemu_edu.ko`. (`Makefile`: `obj-m += qemu_edu.o`)
- **Driver `.name`:** `"qemu_edu"`.
- **Private device struct:** `struct edu_drv`.
  ```c
  struct edu_drv {
      struct pci_dev *pdev;   /* the PCI device we claimed                 (Ch1) */
      void __iomem   *mmio;   /* BAR0 mapped into kernel virtual address   (Ch2) */
      int             irq;    /* MSI vector number                         (Ch4) */
      /* DMA fields (coherent buffer, dma_addr_t, completion) added in     Ch5  */
  };
  ```
- **Log prefix:** use `dev_info(&pdev->dev, "qemu_edu: ...")` / `dev_err` so messages are tagged
  with the device in `dmesg`.

### 5.3 Register map macros (define once, reuse verbatim every chapter)

```c
/* PCI identity — the edu device advertises these on the bus. */
#define EDU_VENDOR_ID       0x1234
#define EDU_DEVICE_ID       0x11e8

/* BAR0 register offsets (little-endian). */
#define EDU_REG_ID          0x00  /* RO  identification magic (~0x010000ed)            */
#define EDU_REG_LIVENESS    0x04  /* RW  write X, read back ~X                         */
#define EDU_REG_FACT        0x08  /* RW  write N; when done, read N!                   */
#define EDU_REG_STATUS      0x20  /* RW  bit0=computing, bit7=raise IRQ when fact done */
#define EDU_REG_INT_STATUS  0x24  /* RO  which interrupt is pending                    */
#define EDU_REG_INT_RAISE   0x60  /* WO  write value -> device raises IRQ w/ that value*/
#define EDU_REG_INT_ACK     0x64  /* WO  write -> clear pending IRQ                     */
#define EDU_REG_DMA_SRC     0x80  /* RW  8B DMA source address                          */
#define EDU_REG_DMA_DST     0x88  /* RW  8B DMA destination address                     */
#define EDU_REG_DMA_COUNT   0x90  /* RW  8B DMA byte count                              */
#define EDU_REG_DMA_CMD     0x98  /* RW  bit0=start bit1=dir bit2=irq-on-done           */

/* Status register bits. */
#define EDU_STATUS_COMPUTING   BIT(0)  /* factorial unit is busy            */
#define EDU_STATUS_IRQ_ENABLE  BIT(7)  /* raise IRQ when factorial finishes */

/* DMA command register bits. */
#define EDU_DMA_CMD_START      BIT(0)
#define EDU_DMA_CMD_TO_RAM     BIT(1)  /* 0 = host RAM -> device buffer; 1 = device buffer -> RAM */
#define EDU_DMA_CMD_IRQ        BIT(2)  /* raise IRQ when the transfer completes */

/* The edu device's internal DMA buffer (device-side addresses). */
#define EDU_DMA_BUF_BASE   0x40000     /* device buffer base address */
#define EDU_DMA_BUF_SIZE   0x1000      /* 4 KB                       */

/* The device decodes only a 28-bit DMA address space. */
#define EDU_DMA_MASK_BITS  28
```

### 5.4 MMIO accessor convention

- Use `ioread32`/`iowrite32` for 32-bit registers, `readq`/`writeq` (or `lo_hi_writeq`) for the
  64-bit DMA address/count registers.
- All edu registers are little-endian; the `ioreadN`/`iowriteN` family handles host endianness.

## 6. Per-chapter skeleton

1. **Where we are** — one paragraph: what prior chapters established, what this adds.
2. **The idea / why** — motivate the concept before any code.
3. **The mechanism** — how the kernel API or device feature works.
4. **The code** — complete `qemu_edu.c` (+ Makefile/helpers), heavily commented, comments tying
   back to the explanation.
5. **Build & run it** — exact host/guest commands with expected `dmesg`/output.
6. **Pause & reflect** — pitfalls / misconceptions / "did you understand X?".
7. **What's next** — one sentence pointing to the next chapter.

## 7. Chapter map

| Ch | Folder | Concept added |
|----|--------|---------------|
| 0 | `00-environment` | QEMU guest + `-device edu`; `lspci` confirms `1234:11e8`; build tools |
| 1 | `01-skeleton` | `pci_driver`, `MODULE_DEVICE_TABLE`, probe/remove printk |
| 2 | `02-mmio` | `pci_enable_device`, request BAR0, `pci_iomap`, ID + liveness |
| 3 | `03-factorial-polling` | write N, poll `EDU_STATUS_COMPUTING`, read N! |
| 4 | `04-interrupts-msi` | `pci_alloc_irq_vectors`, `request_irq`, handler, ACK |
| 5 | `05-dma` | `dma_set_mask`, `dma_alloc_coherent`, round-trip, IRQ completion |
| 6 | `06-userspace` | char device (`cdev`), `file_operations`, drive DMA from userspace |
