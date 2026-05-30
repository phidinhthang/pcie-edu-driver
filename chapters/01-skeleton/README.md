# Chapter 1 — The Skeleton: make `probe` fire

## Where we are

Chapter 0 left us with a guest that has the `edu` device (`1234:11e8`), a toolchain, and the repo
shared at `/mnt/host`. Now we write the **smallest real driver**: one that does nothing to the
hardware, but gets the kernel to recognize it, match it to the device, and call our code. When
`dmesg` prints our `probe!` line, three things are proven at once — enumeration, ID matching, and
the whole build→load→log pipeline. Every later chapter is "add code inside `probe`."

Files in this chapter:
- `qemu_edu.c` — the driver (heavily commented; read it alongside this).
- `Makefile` — builds `qemu_edu.ko` against the guest kernel.

---

## The idea: how Linux connects a *driver* to a *device*

Linux models hardware as a **bus / device / driver** triangle:

- The **bus** (here, PCI) enumerates what's physically present. Chapter 0's `lspci` was the bus's
  inventory: device `1234:11e8` at `00:03.0`.
- A **driver** registers with that bus and says *"I can handle devices with these IDs"* — a small
  table of vendor/device pairs.
- The bus core is the **matchmaker**: for every device, it looks for a driver whose table matches,
  and when it finds one, it calls that driver's **`probe(device)`** function — "here's a device
  that's yours, set it up." When the device leaves (or the driver unloads), it calls **`remove`**.

You never call `probe` yourself. You *register* and the core calls *you*. This inversion (the
framework calls your callbacks) is the shape of nearly all kernel driver code.

So the smallest driver is just: a match table + a `probe` + a `remove` + the registration glue.

---

## The mechanism: the four pieces in `qemu_edu.c`

1. **The ID table** — `struct pci_device_id edu_ids[]`, built with `PCI_DEVICE(0x1234, 0x11e8)`,
   ending in a `{ 0, }` sentinel. This is the "I handle these" list.
2. **`MODULE_DEVICE_TABLE(pci, edu_ids)`** — copies those IDs into the module's metadata so the
   system *could* auto-load us when the device appears. (We'll load manually, but it's standard.)
3. **`probe` / `remove`** — our callbacks. For now `probe` logs and returns `0` (= "I claim it");
   `remove` logs. Returning `0` from probe is the driver telling the core *"successfully bound."*
4. **`module_pci_driver(edu_driver)`** — one macro that generates the init/exit functions which
   `pci_register_driver()` on insmod and `pci_unregister_driver()` on rmmod. Registering makes the
   core immediately scan existing devices and call `probe` for matches — which is why `probe`
   fires the instant we `insmod`, with no hotplugging.

Open `qemu_edu.c` now and read the comments top to bottom; they expand on each of these.

---

## Build & run it

All of this runs **in the guest**. If you rebooted since Chapter 0, re-mount the share first:

```bash
guest# sudo mount -t 9p -o trans=virtio,version=9p2000.L eduhost /mnt/host   # if not mounted
```

Go to this chapter and build:

```bash
guest$ cd /mnt/host/chapters/01-skeleton
guest$ make
```

Expected (versions/paths may differ slightly):

```
make -C /lib/modules/6.8.0-117-generic/build M=/mnt/host/chapters/01-skeleton modules
  CC [M]  /mnt/host/chapters/01-skeleton/qemu_edu.o
  MODPOST /mnt/host/chapters/01-skeleton/Module.symvers
  CC [M]  /mnt/host/chapters/01-skeleton/qemu_edu.mod.o
  LD [M]  /mnt/host/chapters/01-skeleton/qemu_edu.ko
```

You now have `qemu_edu.ko`. Inspect its metadata (proves the IDs are embedded):

```bash
guest$ modinfo ./qemu_edu.ko
```

Expected: among the fields, `alias: pci:v00001234d000011E8sv*sd*bc*sc*i*` — that's our `1234:11e8`
match, encoded.

Load it, then look at the log:

```bash
guest$ sudo insmod ./qemu_edu.ko
guest$ sudo dmesg | tail -n 5
```

Expected — the payoff line:

```
[  ... ] qemu_edu 0000:00:03.0: qemu_edu: probe! claimed device 1234:11e8
```

Confirm the binding the kernel just made:

```bash
guest$ ls -l /sys/bus/pci/drivers/qemu_edu/
```

Expected: a symlink named `0000:00:03.0` (the device's BDF) — the device is now **bound to our
driver**.

Unload and watch `remove` fire:

```bash
guest$ sudo rmmod qemu_edu
guest$ sudo dmesg | tail -n 2
```

Expected:

```
[  ... ] qemu_edu 0000:00:03.0: qemu_edu: remove
```

That round-trip — `probe` on load, `remove` on unload — is the module lifecycle you'll repeat all
course.

---

## ⏸ Pause & reflect

Things newcomers trip on right here:

- **`probe` ≠ `module_init`.** `insmod` runs the module's init (which `module_pci_driver` generated
  for us — it just registers the driver). `probe` is then called *by the PCI core*, once per
  matching device. One init call; zero-or-more probe calls. If no `edu` device were present,
  `insmod` would still succeed but `probe` would never run. (Try it as a thought experiment: a
  driver for absent hardware loads fine and just sits there.)
- **What does "claim" mean?** After `probe` returns 0, the device is *bound* to your driver — you
  see the symlink under `/sys/bus/pci/drivers/qemu_edu/`. Another driver can't also claim it. This
  is why returning the right value from `probe` matters: `0` = mine, negative errno = hands off.
- **Why `dev_info` not `printk`.** `dev_info(&pdev->dev, ...)` auto-tags the line with the device
  (`qemu_edu 0000:00:03.0:`), so in a busy log you know *which* device spoke. Bare `printk` loses
  that. Use `dev_*` whenever you have a device.
- **Why `MODULE_LICENSE("GPL")`.** Many kernel functions are exported "GPL-only." Without this line
  the module may fail to load ("module verification failed" / tainted kernel) once we start calling
  such functions. Declare it now.
- **Common error: `insmod: Operation not permitted` or `Invalid module format`.** The first is
  missing `sudo`; the second means the `.ko` was built against a *different* kernel than the one
  running — rebuild in the guest against `$(uname -r)` (the whole reason we build in the guest).
- **We touched zero hardware.** We haven't read a single register. Matching and binding happen
  purely from config-space IDs the bus already read. Talking to the device starts in Chapter 2.

A question to make sure it landed: *if you `insmod` twice without `rmmod` in between, what happens?*
(Answer: the second fails with "File exists" — a module name can only be loaded once. `rmmod`
first.)

---

## What's next

The kernel hands us the device but we haven't spoken to it. **Chapter 2** enables the device, maps
**BAR0** into kernel memory, and reads the **identification register** at offset `0x00` (expecting
the `~0x010000ed` magic), then exercises the liveness register (write X, read `~X`) — our first
real MMIO conversation with the hardware.
