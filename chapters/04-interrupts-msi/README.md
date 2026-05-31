# Chapter 4 — MSI Interrupts: let the device notify us

## Where we are

Chapter 3 worked, but the CPU **busy-waited**: it sat in a loop reading a status bit, doing nothing
useful, until the device finished. That's fine for sub-microsecond ops and terrible for anything
slower. This chapter inverts the model: we tell the device *"raise an interrupt when you're done,"*
register a handler, and the CPU is **free to do other work** until the device signals. When it does,
the kernel jumps into our handler, we ACK the device, and we wake whoever was waiting.

This is the densest chapter so far — it introduces **interrupt context** (code that runs at any
moment, can't sleep) and the **completion handshake** between handler and waiter. Take it slowly.

Files: `qemu_edu.c` (adds an IRQ handler, MSI setup, a doorbell self-test, and an interrupt-driven
factorial), `Makefile` (unchanged).

---

## The idea: what an interrupt is, and why MSI

An **interrupt** is the hardware's way of saying "stop what you're doing and run this code now." The
CPU is executing normal code; the device asserts an interrupt; the CPU saves its place, jumps to the
registered **interrupt handler**, runs it, then resumes. The device drove the CPU's attention —
no polling.

There are two delivery styles, and the difference matters:

- **Legacy INTx** — the old way: a physical interrupt *wire* (there are only 4: INTA–INTD) shared by
  many devices. When it asserts, the kernel must ask *every* device on that line "was it you?" That
  sharing causes latency, ambiguity, and races. This is the `IRQ 11` you saw in Chapter 0's `lspci`.
- **MSI (Message Signaled Interrupts)** — the modern way: the device signals an interrupt by
  performing a **memory write** to a special address the OS gave it. The chipset turns that write
  into a CPU interrupt. No shared wire: each device (each *vector*) gets its own dedicated interrupt,
  so there's no "which device?" demux, far less sharing, and better scaling.

Recall Chapter 0: `lspci` showed `MSI: Enable- Count=1/1` — the edu device *advertises* MSI but it's
**disabled**. This chapter flips it to `Enable+`.

> **Key consequence of "MSI is a memory write":** the device has to *initiate* a write onto the bus,
> i.e. act as a **bus master**. That's why we call `pci_set_master()` before enabling MSI. (It's the
> same capability DMA needs in Chapter 5 — a device reaching out to memory on its own.) This refines
> the Chapter 2 note that bus mastering was "for DMA": MSI needs it too, for the same reason.

---

## The mechanism: five moving parts

### 1. `pci_set_master(pdev)`
Sets the Bus Master Enable bit so the device may issue upstream memory writes — required for MSI
delivery (and DMA).

### 2. `pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI)`
Asks for between `min=1` and `max=1` MSI vectors. This flips the device's MSI-Enable bit on,
programs the MSI address/data, and binds the vector to a **Linux IRQ number**. Returns how many
vectors it got (here 1) or a negative errno. (`PCI_IRQ_MSI | PCI_IRQ_MSIX | PCI_IRQ_INTX` lets the
kernel pick the best available; we force MSI to keep it concrete.)

### 3. `pci_irq_vector(pdev, 0)` → `request_irq(...)`
`pci_irq_vector` translates "MSI vector 0" into the kernel IRQ number. `request_irq(irq, handler,
flags, name, dev_id)` registers our handler for it:
- `flags = 0` — MSI isn't shared, so no `IRQF_SHARED`.
- `name = "qemu_edu"` — appears in `/proc/interrupts`.
- `dev_id = edu` — an opaque cookie the kernel hands back to the handler every time it fires. This
  is how the handler finds its device state (the handler can't take arguments otherwise).

### 4. The handler — runs in *interrupt context*
```
device interrupts -> kernel calls edu_irq_handler(irq, edu)
  read INT_STATUS  (why did it interrupt?)
  write INT_ACK    (clear it / de-assert — DON'T FORGET THIS)
  record + complete(&irq_done)   (wake the waiter)
  return IRQ_HANDLED
```
Interrupt context has hard rules: **it must not sleep** — no `GFP_KERNEL` allocation, no mutex, no
`wait_for_completion`, nothing blocking. It must be short. It returns `IRQ_HANDLED` ("it was mine")
or `IRQ_NONE` ("not mine," important on shared lines).

### 5. `struct completion` — the handshake
The waiter (in `probe`, process context) needs to block until the handler fires. A `completion` is
exactly that primitive:
- waiter: `reinit_completion()` then `wait_for_completion_timeout()` → sleeps.
- handler: `complete()` → wakes the waiter. `complete()` is irq-safe.

Crucially it's **race-free**: if the interrupt fires *before* the waiter reaches
`wait_for_completion`, `complete()` records it and the wait returns immediately. A naive
"set a flag, then sleep" would miss that early wake-up. (This is the subtlety that makes completions
the right tool instead of a hand-rolled flag.)

### Telling the device to interrupt
- **Doorbell test:** write any value to `EDU_REG_INT_RAISE` (`0x60`) → the device raises an
  interrupt immediately and sets `INT_STATUS` to that value. Great for testing the path in
  isolation.
- **Factorial completion:** set `EDU_STATUS_IRQ_ENABLE` (bit 7) in `STATUS`, then write N. When the
  worker finishes, it raises an interrupt with `INT_STATUS` bit `EDU_IRQ_FACT` (0x1).

Read `qemu_edu.c` now — `edu_irq_handler`, `edu_irq_selftest`, and `edu_factorial_irq` map directly
onto parts 4, the doorbell, and 5.

---

## Build & run it

In the guest (re-mount the share if you rebooted):

```bash
guest$ cd /mnt/host/chapters/04-interrupts-msi
guest$ make
guest$ sudo insmod ./qemu_edu.ko
guest$ sudo dmesg | tail -n 8
```

Expected:

```
qemu_edu 0000:00:03.0: qemu_edu: ID register = 0x010000ed -> OK
qemu_edu 0000:00:03.0: qemu_edu: MSI enabled, using IRQ 24
qemu_edu 0000:00:03.0: qemu_edu: IRQ self-test OK, handler saw INT_STATUS=0x00010000
qemu_edu 0000:00:03.0: qemu_edu: factorial( 5) = 120        (via interrupt)
qemu_edu 0000:00:03.0: qemu_edu: factorial(10) = 3628800    (via interrupt)
qemu_edu 0000:00:03.0: qemu_edu: factorial(12) = 479001600  (via interrupt)
qemu_edu 0000:00:03.0: qemu_edu: probe complete
```

(The IRQ number — `24` here — will vary.) `INT_STATUS=0x00010000` is exactly our `EDU_IRQ_TEST`
value coming back through the handler: the doorbell rang and we heard it.

Now *see* the interrupts the kernel counted, and that MSI is on:

```bash
guest$ grep qemu_edu /proc/interrupts
guest$ sudo lspci -v -d 1234:11e8 | grep MSI
```

Expected: a `/proc/interrupts` line naming `qemu_edu` with a small nonzero count (our self-test +
3 factorials = 4), and `MSI: Enable+` (was `Enable-` in Chapter 0 — we turned it on). Then unload:

```bash
guest$ sudo rmmod qemu_edu
```

---

## ⏸ Pause & reflect

- **Interrupt context — the rule that bites everyone.** The handler can be invoked at *any* instant,
  even mid-`printk` of other code. It **cannot sleep**. If you call something that might block
  (`kmalloc(GFP_KERNEL)`, a mutex, `msleep`, `wait_for_completion`) inside it, you can deadlock or
  panic the kernel. Keep handlers tiny: read why, ACK, wake someone, return. Heavy work goes to a
  **bottom half** (workqueue/threaded IRQ) — a concept for later, but know the term.
- **Why ACK matters.** The handler writes `INT_STATUS` back to `INT_ACK` to clear the device's
  pending state. For a *level-triggered* legacy interrupt, forgetting to ACK means the line stays
  asserted and the handler fires again and again — an **interrupt storm** that locks the machine.
  MSI is edge-like so it's less catastrophic, but you still must clear device state or you'll
  mis-read the next interrupt. ACK is not optional.
- **`IRQ_HANDLED` vs `IRQ_NONE`.** Returning `IRQ_NONE` says "wasn't mine." On a shared line the
  kernel uses this to poll other handlers; if *everyone* returns `IRQ_NONE` repeatedly, the kernel
  disables the line ("nobody cared") to protect the system. With dedicated MSI it's almost always
  ours, but returning the honest value is the contract.
- **The completion is the race fix.** Convince yourself: the device could finish *the instant* after
  `iowrite32(n)` and fire the IRQ before we call `wait_for_completion_timeout`. With a completion,
  `complete()` already happened, so the wait returns at once — no lost wake-up. With a bare
  `bool done; while(!done);` you'd be back to polling; with `if(!done) sleep();` you'd race.
- **`dev_id` is how the handler finds its data.** We passed `edu`; the kernel returns it as the
  handler's `void *dev_id`. With two edu devices, each `request_irq` passes its own `edu`, so each
  handler gets the right state. (Same "one struct per device" principle as Chapter 1's BDF point.)
- **Teardown order.** `free_irq()` *before* `pci_iounmap()` — it guarantees the handler isn't
  running (and won't start) once it returns, so unmapping the registers it touches is then safe.
  Unmap-then-free would risk the handler dereferencing a freed mapping. We also disable the device's
  factorial-IRQ bit first, so nothing is pending.

**A connection you'll appreciate (latency/HFT):** interrupts free the CPU, but they cost a
**context switch** — save state, jump, handler, restore — typically hundreds of nanoseconds to
microseconds of latency and jitter. This is exactly why ultra-low-latency stacks (DPDK, kernel
NAPI's busy-poll, `io_uring` polled mode) often go *back* to **polling** for the hottest path: when
you're going to consume the result immediately anyway, spinning beats eating interrupt latency. So
Chapters 3 and 4 aren't "wrong vs right" — they're a **latency-vs-CPU-efficiency tradeoff** you pick
per workload. You already live on the polling side of this; now you've built both.

Experiment worth doing: **comment out the `iowrite32(status, edu->mmio + EDU_REG_INT_ACK)` line** in
the handler, rebuild, reload. Predict what happens, then check `dmesg` / `grep qemu_edu
/proc/interrupts`. (On this MSI setup you likely still get one delivery per event but the device's
pending bit never clears, so subsequent reads of `INT_STATUS` look wrong — a controlled way to feel
why ACK exists.)

---

## What's next

The device can now *notify* us. **Chapter 5** is the main event: **DMA**. We set the device's 28-bit
DMA mask, allocate a DMA-coherent buffer, hand the device a host RAM address, and have it copy data
between host RAM and its internal buffer — with **completion delivered by the very interrupt
mechanism we just built**. That's the full shape of a real high-throughput device driver.
