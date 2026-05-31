# Chapter 2 — MMIO: map BAR0 and talk to the device

## Where we are

Chapter 1's driver bound to the device but never touched it — matching happens purely from
config-space IDs the bus already read. Now we make our **first real hardware accesses**: enable the
device, map its **BAR0** register window into kernel memory, read the **identification register**
(it must return `0x010000ed`), and run the **liveness** check (write X, read back `~X`). When those
two lines print "OK" in `dmesg`, you've proven the full MMIO read *and* write path works.

This is the conceptual core of register-level driver work. Everything later (factorial, interrupts,
DMA) is just *which* registers we poke through this same mechanism.

Files: `qemu_edu.c` (grows from Ch1), `Makefile` (unchanged).

---

## The idea: what "MMIO through a BAR" really means

Recall from Chapter 0's `lspci`: `Memory at fea00000 ... [size=1M]`. The device asked for a 1 MB
window; firmware placed it at **physical address** `0xfea00000`. **Memory-Mapped I/O (MMIO)** means:
when the CPU reads/writes those physical addresses, the read/write is routed by the chipset to the
*device's registers* instead of to RAM. The device's "datasheet" (our register map) is just offsets
into that window: ID at `+0x00`, liveness at `+0x04`, and so on.

But a kernel driver can't use a physical address directly — the CPU runs on **virtual** addresses
through the MMU. So there are four steps, and they're distinct on purpose:

1. **Enable** the device (`pci_enable_device`) — turn it on and allow it to decode accesses to its
   BARs. A freshly-enumerated device may be in a low-power state with memory decoding off; reads
   would return garbage.
2. **Reserve** the region (`pci_request_region`) — claim *ownership* of BAR0's physical range so
   two drivers can't fight over it. This is bookkeeping (it shows up in `/proc/iomem`), not mapping.
3. **Map** it (`pci_iomap`) — ask the kernel for a **virtual address** that points at that physical
   MMIO window. Now we have a usable pointer.
4. **Access** it (`ioread32`/`iowrite32`) — perform individual register reads/writes through that
   pointer.

Separating "reserve" from "map" feels redundant at first, but they answer different questions:
reserve = *"is this range mine?"*, map = *"give me a pointer to reach it."*

---

## The mechanism: why not just dereference a pointer?

The mapping is typed `void __iomem *`. That `__iomem` tag is the kernel saying **"this is device
space, not memory — don't treat it like a normal pointer."** You must use the accessor functions:

- `ioread32(addr)` / `iowrite32(val, addr)` (and 8/16/64-bit variants).

Why the accessors instead of `*reg = val;`?

- **No compiler games.** A device register can change on its own and every access can have a side
  effect. The accessors ensure the access actually happens, once, exactly as written — the compiler
  can't cache, reorder, or elide it.
- **Ordering / barriers.** MMIO needs stronger ordering than RAM; the accessors insert the right
  barriers so writes reach the device in program order.
- **Endianness.** `ioread32`/`iowrite32` do little-endian device access and convert to host byte
  order. edu is little-endian; on a little-endian host it's a no-op, but the code stays portable.

So: `ioread32(edu->mmio + EDU_REG_ID)` = "do one 32-bit MMIO load from BAR0 offset 0x00." Pointer
arithmetic on `edu->mmio` is in **bytes**, and our offsets (`0x00`, `0x04`) are byte offsets — they
line up directly.

### Resource lifecycle & error unwinding

`probe` now acquires several resources in order (enable → request → map). If a later step fails, we
must release the earlier ones — leaking an enabled device or a reserved region is a real bug. The
canonical kernel idiom is **`goto` error labels that unwind in reverse**:

```
enable → request → map → (success)
                    └ fail → release ─┐
            └────── fail → disable ───┤  each label undoes one prior step
   └─────────────── fail → free ──────┘
```

`remove` does the same teardown for the success path, also in reverse order. Read how `qemu_edu.c`
implements both — the `err_*` labels mirror the setup steps one-to-one.

We also stash our `struct edu_drv` on the device with `pci_set_drvdata(pdev, edu)` so `remove`
(and, soon, the interrupt handler) can get it back with `pci_get_drvdata(pdev)`.

---

## Build & run it

In the guest (re-mount the share first if you rebooted):

```bash
guest$ cd /mnt/host/chapters/02-mmio
guest$ make
guest$ sudo insmod ./qemu_edu.ko
guest$ sudo dmesg | tail -n 6
```

Expected (addresses will differ):

```
qemu_edu 0000:00:03.0: qemu_edu: ID register = 0x010000ed (expect 0x010000ed) -> OK
qemu_edu 0000:00:03.0: qemu_edu: liveness wrote 0xdeadbeef, read 0x21524110 (~wrote=0x21524110) -> OK
qemu_edu 0000:00:03.0: qemu_edu: probe complete, BAR0 mapped at 00000000xxxxxxxx
```

(`0x21524110` is `~0xdeadbeef` — verify it by hand: `~0xdeadbeef = 0x21524110`. The device computed
that complement, which is how you know your *write* landed and your *read* came back.)

Peek at the ownership we registered, then unload:

```bash
guest$ sudo grep qemu_edu /proc/iomem
guest$ sudo rmmod qemu_edu
```

Expected from `/proc/iomem`: a line like `fea00000-feafffff : qemu_edu` — BAR0's physical range,
now tagged as ours (this is what `pci_request_region` did).

---

## ⏸ Pause & reflect

- **Physical vs. virtual address.** `0xfea00000` (from `lspci`) is *physical*. `pci_iomap` returns
  a *kernel virtual* address (the `%p` in the log) that the MMU routes to that physical window. They
  are different numbers naming the same registers from two viewpoints. Your driver only ever uses
  the virtual one.
- **Why the ID register is the canonical first read.** It's read-only with a known constant. If it
  reads `0x010000ed`, your enable+map+accessor chain is correct end-to-end. If it reads
  `0xffffffff`, that's the classic "all-ones" symptom — the device isn't decoding (forgot
  `pci_enable_device`?), wrong BAR, or wrong offset. All-ones is what the bus returns when nothing
  answers.
- **Liveness proves *write*, ID proves *read*.** A RO register can't confirm your writes work. The
  liveness register can: you write, the device transforms (`~X`) and stores, you read the transform
  back. Round-trip = both directions verified.
- **`__iomem` and the dreaded direct deref.** If you ever write `*(u32 *)edu->mmio` you might "get
  away with it" on x86, but it's wrong: no guaranteed barriers, the compiler may reorder/cache it,
  and `sparse` (the kernel's static checker) will flag the `__iomem` violation. Always use the
  accessors.
- **Reserve vs. map confusion.** `pci_request_region` does **not** give you a usable pointer — it
  only reserves. `pci_iomap` gives the pointer. You need both: one for ownership, one for access.
  (Skipping `request_region` often still "works," which is exactly why it's a trap — until two
  drivers collide.)
- **Cleanup ordering matters.** Unmap before releasing the region, disable last. Doing it out of
  order (e.g. disabling the device while a mapping is live) is how drivers cause oopses on unload.
  Reverse-of-acquire is the rule.

Check yourself: *what would the ID register read if you commented out `pci_enable_device`?* (Likely
`0xffffffff` — memory decoding off, nothing answers, bus returns all-ones. Try it if you're
curious; it's a safe, instructive failure.)

---

## What's next

We can read and write registers at will. **Chapter 3** uses that to drive the **factorial unit**:
write N to a command register, **poll** the status register's "computing" bit until the device
finishes, then read back N!. That's the command/status/poll pattern — the polled cousin of the
interrupt-driven completion we build in Chapter 4.
