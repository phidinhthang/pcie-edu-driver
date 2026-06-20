# Chapter 6 — A character device: drive the hardware from userspace

## Where we are

Everything so far ran *inside* the kernel: `probe()` poked registers and kicked DMA on its own. But a
driver exists to serve **userspace** — that's the whole point of `/dev/sda`, `/dev/nvme0`, `/dev/net/tun`.
This finale turns our driver into something a normal program can use: it registers **`/dev/qemu_edu`**,
a 4 KB window onto the device's internal buffer.

```
echo "hi" > /dev/qemu_edu     # write(2)  -> copy_from_user -> DMA host->device
cat /dev/qemu_edu             # read(2)   -> DMA device->host -> copy_to_user
```

No new hardware concepts — the DMA engine and IRQ handler are **identical to Chapter 5**. What's new
is the *interface*: how a userspace `read`/`write` syscall reaches your driver, and how data crosses
the user/kernel boundary safely.

Files: `qemu_edu.c` (adds a `cdev`, `file_operations`, and `/dev` node), `Makefile` (unchanged).

---

## The idea: a device file is a syscall entry point

A character device node (`/dev/qemu_edu`) is a filesystem name bound to a **(major, minor)** number.
The major selects *which driver*; the minor selects *which instance*. When a process calls
`open`/`read`/`write`/`close` on that node, the kernel routes the syscall to the matching function in
your driver's **`struct file_operations`**. That's the entire contract: you fill in the callbacks, the
kernel wires up the syscalls.

So "writing a char driver" is really three jobs:

1. **Get a device number** — `alloc_chrdev_region()` reserves a free `(major,minor)` dynamically (the
   modern way; hardcoded majors are legacy).
2. **Bind your callbacks to it** — `cdev_init()` attaches your `file_operations`; `cdev_add()` makes
   it **live** (opens can arrive the instant this returns).
3. **Create the `/dev` node** — `class_create()` + `device_create()` register with the driver model
   so **udev** automatically makes `/dev/qemu_edu` appear (and removes it on unload). Without this
   you'd have to `mknod` by hand.

### The user/kernel boundary: `copy_to_user` / `copy_from_user`

This is the concept to really absorb. A userspace pointer is **not** a kernel pointer:

- it refers to a *different address space* (the calling process's), which the kernel isn't currently
  running in the same way;
- it might be **invalid, unmapped, or swapped out** — a malicious or buggy program can pass any
  value;
- dereferencing it directly (`memcpy`) would be a security hole and can **oops the kernel**.

So the kernel never touches user memory directly. It uses **`copy_from_user(dst_kernel, src_user, n)`**
and **`copy_to_user(dst_user, src_kernel, n)`**, which validate the user address, handle page faults,
and (return value) report how many bytes they *couldn't* copy — `0` means full success. Every byte
that crosses the boundary goes through these. Our `write()` does `copy_from_user` into the DMA buffer;
our `read()` does `copy_to_user` out of it. That's the gateway between a `cat` process and PCIe DMA.

### Concurrency: one engine, so serialize

The device has **one** DMA engine and **one** scratch buffer. If two processes wrote at once, their
transfers and `data_len` bookkeeping would interleave and corrupt each other. A **mutex** around the
read/write bodies makes each operation atomic with respect to the others. (We use
`mutex_lock_interruptible` so a blocked process can still be killed with Ctrl-C.) Real multi-queue
drivers use finer locking, but the principle — *protect shared device state* — is universal.

---

## The mechanism in code

`edu_open` recovers our `struct edu_drv` from the inode with **`container_of(inode->i_cdev, struct
edu_drv, cdev)`** — because we embedded the `cdev` *inside* `edu`, we can walk back from it to the
whole struct, with no global variable. It stashes that pointer in `file->private_data`, so `read`/
`write` find their device for free.

- **`edu_write`** clamps to 4 KB, `copy_from_user` into `dma_buf`, then `edu_dma_xfer(..., to_ram=false)`
  (host→device). It records `data_len` (how much is now valid) and returns the byte count.
- **`edu_read`** uses `*ppos`: on the first call it DMAs the device buffer back (`to_ram=true`) and
  returns the bytes; once the reader has consumed `data_len`, it returns `0` (**EOF**) so `cat`
  terminates instead of looping forever.

Model choice: each `write()` **replaces** the buffer (it doesn't append), and `read()` returns exactly
what was last written. Simple and predictable for a 4 KB scratchpad.

Read `qemu_edu.c` — the char-device section (`edu_open` … `edu_fops`) is the new material; the DMA
helper and IRQ handler are Chapter 5 verbatim.

---

## Build & run it

In the guest (re-mount the share if you rebooted):

```bash
guest$ cd /mnt/host/chapters/06-userspace
guest$ make
guest$ sudo insmod ./qemu_edu.ko
guest$ sudo dmesg | tail -n 5
```

Expected — note the new node announcement (major number will vary):

```
qemu_edu 0000:00:03.0: qemu_edu: ID register = 0x010000ed -> OK
qemu_edu 0000:00:03.0: qemu_edu: MSI enabled, using IRQ 24
qemu_edu 0000:00:03.0: qemu_edu: /dev/qemu_edu ready (major 510, minor 0)
qemu_edu 0000:00:03.0: qemu_edu: probe complete
```

Confirm the node exists (a `c` for character device, and the matching major):

```bash
guest$ ls -l /dev/qemu_edu
# crw------- 1 root root 510, 0 ... /dev/qemu_edu
```

Now the payoff — **drive PCIe DMA from the shell.** Write goes host→device via DMA; read pulls it
back device→host via DMA (the node is root-owned, so use `sudo`):

```bash
guest$ echo "DMA from userspace!" | sudo tee /dev/qemu_edu
guest$ sudo cat /dev/qemu_edu
DMA from userspace!
```

Watch the two DMA directions register in the log:

```bash
guest$ sudo dmesg | tail -n 3
# qemu_edu ...: write 20 bytes -> DMA host->device
# qemu_edu ...: read -> DMA device->host 20 bytes
```

Prove it's really binary-faithful round-tripping (not just text):

```bash
guest$ printf 'ABC\x00\x01\x02\xff' | sudo tee /dev/qemu_edu >/dev/null
guest$ sudo cat /dev/qemu_edu | xxd
# 00000000: 4142 4300 0102 ff                        ABC....
```

Each `/proc/interrupts` count for `qemu_edu` rises by one per DMA (one per write, one per read):

```bash
guest$ grep qemu_edu /proc/interrupts
```

Unload (udev removes the node automatically):

```bash
guest$ sudo rmmod qemu_edu
guest$ ls -l /dev/qemu_edu        # now: No such file or directory
```

---

## ⏸ Pause & reflect

- **Why `copy_*_user` and not `memcpy`.** This is the boundary that makes the kernel trustworthy. The
  user pointer is untrusted and from another address space; `copy_from_user`/`copy_to_user` validate
  it, fault pages in, and never let a bad pointer crash the kernel. Using `memcpy` on a user pointer
  is a textbook vulnerability. Internalize: *kernel code never dereferences a user pointer directly.*
- **`cdev_add` makes you live — ordering matters.** The instant `cdev_add` returns, a process can
  `open` and `read`/`write`. So everything those paths touch (the DMA buffer, the mutex, the IRQ)
  must already be set up — which is why char-device registration is the **last** thing `probe` does,
  and the **first** thing `remove` tears down. Get this backwards and you race a userspace open
  against half-initialized state.
- **`container_of` instead of a global.** Embedding `struct cdev` inside `struct edu_drv` lets
  `open()` recover the device from the inode with zero globals — the same pattern that makes "one
  struct per device" scale to many devices. (Compare the Chapter 1 BDF point and Chapter 4's
  `dev_id`: every layer hands you a way back to *your* instance.)
- **EOF and `*ppos`.** `read()` returning `0` is the universal "no more data" signal; without the
  `*ppos`/`data_len` bound, `cat` would re-read the same bytes forever. File position is how a stream
  of `read()` calls knows where it is — even for a "device," the file abstraction still applies.
- **The `.owner = THIS_MODULE` refcount.** While any process holds `/dev/qemu_edu` open, the module's
  refcount is nonzero and `rmmod` fails with *"Module is in use."* That's what prevents pulling the
  driver out from under a live `read()`. Try it: `sleep 100 < /dev/qemu_edu &` then `sudo rmmod
  qemu_edu` — it'll refuse until you kill the sleeper.
- **This is the real shape of a device driver.** Step back and look at the whole stack you built:
  **enumerate** (Ch1) → **MMIO control** (Ch2–3) → **interrupts** (Ch4) → **DMA data movement** (Ch5)
  → **userspace interface** (Ch6). Swap the edu device for your FPGA PCIe core and the skeleton is the
  same; what changes is the register map and the DMA descriptor format. You now have the end-to-end
  mental model.

Experiment worth doing: open the node from a tiny C program (`open("/dev/qemu_edu", O_RDWR)`, `write`,
`lseek(fd,0,SEEK_SET)`, `read`) to feel the syscalls directly instead of through the shell — it's the
same path `cat` takes, and it's how a real userspace library (think a DPDK-style control plane, or your
trading app's device shim) would talk to the driver.

---

## Where to go from here (beyond this course)

You've built a complete, if minimal, PCIe DMA driver. The natural next steps toward production /
real-FPGA work:

- **`mmap()`** the DMA buffer into userspace so a process reads/writes it with **zero copy** — no
  `copy_*_user` at all, the hot path for low-latency I/O (and conceptually how DPDK/RDMA bypass the
  kernel). This is the single biggest latency win and the most natural follow-on for your HFT angle.
- **Streaming DMA + descriptor rings** — map user pages directly (`dma_map_sg`/`pin_user_pages`) and
  feed a ring of descriptors, the way XDMA/NVMe/NICs sustain many in-flight transfers with coalesced
  interrupts. (Ch5's engine was the unit cell; this is the assembly line.)
- **Threaded IRQ / NAPI-style bottom halves** for heavy completion work off the hard-IRQ path.
- **Multiple devices & robust minors** — an `xarray`/`idr` of instances, per-device minors.
- **The AWS F1/F2 XDMA shell** — its driver exposes exactly these primitives (BARs, MSI-X, descriptor
  DMA, char/`mmap` interfaces); you now have the vocabulary to read and use it.

That's the course. From `lspci` showing an unknown `1234:11e8` to a `/dev` node that DMAs data through
the device on a `cat` — every layer of a real PCIe driver, built and understood one concept at a time.
