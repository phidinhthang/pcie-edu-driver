# Chapter 0 — The Environment: a QEMU guest that has the `edu` device

## Where we are

Nothing built yet. The job of this chapter is to get a **Linux guest VM** running with QEMU's
`edu` PCI device attached, and to **prove the device is there** (`lspci` shows `1234:11e8`). Once
that one fact is on screen, every later chapter has a place to `insmod` a driver and watch
`dmesg`. This is plumbing, but it's the plumbing all the real learning rests on.

By the end you will have:
- a guest Ubuntu VM you can boot in a few seconds with one script,
- the `edu` device visible inside it,
- a compiler + kernel headers in the guest (so we can build modules),
- the host repo shared into the guest (so we edit on the host, build in the guest).

---

## The idea: why a *guest VM* at all?

You're running on **WSL2**, which is itself a lightweight VM that Microsoft manages. You cannot
bolt an arbitrary virtual PCI device onto WSL2's own kernel, and building out-of-tree kernel
modules against that managed kernel is awkward. So instead we run **our own VM inside WSL2** using
QEMU, and attach the `edu` device to *that* guest:

```
WSL2 Ubuntu (host)         <- you are here: edit code, run QEMU, run git
  └─ QEMU  (-device edu)    <- boots a guest Linux that HAS the edu device
       └─ Guest Ubuntu      <- here we insmod the driver and read dmesg
```

Bonus: if a buggy driver panics the guest kernel, you just reboot the guest. The host is never at
risk. That safety is exactly why driver developers work in VMs.

> The `edu` device itself is **built into QEMU** — there is no repo to clone to *use* it. (You'd
> clone QEMU source only to *read* `hw/misc/edu.c`, which is optional.) The only code we author in
> this whole course is the driver.

---

## Step 0 — Confirm the host already has what it needs

QEMU is already installed on this machine, but let's make it reproducible for anyone starting
fresh. Run:

```bash
host$ qemu-system-x86_64 --version
host$ qemu-img --version
host$ ls -l /dev/kvm
```

Expected: a QEMU version line (we have **8.2.2**), a `qemu-img` version, and `/dev/kvm` existing.
`/dev/kvm` means hardware acceleration is available, so the guest will be fast.

Now install the few helper packages (idempotent — safe to re-run):

```bash
host$ sudo apt update
host$ sudo apt install -y qemu-system-x86 qemu-utils cloud-image-utils wget
```

- `qemu-system-x86` — QEMU itself (ships the `edu` device).
- `qemu-utils` — `qemu-img`, for creating/overlaying disk images.
- `cloud-image-utils` — gives us `cloud-localds`, which builds the cloud-init "seed" image.
- `wget` — to download the cloud image.

**Verify the edu device exists in your QEMU build** (this is the thing the whole course depends on):

```bash
host$ qemu-system-x86_64 -device help 2>/dev/null | grep -i '"edu"'
```

Expected:

```
name "edu", bus PCI
```

If you see that line, you're good.

---

## Step 1 — Download an Ubuntu cloud image (the guest's root filesystem)

A **cloud image** is a ready-to-boot Ubuntu disk, pre-installed, meant to be configured at first
boot by *cloud-init* (no interactive installer to click through). Perfect for us.

```bash
host$ mkdir -p ~/wks/pcie-edu-driver/vm
host$ cd ~/wks/pcie-edu-driver/vm
host$ wget -O noble-server-cloudimg-amd64.img \
        https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img
```

`noble` = Ubuntu 24.04 LTS. The file is ~600 MB. When it finishes:

```bash
host$ qemu-img info noble-server-cloudimg-amd64.img
```

Expected (numbers may vary slightly): `file format: qcow2`, a `virtual size: 3.5 GiB`-ish, a much
smaller `disk size`.

> Why `vm/` is outside the chapters and **git-ignored**: images are big binaries we *generate*,
> not source. The repo stays small; anyone can re-download.

---

## Step 2 — Make a writable overlay disk (don't dirty the base image)

We don't boot the downloaded image directly. We create a thin **overlay** that keeps the base
image read-only and writes all changes to a separate file. This means we can always throw the
overlay away and get a pristine guest back.

```bash
host$ cd ~/wks/pcie-edu-driver/vm
host$ qemu-img create -f qcow2 -F qcow2 \
        -b noble-server-cloudimg-amd64.img \
        edu-guest.qcow2 16G
```

- `-b ...cloudimg...` — the **backing file**: reads fall through to it; writes go to the overlay.
- `-F qcow2` — the backing file's format (required by modern `qemu-img`).
- `16G` — virtual size ceiling (it won't use 16 GB on disk; qcow2 grows as needed). Room for
  kernel headers + builds.

Verify the overlay points at its backing file:

```bash
host$ qemu-img info edu-guest.qcow2
```

Expected: `backing file: noble-server-cloudimg-amd64.img` in the output.

---

## Step 3 — Build the cloud-init "seed" (sets the login password)

The cloud image has no password yet. We hand it our `cloud-init/user-data` (sets the `ubuntu`
user's password to `edu`) and `cloud-init/meta-data` by packaging them into a tiny disk image
that cloud-init reads on first boot.

```bash
host$ cd ~/wks/pcie-edu-driver/vm
host$ cloud-localds seed.img \
        ../chapters/00-environment/cloud-init/user-data \
        ../chapters/00-environment/cloud-init/meta-data
host$ ls -l seed.img
```

Expected: a small (~tens of KB) `seed.img`. (Look back at
`chapters/00-environment/cloud-init/user-data` — it's commented and explains each field.)

> ⏸ **Pause & reflect — the cloud image / cloud-init model**
>
> - *Why two disks (root + seed)?* The root disk is the OS; the seed is a separate read-only disk
>   whose only job is to carry first-boot config. cloud-init looks for a disk labeled `cidata`
>   (which `cloud-localds` creates) and applies it.
> - *Common pitfall:* editing `user-data` after the guest already booted once does **nothing** —
>   cloud-init runs only on *first* boot (it records that it ran). To re-apply, delete
>   `edu-guest.qcow2` and recreate the overlay (Step 2), which gives a fresh first boot.
> - *Misconception:* "the seed contains Ubuntu." No — the seed is just a few KB of YAML config;
>   all the OS bytes live in the cloud image / overlay.

---

## Step 4 — Boot the guest (with the edu device attached)

The launch script `run-qemu.sh` wires everything together (read its comments — every flag is
explained, including the `-device edu` line):

```bash
host$ ~/wks/pcie-edu-driver/chapters/00-environment/run-qemu.sh
```

You'll see `[run-qemu] KVM acceleration: ON`, then a wall of kernel boot messages on this same
terminal (that's `-nographic`: the guest's serial console is your terminal). The **first** boot is
slower because cloud-init is configuring the machine. Wait for the login prompt:

```
edu-guest login:
```

Log in:
- username: `ubuntu`
- password: `edu`

If cloud-init is still finishing, the password might not be set for a few seconds — wait and retry.

> **How to get out:** to quit QEMU entirely, press `Ctrl-a` then `x`. To cleanly shut the guest
> down from inside, use `guest$ sudo poweroff`.

---

## Step 5 — The payoff: confirm the edu device is present

Inside the guest:

```bash
guest$ lspci -nn | grep -i 1234:11e8
```

Expected (the slot number may differ):

```
00:04.0 Unclassified device [00ff]: Device 1234:11e8
```

There it is — the `edu` device on the guest's PCI bus. Now look closer:

```bash
guest$ sudo lspci -v -d 1234:11e8
```

Expected highlights:
- `Memory at ... [size=1M]` — that's **BAR0**, the 1 MB MMIO region we'll map in Chapter 2.
- a `MSI: Enable- Count=1` capability line — the interrupt support we'll use in Chapter 4.

> ⏸ **Pause & reflect — reading the `lspci` line**
>
> - `00:04.0` is **bus:device.function** (BDF) — the device's address on the PCI bus. The kernel
>   uses this to identify it; you'll see it again in `dmesg` when our driver probes.
> - `[1234:11e8]` is **vendor:device** ID. Our driver matches on exactly this pair (Chapter 1).
>   `1234`/`11e8` are QEMU's made-up IDs for a fake device — not a registered real vendor.
> - "Unclassified device [00ff]" just means edu declares no standard class (it's not a NIC/GPU/…).
>   Real devices report a class so the OS can pick a generic driver; edu deliberately doesn't.
> - *Pitfall:* if `lspci` shows nothing, you booted without `-device edu` (wrong script, or QEMU
>   started before the flag) — re-check the launch command.

---

## Step 6 — Install build tools + kernel headers (in the guest)

To build a kernel module we need the compiler and the **headers for the exact running kernel**
(modules must match their kernel's version and config):

```bash
guest$ sudo apt update
guest$ sudo apt install -y build-essential linux-headers-$(uname -r)
```

`$(uname -r)` expands to the running kernel version, so we get the matching headers. Verify:

```bash
guest$ ls -d /lib/modules/$(uname -r)/build
```

Expected: a path that exists (a symlink into `/usr/src/linux-headers-...`). This `build` directory
is what our `Makefile` points at in every later chapter.

---

## Step 7 — Share the host repo into the guest (edit on host, build in guest)

The launch script already exported the repo over 9p with mount tag `eduhost`. Mount it:

```bash
guest# sudo mkdir -p /mnt/host
guest# sudo mount -t 9p -o trans=virtio,version=9p2000.L eduhost /mnt/host
guest$ ls /mnt/host
```

Expected: you see `README.md`, `COURSE-CONTRACT.md`, `chapters/`, … — the very repo you're editing
on the host. Now the workflow for every chapter is:

1. Edit `qemu_edu.c` on the **host** (your normal editor).
2. In the **guest**, `cd /mnt/host/chapters/NN-name` and `make`.
3. `sudo insmod qemu_edu.ko`, then `dmesg` to see what happened.

> *Pitfall:* the 9p mount does **not** survive a reboot. Re-run the `mount` command after each
> guest boot (or add it to `/etc/fstab` later if you like). If `make` complains about file
> ownership/permissions on the share, building into a copied directory inside the guest also works.

---

## Your daily loop from now on

```bash
host$  ~/wks/pcie-edu-driver/chapters/00-environment/run-qemu.sh   # boot guest
guest$ # log in (ubuntu / edu), then:
guest# sudo mount -t 9p -o trans=virtio,version=9p2000.L eduhost /mnt/host
```

…and you're ready to build. To stop: `guest$ sudo poweroff` (or `Ctrl-a x`).

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `[run-qemu] KVM unavailable` | Nested virt off; it still works, just slower. Fine for learning. |
| Hangs at boot, no login | First boot + cloud-init takes longer; give it a minute. Still stuck? Recreate the overlay (Step 2) and rebuild the seed (Step 3). |
| Login rejected | cloud-init hadn't finished setting the password — wait and retry, or recreate the overlay for a clean first boot. |
| `lspci` shows no `1234:11e8` | Booted without `-device edu`; check you ran `run-qemu.sh`. |
| `make` later: "No rule to make target .../build" | Kernel headers not installed in the guest — redo Step 6. |
| 9p mount fails | Ensure you booted via `run-qemu.sh` (it adds the `-virtfs` share); re-run the mount command. |

---

## What's next

The device is visible and we can build modules. **Chapter 1** writes the smallest possible driver:
a `pci_driver` that matches `1234:11e8` and prints a line from `probe` — proving the kernel
recognizes and hands us the device.
