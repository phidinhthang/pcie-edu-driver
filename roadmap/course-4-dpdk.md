# Course 4 — DPDK & low-latency packet processing (kernel bypass at scale)

> **Goal:** drive a NIC entirely from userspace with **DPDK** — the framework HFT and high-throughput
> networking actually use. Poll-mode drivers, hugepages, mbuf pools, burst RX/TX, and the
> latency-tuning mindset. This is where Course 2's VFIO bypass and Course 3's rings/NAPI converge
> into the production kernel-bypass model.

---

## 0. Instructions to the coding agent (read first)

You are bootstrapping a course in a track. The learner has completed Course 1 (kernel PCIe/DMA driver
on QEMU `edu`), and should have done Course 2 (mmap + **VFIO** userspace driver) and ideally Course 3
(kernel NIC driver: rings/NAPI/MSI-X). Reproduce Course 1's **build-along method** (see §3).

- Create a **new git repo** (suggested: `~/wks/dpdk-lab`). `git init`. Identity
  `user.name = phidinhthang`, `user.email = phidinhthang1112@gmail.com`. Commit per chapter.
- **First deliverables:** top `README.md` + `COURSE-CONTRACT.md`, then **Chapter 0 only**, then stop.
- **One chapter per turn**; learner builds/runs/verifies before the next. Each chapter: `README.md`
  (theory + host/guest-marked commands + expected output) + heavily-commented code.
- Note: DPDK apps are **userspace C** linked against the DPDK libraries (use the DPDK `pkg-config` +
  a plain Makefile, or `meson`). No kernel module building here.

## 1. Who the learner is
FPGA/HFT developer, latency-obsessed, strong C and hardware intuition, has written kernel drivers
(Courses 1, 3) and a userspace VFIO driver (Course 2). This course is squarely on their north star:
**kernel bypass for lowest latency.** Lean into the HFT framing throughout.

## 2. Prerequisites (assume the learner knows)
From Course 2: **VFIO**, the **IOMMU**, mapping BARs and DMA memory into userspace, busy-polling a
device. From Course 3 (helpful): descriptor rings, the poll-vs-interrupt tradeoff (NAPI). DPDK will
feel like "Course 2's hand-rolled poll-mode driver, productized."

## 3. Teaching method (carry into the new repo)
Identical to Course 1: learner runs every command; reproducible from zero with expected output;
prompt markers `host$`/`guest$`/`guest#`; one concept per step; theory→mechanism→code; no quizzes;
**Pause & reflect** callouts; per-chapter README skeleton.

## 4. Substrate & environment
Reuse the WSL2-host → QEMU-guest setup, but the guest now needs a **DPDK-capable NIC** and DPDK's
runtime prerequisites:
- **NIC:** QEMU **`virtio-net-pci`** is the simplest DPDK substrate in a VM (DPDK's `net_virtio` PMD).
  Add a dedicated NIC for DPDK *plus* a survival NIC for SSH/console, because DPDK will take the
  DPDK-bound NIC away from the kernel.
- **Hugepages:** reserve them in the guest (`vm.nr_hugepages` / `hugetlbfs` mount); explain why
  (contiguous DMA memory, fewer TLB misses — a real latency lever).
- **Device binding:** bind the DPDK NIC to **`vfio-pci`** (reuse Course 2's vIOMMU; preferred) or
  `uio_pci_generic`. `dpdk-devbind.py` to bind/inspect.
- **DPDK install:** apt package or build from source; verify with `dpdk-hugepages.py`/`dpdk-testpmd`.
Chapter 0 establishes all of this and proves it with a stock `dpdk-testpmd` run before any custom code.

## 5. Naming & conventions seed
- App per chapter, e.g. `l2fwd_min.c`, `rxtx_burst.c`; build via DPDK `pkg-config`
  (`pkg-config --cflags --libs libdpdk`) in a plain Makefile.
- Keep one EAL-init + port-init helper reused across chapters (a small `common.h`).

## 6. Chapter map (proposed)
| Ch | Folder | Concept added |
|----|--------|---------------|
| 0 | `00-environment` | NIC + survival NIC; hugepages; bind NIC to `vfio-pci`; run stock `dpdk-testpmd` to confirm the platform |
| 1 | `01-eal-port-init` | EAL init (`rte_eal_init`), discover ports, configure + start one port (`rte_eth_dev_*`) |
| 2 | `02-mempool-mbuf` | `rte_mempool` + `rte_mbuf`: the packet buffer model; allocate, inspect, free |
| 3 | `03-rx-tx-burst` | The hot loop: `rte_eth_rx_burst` / `tx_burst`; a minimal L2 forwarder/echo; see packets move |
| 4 | `04-latency` | Measure round-trip latency; busy-poll vs the kernel path; pinning, isolation (`isolcpus`), and why polling wins here |
| 5 | `05-multiqueue-rss` | Multiple RX/TX queues, RSS, lcore-per-queue scaling — the throughput story |
| 6 | `06-flow-and-offload` | (Advanced) `rte_flow` rules / offloads; where hardware does the work — the bridge back to FPGA NIC offload |

## 7. Concepts to weave in, and FPGA/HFT hooks
- **PMD (poll-mode driver):** no interrupts on the data path — the CPU spins on the RX ring. This is
  *literally* Course 2's userspace poll-mode driver, generalized and optimized. Call that back.
- **Hugepages:** why DMA-able, physically-contiguous, TLB-friendly memory matters for latency; the
  IOMMU/IOVA story from Course 2 carries straight over.
- **mbuf/mempool:** DPDK's answer to `sk_buff` — lockless per-core pools, no per-packet allocation in
  the hot path. Contrast with Course 3's skb to make both click.
- **Burst API & batching:** amortizing per-packet cost; why DPDK processes packets in bursts.
- **The whole-stack payoff:** Course 1 (DMA) + Course 2 (VFIO bypass) + Course 3 (rings/NAPI) all
  reappear here as one coherent picture: *this* is how a trading NIC stack (DPDK, Onload, or a custom
  FPGA shell) gets the kernel off the critical path.
- **Pitfalls to pre-empt:** forgetting hugepages / wrong count; NIC not bound to vfio-pci (or IOMMU
  off); losing your SSH NIC to DPDK; expecting interrupts (there are none on the fast path); core
  pinning and NUMA basics.

## 8. Definition of done
The learner can stand up DPDK on a NIC, write a minimal poll-mode forwarder, measure and reason about
its latency, and scale it across queues/cores — and can articulate exactly how DPDK relates to the
kernel network driver (Course 3) and to their own FPGA/XDMA work. The track's arc is complete:
**kernel driver → kernel bypass → network stack → production bypass.**
