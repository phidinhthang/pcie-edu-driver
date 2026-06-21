# Course 3 — Modern Linux network driver (netdev, NAPI, sk_buff, descriptor rings)

> **Goal:** learn the Linux **network driver** model end to end — first the stack-facing API on a
> *virtual* netdev (no hardware), then the *real* hardware path (TX/RX **descriptor rings**,
> streaming DMA, **MSI-X**, **NAPI**) by writing a driver for an emulated `e1000` NIC. This is where
> "descriptor rings" and "multiqueue interrupts" become real — the genuine FPGA/XDMA ring model.

---

## 0. Instructions to the coding agent (read first)

You are bootstrapping a course in a track. The learner has completed Course 1 (kernel PCIe/DMA driver
for QEMU `edu`: probe/remove, MMIO, MSI, `dma_alloc_coherent`, char device) and ideally Course 2
(mmap + VFIO). Reproduce Course 1's **build-along method** (see §3).

- Create a **new git repo** (suggested: `~/wks/linux-net-driver`). `git init`. Identity
  `user.name = phidinhthang`, `user.email = phidinhthang1112@gmail.com`. Commit per chapter.
- **First deliverables:** top `README.md` + `COURSE-CONTRACT.md`, then **Chapter 0 only**, then stop.
- **One chapter per turn**; learner builds/runs/verifies before the next. Each chapter: `README.md`
  (theory + host/guest-marked commands + expected output) and heavily-commented code.

## 1. Who the learner is
FPGA/HFT developer, strong hardware + latency intuition, comfortable in C, has written kernel PCIe
drivers (Course 1). Motivate the *why*; connect rings/NAPI/DMA to FPGA NICs and XDMA where natural.

## 2. Prerequisites (assume the learner knows)
PCI probe/remove, BAR MMIO, MSI + IRQ handlers, `dma_alloc_coherent` and bus-vs-CPU addresses, the
polling-vs-interrupt tradeoff. **New territory:** the kernel **network stack** APIs.

## 3. Teaching method (carry into the new repo)
Identical to Course 1: learner runs every command; reproducible from zero with expected output;
prompt markers `host$`/`guest$`/`guest#`; one concept per step; theory→mechanism→code; no
quizzes; **Pause & reflect** callouts; per-chapter README skeleton *(Where we are → idea → mechanism
→ code → build & run → reflect → what's next)*.

## 4. Substrate & environment — a deliberate **two-phase** design
Reuse the WSL2-host → QEMU-guest → 9p-share setup. This course uses **two substrates**, easy → real:

- **Phase A — a virtual netdev (no hardware), snull-style.** Teaches the *stack-facing* API in
  isolation: `alloc_etherdev`, `net_device_ops`, `ndo_open/stop/start_xmit`, `sk_buff`, `netif_rx` /
  NAPI, stats. No DMA, no PCI — just the network-driver contract. Verify with `ip link`, `ping`
  across the virtual interface(s).
- **Phase B — the `e1000` NIC.** Add `-device e1000 -netdev user,...` to QEMU and write a real driver:
  PCI probe (reuse Course 1), **TX/RX descriptor rings** in DMA-coherent memory, **streaming DMA** of
  `sk_buff` payloads (`dma_map_single` + `dma_sync_*`), **MSI/MSI-X**, **NAPI** poll integrated with
  the real IRQ. The learner must `unbind` the in-tree `e1000` driver first (note: this drops guest
  networking on that NIC — keep a second NIC, e.g. virtio-net, for SSH, or work on the console).
  Datasheet: Intel's e1000 (8254x) SDM; keep scope to bring-up + TX + RX, skip PHY/EEPROM esoterica.

Chapter 0 sets up both: confirm the virtual-netdev phase needs no extra QEMU, and add the `e1000`
device + a survival NIC for Phase B; show `lspci`/`ethtool -i` identifying the e1000.

## 5. Naming & conventions seed
- Phase A module: `snet` (or similar) → `snet.ko`; one or two virtual interfaces `sn0`/`sn1`.
- Phase B module: `mye1000` → `mye1000.ko`; `struct e1000_priv`. Define e1000 register macros once in
  `COURSE-CONTRACT.md` and reuse.
- Keep `priv` recovered via `netdev_priv()`; `container_of` patterns as in Course 1.

## 6. Chapter map (proposed)
| Ch | Folder | Concept added |
|----|--------|---------------|
| 0 | `00-environment` | QEMU NIC setup (e1000 + a survival NIC); `lspci`/`ethtool -i`; build tools recap |
| 1 | `01-netdev-skeleton` | Phase A: `alloc_etherdev` + `net_device_ops`, register, `ip link` sees it |
| 2 | `02-tx-path` | Phase A: `ndo_start_xmit`, `sk_buff` anatomy, stats, `netif_*queue` flow control |
| 3 | `03-rx-napi` | Phase A: RX + **NAPI** (`napi_schedule`/`poll`/`napi_complete`), `ping` works |
| 4 | `04-e1000-probe` | Phase B: PCI probe, map BARs, read MAC, reset/bring-up the device |
| 5 | `05-e1000-rings` | TX/RX **descriptor rings** in coherent memory; ring producer/consumer indices, doorbells |
| 6 | `06-e1000-tx` | Real transmit: map skb (streaming DMA), post descriptor, ring the tail, reclaim on completion |
| 7 | `07-e1000-rx-irq` | Real receive: refill RX ring, **MSI-X**, NAPI poll, hand skbs to the stack; `ping` over real DMA |
| 8 | `08-ethtool-stats` | `ethtool_ops`, stats, link state — the "production polish" surface |

## 7. Concepts to weave in, and FPGA/HFT hooks
- **`sk_buff`** is the kernel's packet container — headroom/tailroom, linear vs paged. The
  abstraction every NIC driver speaks.
- **Descriptor rings** are Course 1 Ch5's single DMA "unit cell" scaled into a producer/consumer
  ring with a doorbell — *exactly* the FPGA/XDMA model. Make this connection explicit.
- **NAPI** is the polling-vs-interrupt tradeoff (Course 1 Ch3/Ch4) institutionalized: interrupt to
  wake, then **poll a budget** to drain — the kernel's answer to interrupt storms. Direct HFT
  relevance (and the conceptual cousin of DPDK's pure polling in Course 4).
- **Streaming DMA** (`dma_map_single`/`dma_map_page` + `dma_sync_*`) vs Course 1's coherent buffers:
  why packets use streaming, and the sync pitfalls on non-coherent hardware.
- **MSI-X / multiqueue**: multiple vectors, per-queue interrupts, affinity — what edu couldn't do.
- **Pitfalls to pre-empt:** racing the in-tree e1000 driver (unbind first); skb lifetime/ownership
  (who frees, when); forgetting `dma_unmap` on TX completion (leak/corruption); ring full / NAPI
  budget handling; endianness of descriptors.

## 8. Definition of done
The learner can register a `net_device`, implement TX/RX with NAPI, and — on real (emulated)
hardware — drive descriptor rings with streaming DMA and MSI-X, passing real packets through the
Linux network stack. They now understand the layer DPDK (Course 4) deliberately replaces.
