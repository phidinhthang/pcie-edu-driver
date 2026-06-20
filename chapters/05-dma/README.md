# Chapter 5 — DMA: the device moves whole buffers by itself

## Where we are

Every byte we've moved so far crossed the bus through a **register**: one `ioread32`/`iowrite32` per
4 bytes, the CPU personally shuttling each word. That's the right tool for *control* (start this, read
that status) and the wrong tool for *bulk data* — at 4 bytes per CPU instruction you'll never feed a
10 GbE link or an NVMe drive.

**DMA (Direct Memory Access)** is the answer, and it's the whole reason PCIe DMA cores exist. We hand
the device a **host RAM address** and a length; the device's own engine reads or writes that memory
across the bus, no CPU per byte. The CPU just sets up the transfer and waits for the **completion
interrupt** — the very interrupt path you built in Chapter 4. This is the actual shape of a real
high-throughput driver (your FPGA core, AWS F1/F2 XDMA): *describe a buffer, kick the engine, wait for
the IRQ.*

Files: `qemu_edu.c` (adds a DMA buffer, a transfer helper, and a round-trip demo), `Makefile`
(unchanged). The interrupt handler is unchanged from Chapter 4 — DMA completion arrives through it.

---

## The idea: three addresses for one buffer

This is the concept that trips up everyone new to DMA, so let's nail it first. A single DMA buffer is
seen through **three different "addresses," and they are not the same number:**

- **CPU virtual address** — the pointer *your code* dereferences (`edu->dma_buf`). Lives in the
  kernel's virtual address space; meaningless to the device.
- **Physical address** — where those bytes actually sit in the DRAM chips.
- **Bus / DMA address** (`dma_addr_t`, our `edu->dma_handle`) — the number *the device* must put on
  the bus to reach that memory. On simple systems it equals the physical address; with an **IOMMU**
  in the path it's a *remapped* address the IOMMU translates back to physical. Either way: **this is
  the only address the device understands**, and it's what we program into the DMA registers.

So the rule is: **CPU touches the buffer via the CPU pointer; the device touches it via the
`dma_addr_t`.** Never hand a device a CPU pointer (`virt_to_phys` of a kmalloc'd pointer is *not* a
valid bus address in general) — always get the bus address from the DMA API. `dma_alloc_coherent`
hands you *both* views in one call, which is exactly why we use it.

### Coherent vs streaming DMA

There are two flavors of DMA mapping, and picking the right one is a real driver decision:

- **Coherent (consistent)** — `dma_alloc_coherent()`. The buffer is kept coherent between CPU and
  device automatically (typically mapped uncached, or on cache-coherent hardware just guaranteed
  consistent). You read/write it any time without manual cache management. Best for **long-lived,
  bidirectional** buffers: descriptor rings, control structures, small scratch buffers. We use this
  because it keeps the chapter focused on the transfer, not on cache flushing.
- **Streaming** — `dma_map_single()` / `dma_map_sg()`. You map an *existing* buffer for one transfer
  in one direction, then unmap it; you must `dma_sync_*` around CPU access. Lower overhead for
  **one-shot, high-throughput** transfers of buffers you didn't allocate (e.g. a userspace page, a
  network packet). This is what a production fast-path usually uses — we'll meet it conceptually in
  Chapter 6.

### The DMA mask — "how wide are your address pins?"

A device can only drive as many address bits as it has. Our edu engine decodes a **28-bit** address
space (256 MB). If the kernel handed it a buffer above 256 MB, the device would silently truncate the
address and scribble on the wrong memory. `dma_set_mask_and_coherent(dev, DMA_BIT_MASK(28))` is our
**promise** to the DMA layer — "never give this device an address wider than 28 bits" — so the
allocator places our buffer somewhere reachable. Real cards do the same with 32- or 64-bit masks;
getting the mask wrong is a classic source of memory corruption.

---

## The mechanism: program four registers, then wait

The edu DMA engine has four 8-byte registers (note: **64-bit**, hence `writeq`, because real DMA
addresses can be 64-bit):

| Register | Offset | Meaning |
|----------|--------|---------|
| `EDU_REG_DMA_SRC`   | `0x80` | source address |
| `EDU_REG_DMA_DST`   | `0x88` | destination address |
| `EDU_REG_DMA_COUNT` | `0x90` | byte count |
| `EDU_REG_DMA_CMD`   | `0x98` | `bit0`=start, `bit1`=direction, `bit2`=IRQ-on-done |

One side of the transfer is **host RAM** (our `dma_handle`); the other is the device's **internal
buffer**, which lives at device-side addresses `[0x40000, 0x40000+0x1000)`:

- **host → device** (`EDU_DMA_CMD_TO_RAM` clear): `SRC = dma_handle`, `DST = 0x40000`.
- **device → host** (`EDU_DMA_CMD_TO_RAM` set): `SRC = 0x40000`, `DST = dma_handle`.

The sequence (in `edu_dma_xfer`):

```
1. reinit_completion()                  # arm the handshake
2. write SRC, DST, COUNT                 # describe the transfer
3. write CMD = START | IRQ [| TO_RAM]    # GO  <-- writing CMD triggers the engine
4. wait_for_completion_timeout()         # sleep; the CPU is free
   ...engine copies across the bus, then raises EDU_IRQ_DMA (0x100)...
   ...our handler ACKs and complete()s; we wake up...
5. return
```

**Why CMD is written last:** the START bit *is* the trigger. SRC/DST/COUNT must already be latched
when it lands. The kernel's MMIO accessors to one device are ordered, so writing CMD last is
sufficient — no explicit barrier needed here.

### The round-trip demo (proving the bytes really travelled)

`edu_dma_roundtrip` does something deliberately convincing:

1. write a known string into the DMA buffer (CPU, via the CPU pointer),
2. DMA it **into** the device's internal buffer,
3. **wipe our copy** (`memset` to zero) — so any bytes we see later *cannot* be local leftovers,
4. DMA the device buffer **back** into host RAM,
5. `memcmp` — it matches the original, so the string genuinely lived inside the device and came home.

Step 3 is the trick that turns "looks like it worked" into proof.

Read `qemu_edu.c` now — `edu_dma_xfer` maps onto the sequence above, `edu_dma_roundtrip` onto the
five steps, and `edu_irq_handler` is untouched from Chapter 4.

---

## Build & run it

In the guest (re-mount the share if you rebooted):

```bash
guest$ cd /mnt/host/chapters/05-dma
guest$ make
guest$ sudo insmod ./qemu_edu.ko
guest$ sudo dmesg | tail -n 8
```

Expected (the `cpu=` pointer and `bus=` address will differ on your run; `bus` is small because of
the 28-bit mask):

```
qemu_edu 0000:00:03.0: qemu_edu: ID register = 0x010000ed -> OK
qemu_edu 0000:00:03.0: qemu_edu: MSI enabled, using IRQ 24
qemu_edu 0000:00:03.0: qemu_edu: DMA buffer: cpu=00000000xxxxxxxx  bus=0x0000000004001000  size=4096
qemu_edu 0000:00:03.0: qemu_edu: DMA host->device done (INT_STATUS=0x00000100)
qemu_edu 0000:00:03.0: qemu_edu: DMA device->host done (INT_STATUS=0x00000100)
qemu_edu 0000:00:03.0: qemu_edu: DMA round-trip OK -> "Hello from host RAM, round-tripped through the edu device via DMA!"
qemu_edu 0000:00:03.0: qemu_edu: probe complete
```

`INT_STATUS=0x00000100` is `EDU_IRQ_DMA` — the device telling us *"DMA finished,"* through the same
handler that signalled the factorial in Chapter 4. Confirm the interrupts were really counted (two
transfers = two IRQs), then unload:

```bash
guest$ grep qemu_edu /proc/interrupts
guest$ sudo rmmod qemu_edu
```

---

## ⏸ Pause & reflect

- **`dma_addr_t` is not a CPU pointer.** The single most common DMA bug is handing the device a CPU
  virtual address (or a `kmalloc` pointer) instead of the `dma_addr_t` from the DMA API. The CPU
  pointer and the bus address point at the *same bytes* but are *different numbers*, and only the
  DMA API knows the translation (especially with an IOMMU). CPU uses `dma_buf`; device uses
  `dma_handle`. Keep them straight and DMA is easy.
- **Why we never needed a cache flush.** `dma_alloc_coherent` gives **coherent** memory, so the CPU's
  write in step 1 is visible to the device, and the device's write in step 4 is visible to the CPU,
  with no `dma_sync_*`. With *streaming* mappings you'd have to sync explicitly around each access —
  forgetting it gives stale-data bugs that only show on real (non-coherent) hardware. Know which kind
  you're holding.
- **The IOMMU is your seatbelt.** A bus-mastering device can read/write *any* memory its address bits
  reach — a buggy or malicious device is a security problem. An **IOMMU** (Intel VT-d / AMD-Vi /
  ARM SMMU) restricts a device to only the addresses you mapped for it, and is why the bus address
  can differ from the physical one. On real systems you'll see DMA failures if a buffer isn't
  properly mapped through it. (This is also why "just use `virt_to_phys`" is wrong on IOMMU systems.)
- **The interrupt path paid off.** Notice we wrote *zero* new interrupt code — DMA completion reused
  Chapter 4's handler and completion verbatim. That's the real architecture of a fast device:
  **MMIO to configure, DMA to move data, MSI to signal done.** You now have all three.
- **Address ordering / the trigger.** Writing CMD last isn't stylistic — START kicks the engine, so
  the other registers must be set first. On some buses you'd add a barrier; on PCIe MMIO to one
  device the accessors are already ordered. (When in doubt, the kernel gives `wmb()`/`dma_wmb()`.)
- **The 28-bit mask is a real constraint, not a formality.** Comment out the
  `dma_set_mask_and_coherent` call and you *might* still work by luck (the allocator often returns low
  memory anyway) — but on a machine with lots of RAM you'd eventually get a high buffer, the device
  would truncate the address to 28 bits, and the DMA would corrupt unrelated memory. Declaring the
  mask is how you avoid betting on luck.

**A connection you'll appreciate (FPGA / XDMA):** our edu engine does **one** transfer per kick —
program SRC/DST/COUNT, set START. Real high-throughput cores (Xilinx/AWS XDMA, NVMe, NICs) replace
that with a **descriptor ring**: you write an array of `{src, dst, len, flags}` descriptors into a
DMA-coherent buffer, tell the engine "here's the ring, here's the new tail," and it chews through
many transfers back-to-back, raising one interrupt per batch (or coalesced). That's strictly the same
three ingredients — a coherent buffer, bus addresses, a completion IRQ — scaled up. You've now built
the unit cell; production is that cell in a ring with a doorbell.

Experiment worth doing: change `len` to push only part of the buffer, or DMA into a non-zero offset
of the device buffer (`EDU_DMA_BUF_BASE + 256`) and back, and confirm the round-trip still matches —
it makes the "device-side addresses" concrete.

---

## What's next

We've driven DMA entirely from `probe` (kernel-internal). **Chapter 6** opens it to **userspace**: we
register a **character device** (`cdev` + `file_operations`) so a normal program can `write()` bytes
that the driver DMAs into the device and `read()` them back — turning this driver into something a
user can actually *use*, the way `/dev/...` nodes expose real hardware.
