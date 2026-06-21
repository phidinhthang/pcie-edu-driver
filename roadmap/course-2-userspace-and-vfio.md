# Course 2 — Userspace access & kernel bypass (mmap + VFIO on the `edu` device)

> **Goal:** take the *same* QEMU `edu` device into userspace two ways — first with `mmap` zero-copy
> through your Course 1 kernel driver, then with **full kernel bypass** via VFIO, ending in a small
> **userspace poll-mode driver** that drives edu's DMA with no kernel driver at all. This is *how
> DPDK touches hardware*, learned on the device you already understand.

---

## 0. Instructions to the coding agent (read first)

You are bootstrapping the **second course** in a track. The learner has completed Course 1 (a kernel
PCIe/DMA driver for the QEMU `edu` device: `pci_driver` probe/remove, MMIO via BAR0, MSI interrupts,
`dma_alloc_coherent`, a char device). Reproduce that course's **build-along method exactly** (see §3).

Concretely:
- Create a **new git repo** in a fresh directory (suggested: `~/wks/edu-userspace-vfio`). `git init`.
- Set git identity: `user.name = phidinhthang`, `user.email = phidinhthang1112@gmail.com`. Commit per
  chapter, with a trailer `Co-Authored-By: Claude <noreply@anthropic.com>` if your harness uses one.
- **First deliverables:** a top `README.md` (course overview + chapter table) and a
  `COURSE-CONTRACT.md` (conventions: names, register macros reused from Course 1, prompt markers,
  teaching rules from §3). Then generate **Chapter 0 only** and stop for the learner to verify.
- **One chapter per turn.** Never dump the whole course. The learner builds/runs/verifies each
  chapter and reports back before you write the next.
- Each chapter folder: `README.md` (theory + every command, host/guest-marked, with expected output)
  + heavily-commented code. Self-contained snapshots so chapters `diff` cleanly.

## 1. Who the learner is
FPGA developer for HFT/trading: strong HDL/hardware and latency intuition, comfortable in C, now has
one kernel driver under their belt (Course 1) but is still relatively new to kernel/userspace
internals. They learn by **running every command themselves** to build muscle memory. Motivate the
*why*, connect to latency/FPGA/XDMA where natural.

## 2. Prerequisites (assume the learner knows)
From Course 1: PCI enumeration & probe/remove, BAR0 MMIO with `ioread32`/`iowrite32`, the edu
register map, MSI setup (`pci_alloc_irq_vectors`/`request_irq`) + an IRQ handler + `struct
completion`, `dma_alloc_coherent` and the bus-vs-CPU-vs-physical address distinction, a char device
with `file_operations` and `copy_*_user`. Reuse the edu register macros verbatim.

## 3. Teaching method (carry these into the new repo — they made Course 1 work)
- **Learner runs every educational command;** the agent only writes files and commits repo plumbing.
- **Reproducible from zero:** every command in the chapter README, explained, marked `host$` (WSL2
  host), `guest$` (guest user), `guest#` (guest root), with **expected output** shown.
- **One concept per step, verified before moving on.** **Theory → mechanism → code.** **No
  exercises/quizzes.**
- **Pause & reflect** callouts at natural breakpoints: surface the questions, pitfalls, and
  misconceptions a newcomer hits right there.
- Per-chapter README skeleton: *Where we are → The idea/why → The mechanism → The code → Build & run
  it → Pause & reflect → What's next.*

## 4. Environment (extends Course 1's setup)
Same architecture: **WSL2 Ubuntu host → QEMU guest (Ubuntu cloud image) with `-device edu` → build
the driver/programs in the guest** over the 9p share (`mount_tag eduhost` at `/mnt/host`), KVM
enabled. **New requirement for the VFIO half:** the guest needs a **virtual IOMMU**. Update the QEMU
launch to a `q35` machine with `intel-iommu` (e.g. `-machine q35,kernel-irqchip=split -device
intel-iommu` and ensure `edu` sits behind it), and boot the guest kernel with `intel_iommu=on`.
Chapter 0 must establish and *verify* this (IOMMU groups visible under
`/sys/kernel/iommu_groups/`, edu in its own group). Also covered there: loading `vfio-pci`, and how
to unbind edu from the Course-1 driver and bind it to `vfio-pci` (via `driver_override` / `new_id`).

## 5. Naming & conventions seed
- Kernel module (mmap chapters): keep `qemu_edu` / `struct edu_drv`, extended from Course 1.
- Userspace programs (VFIO chapters): a small C app, e.g. `edu_user.c` → `edu_user`, built with a
  plain Makefile (no kernel build system).
- Reuse all `EDU_REG_*` / `EDU_DMA_*` macros from Course 1.

## 6. Chapter map (proposed — refine as you teach)
| Ch | Folder | Concept added |
|----|--------|---------------|
| 0 | `00-environment` | Add a vIOMMU to the QEMU guest; verify IOMMU groups; `vfio-pci` available; recap the Course-1 driver still loads |
| 1 | `01-mmap` | Add `.mmap` (`dma_mmap_coherent`) + an `ioctl` to trigger DMA to the **kernel** driver; a userspace program maps the buffer and round-trips data **zero-copy** (no `copy_*_user`) |
| 2 | `02-vfio-intro` | What VFIO is and why; the IOMMU as the safety boundary; unbind edu from `qemu_edu`, bind to `vfio-pci`; walk the container/group/device model |
| 3 | `03-vfio-mmio` | Userspace opens the VFIO device, gets BAR0 region info, `mmap`s it, and reads the edu **ID register from userspace** — first hardware touch with *no kernel driver* |
| 4 | `04-vfio-dma` | `VFIO_IOMMU_MAP_DMA` a userspace buffer, program edu's DMA registers with the IOVA, run a transfer, **busy-poll** completion |
| 5 | `05-vfio-msi` | Receive edu's MSI in userspace via `VFIO_DEVICE_SET_IRQS` + an `eventfd`; contrast busy-poll vs interrupt again |
| 6 | `06-poll-mode-driver` | Capstone: a tiny **userspace poll-mode driver** for edu — map BAR + DMA, kick transfers, busy-poll — "DPDK architecture in miniature" |

## 7. Concepts to weave in, and FPGA/HFT hooks
- **`mmap` zero-copy** is the single biggest latency win over `read`/`write`: the data path stops
  crossing the user/kernel boundary per byte. Tie back to Course 1 Ch6's `copy_*_user`.
- **VFIO + IOMMU**: the IOMMU is what makes userspace DMA *safe* (a device can only reach memory you
  mapped for it). This is the "seatbelt" from Course 1 Ch5 made central. Explain IOMMU groups,
  IOVA vs physical, and why kernel bypass needs it.
- **Poll-mode vs interrupt** returns as the core latency-vs-CPU tradeoff (Course 1 Ch3 vs Ch4) — now
  the learner *chooses* polling in userspace, exactly as DPDK does.
- **Pitfalls to pre-empt:** IOMMU not enabled / wrong machine type; edu not in its own IOMMU group;
  forgetting to unbind the kernel driver before binding `vfio-pci`; passing a CPU pointer instead of
  the mapped IOVA to the device; permissions on `/dev/vfio/*`.

## 8. Definition of done
The learner can: expose a kernel DMA buffer to userspace via `mmap`; bind a PCIe device to VFIO;
from a plain userspace program map its BARs, map DMA memory through the IOMMU, drive a DMA transfer,
and take its interrupt — i.e. they've written a **userspace device driver** and understand precisely
what DPDK does under the hood. This is the direct on-ramp to Course 4.
