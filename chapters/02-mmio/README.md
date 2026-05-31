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

### These registers are device *logic*, not memory

A crucial point that's easy to miss: the BAR0 window is **not RAM**. We never store `0x010000ed`
anywhere — the *device* produces it. Every offset is decoded by the device's own logic, and a read
or write is a **transaction the device interprets however it likes**:

- **ID register (`0x00`)** is hardwired to return the constant `0x010000ed` on any read; writes are
  ignored. (A read-only identity/version register.)
- **Liveness (`0x04`)** *stores* the value you write, but its **read path returns the bitwise
  complement**. Read logic differs from write logic — something plain memory can never do.

If you've written RTL, this is exactly the register-decode behind an AXI-Lite (or similar)
interface — a read decode and a write decode:

```
// the mental model (pseudo-HDL)
case (read_addr)
  0x00: rdata <= 32'h010000ed;   // hardwired ID constant
  0x04: rdata <= ~liveness_reg;  // read returns the complement
case (write_addr)
  0x04: liveness_reg <= wdata;   // store on write
```

QEMU's `edu` device (`hw/misc/edu.c`) is literally a C function doing that `case` on the offset —
a *software model* of the register-decode you'd otherwise synthesize. So MMIO is never "write a
value, read the same value back like memory"; it's "poke the device's logic and see what it does."
From Chapter 3 on, that logic gets richer (write N → the device computes N!).

But a kernel driver can't use a physical address directly — the CPU runs on **virtual** addresses
through the MMU. So there are four steps, and they're distinct on purpose:

1. **Enable** the device (`pci_enable_device`) — this is *mostly not* about power. Its
   operationally critical effect is flipping the **"Memory Space Enable" bit** in the device's PCI
   *command register* (in config space, separate from BAR0). Until that bit is set, **the device
   does not decode accesses to its BARs** — reads return all-ones (`0xffffffff`), writes vanish.
   (It also wakes the device from a low-power D3 state if needed — that's the power angle, but the
   decode-enable is what makes our register reads work.) A separate command bit, **bus mastering**
   (`pci_set_master`), lets the device initiate its *own* reads/writes to host RAM — needed for DMA
   in Chapter 5, not for poking registers, so we leave it off for now.
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

Why the accessors instead of `*reg = val;`? Because correct MMIO needs **two separate guarantees**,
provided by two different layers:

**Layer 1 — the mapping itself is uncached.** When `pci_iomap` maps BAR0, it marks those pages as
**device / uncacheable** (via page-table/PAT attributes). So the **CPU data cache never
participates** — every access goes out to the device, every time. There is no "second read returns
a cached copy," because the hardware is told *don't cache this region*. This is a property of *how
the region is mapped*, not of the accessor function.

**Layer 2 — the accessors stop the *compiler* and fix ordering/endianness.** Even with an uncached
mapping, a plain `*ptr` lets the **compiler** misbehave — cache the value in a register, hoist it
out of a loop, reorder it, or elide a "redundant" second read. `ioread32`/`iowrite32` additionally:

- **force a real access**, exactly once, in program order — the compiler can't optimize it away;
- insert **memory barriers** so MMIO accesses are correctly ordered w.r.t. each other and w.r.t.
  normal memory;
- handle **endianness** — little-endian device access converted to host byte order (a no-op on a
  little-endian host, but keeps the code portable).

Why this matters: **a read can have a side effect.** Reading a status register might clear a flag;
reading a FIFO data port might *pop* an entry — so reading twice ≠ reading once. (From the hardware
side: a read-to-clear interrupt register, or an AXI-Stream read port.) If the compiler eliminated
your "redundant" second read, you'd lose a FIFO word or miss an interrupt clear. edu's liveness
register is benign, but the machinery must assume the worst because real registers bite. So both
layers are load-bearing: **the mapping forbids CPU caching; the accessors forbid compiler
caching/reordering and handle ordering + endianness.**

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
