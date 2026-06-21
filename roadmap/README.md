# Roadmap — a track of courses on Linux device & network drivers

This folder holds **kickoff briefs** for the courses that follow this one (the PCIe/DMA driver
course on QEMU's `edu` device). Each brief is **self-contained**: open a fresh chat session, point
the coding agent at a single `course-N-*.md` file, tell it to start a new repo, and it has
everything it needs to generate that course in the same build-along style as Course 1.

## The track

| # | Course | Substrate | Builds the concept of |
|---|--------|-----------|-----------------------|
| 1 | **PCIe/DMA driver foundation** (this repo) | QEMU `edu` | enumerate, MMIO/BAR, MSI, DMA, char device |
| 2 | [Userspace access & kernel bypass](course-2-userspace-and-vfio.md) | QEMU `edu` (+ vIOMMU) | `mmap` zero-copy, VFIO, a userspace poll-mode driver |
| 3 | [Modern Linux network driver](course-3-kernel-network-driver.md) | virtual netdev, then `e1000` NIC | `net_device`, NAPI, `sk_buff`, descriptor rings, MSI-X |
| 4 | [DPDK & low-latency packet processing](course-4-dpdk.md) | QEMU `virtio-net` | PMD, hugepages, mbuf, burst I/O, kernel-bypass at scale |

## Why this order

Course 1 is the **substrate-neutral trunk** (BARs, MSI, DMA) every later course rests on. Course 2
takes the *same edu device* into userspace two ways — kernel-assisted (`mmap`) and full kernel
bypass (VFIO) — which is exactly *how DPDK touches hardware*, learned on familiar ground. Courses 3
and 4 are **siblings**, not sequential: they're the two competing ways to drive a NIC — through the
kernel network stack (Course 3) versus bypassing it (Course 4). Do them in either order; Course 3
gives you the stack DPDK throws away, Course 4 gives you the latency path. Both reuse rings + MSI +
DMA from Courses 1–2.

> **Substrate honesty:** each feature is taught where it's *real*. `edu` has one MSI vector, one DMA
> engine, and a 4 KB buffer, so genuine **descriptor rings and MSI-X live in Course 3** (the NIC
> actually has them), not faked on edu. Course 2 stays on edu because `mmap` and VFIO *are* fully
> real there.

## How to start a course (in a fresh session)

1. Open a new chat with the coding agent in an empty working directory.
2. Paste or point it at the relevant `roadmap/course-N-*.md` from this repo (copy the file over, or
   give its path).
3. Say: *"Read this brief and start the course — create the new repo and generate Chapter 0, then
   wait for me to build and verify before the next chapter."*
4. The brief tells the agent the learner profile, environment, teaching method, conventions, and
   chapter map, so it reproduces Course 1's cadence without needing this conversation's history.

The learner runs every educational command themselves and reports back — same as Course 1.
