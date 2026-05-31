# Chapter 3 — Command / Status / Poll: the factorial unit

## Where we are

So far the device answered instantly: read ID, write/read liveness — done within the MMIO access
itself. Real hardware usually does **work that takes time** (a DMA transfer, a crypto op, a
calculation) and needs a way to say *"still working… now done."* The edu device models this with a
**factorial unit**: write N, it computes N! in a background worker, and exposes a **"computing"
status bit** while busy. This chapter drives it by **polling** that bit. It's the same
*command → wait-for-completion → read-result* shape you'll use for almost every nontrivial device —
and the polling here is the deliberate contrast to the *interrupt* version in Chapter 4.

Files: `qemu_edu.c` (adds an `edu_factorial()` helper + a demo loop in probe), `Makefile`
(unchanged). Everything from Chapter 2 stays.

---

## The idea: how a device reports "busy / done"

Three registers cooperate (from our datasheet):

- `EDU_REG_FACT` (`0x08`) — **command + result, same address.** Writing N is the *command* ("start
  computing N!"). Once finished, reading it returns the *result* (N!).
- `EDU_REG_STATUS` (`0x20`), bit 0 `EDU_STATUS_COMPUTING` — the **busy flag.** Set while the
  worker runs, cleared when it's done.

The protocol:

```
1. WRITE N -> EDU_REG_FACT        (issue the command)
2. spin while (STATUS & COMPUTING) (wait for completion)
3. READ EDU_REG_FACT              (collect the result, N!)
```

That middle step — **polling** — means the CPU repeatedly reads the status register, burning cycles
until the bit clears. Simple and direct. Its weakness (which motivates Chapter 4) is exactly that:
the CPU is *busy-waiting*, doing nothing useful while it spins.

### Why there's no race (an important subtlety)

A natural worry: *what if I poll the status bit before the device has set it, see "not busy," and
read a garbage result?* The edu device closes that hole: the **write handler sets `COMPUTING`
synchronously**, before your `iowrite32(N)` even returns. So by the time you reach step 2, the bit
is already set (or the worker has already finished and set the real result). Either outcome is
correct:

- worker still running → you see `COMPUTING`, you wait, then read the result;
- worker already done (tiny N) → you see it clear immediately and read the correct result.

There is no window where you read a stale/garbage value. (Real hardware datasheets specify this kind
of ordering guarantee; here QEMU's model provides it.)

### One real-world wrinkle: result width

The device computes a **32-bit** factorial. `12!` = 479,001,600 fits in 32 bits; **`13!` overflows**
(it's > 4.29e9). So we demo `N ≤ 12` for exact answers — and the overflow is a nice experiment
(below). Every real device has limits like this; reading the "datasheet" for them is the job.

---

## The mechanism in code

`edu_factorial()` implements the three steps literally. Two details worth noting:

- **`cpu_relax()`** inside the spin loop is a cheap hint to the CPU that we're busy-waiting (on x86
  it emits a `PAUSE` instruction). It reduces power/contention with a sibling hyperthread. Always
  put it in a poll loop.
- **A bounded spin count** (`max_spins`) is a safety valve: never trust hardware to *always* clear a
  bit. If a device wedges, an unbounded `while` would **hang the kernel**. We cap it and `dev_warn`.

We run the demo from `probe` over inputs `{0,1,5,10,12}`. (Running heavy work in `probe` is fine for
a teaching demo; a production driver would do this in response to a userspace request — Chapter 6.)

Read `qemu_edu.c`: the `edu_factorial()` comments map one-to-one to steps 1–3.

---

## Build & run it

In the guest (re-mount the share if you rebooted):

```bash
guest$ cd /mnt/host/chapters/03-factorial-polling
guest$ make
guest$ sudo insmod ./qemu_edu.ko
guest$ sudo dmesg | tail -n 9
```

Expected (the `polled N times` counts are timing-dependent — often `0`, since the device finishes
before our first poll):

```
qemu_edu 0000:00:03.0: qemu_edu: ID register = 0x010000ed -> OK
qemu_edu 0000:00:03.0: qemu_edu: liveness 0xdeadbeef -> 0x21524110 -> OK
qemu_edu 0000:00:03.0: qemu_edu: factorial( 0) = 1          (polled 0 times)
qemu_edu 0000:00:03.0: qemu_edu: factorial( 1) = 1          (polled 0 times)
qemu_edu 0000:00:03.0: qemu_edu: factorial( 5) = 120        (polled 0 times)
qemu_edu 0000:00:03.0: qemu_edu: factorial(10) = 3628800    (polled 0 times)
qemu_edu 0000:00:03.0: qemu_edu: factorial(12) = 479001600  (polled 0 times)
qemu_edu 0000:00:03.0: qemu_edu: probe complete
```

Verify a couple by hand: `5! = 120`, `10! = 3,628,800`, `12! = 479,001,600`. Then unload:

```bash
guest$ sudo rmmod qemu_edu
```

---

## ⏸ Pause & reflect

- **"polled 0 times" — did polling even happen?** Yes; the loop ran, checked the bit once, found it
  already clear, and didn't spin. That's the *correct* behavior for a fast device — and it's why the
  no-race guarantee matters: even with zero spins, the result is valid. To *see* nonzero spins you'd
  need the device to take longer than one MMIO read; this model is too fast for that, but the
  structure is exactly what you'd use for slow hardware.
- **The cost of polling.** While spinning, this CPU does nothing else. For microseconds that's fine;
  for a millisecond DMA it's wasteful, and inside `probe` (which can run in atomic-ish contexts in
  some paths) long spins are antisocial. This is the whole motivation for **interrupts** (Chapter 4):
  let the device *tell* the CPU when it's done, so the CPU can do other work meanwhile.
- **Idiomatic polling.** Hand-rolling the loop teaches the pattern, but the kernel ships
  `readx_poll_timeout()` / `read_poll_timeout()` (`<linux/iopoll.h>`) that do exactly this — poll a
  register until a condition holds, with a real time-based timeout. In production you'd use those;
  we wrote it out so the mechanism is visible.
- **Command and result share an address.** Writing `0x08` means "start"; reading `0x08` means "give
  me the answer." Same offset, opposite meaning by direction — common in real register maps (a
  TX/RX FIFO port is the same idea). Don't assume an address has one fixed meaning.
- **Always bound your waits.** The `max_spins` cap isn't decoration. A driver that does
  `while (reg & BUSY);` against hardware that never clears `BUSY` is an unkillable kernel hang. Cap
  it, and react.

Two experiments worth doing:
1. **Overflow:** add `13` (or `20`) to the `inputs[]` array, rebuild, reload. Predict the result
   first, then check — you'll see a wrong/wrapped number because `N!` exceeded 32 bits. This is a
   real lesson in reading device limits, not a bug in your code.
2. **Watch the busy bit directly:** right after the `iowrite32(n, ...)` in `edu_factorial`, add a
   `dev_info` printing `ioread32(edu->mmio + EDU_REG_STATUS)` — you'll usually catch `COMPUTING`
   (bit 0) set, confirming the synchronous-set guarantee.

---

## What's next

Polling works but wastes the CPU. **Chapter 4** flips the model: we enable **MSI interrupts**, ask
the factorial unit to **raise an interrupt when it finishes**, register an interrupt handler, and
have the device *notify* us — then ACK the interrupt. Same factorial, but the CPU is free until the
device signals completion.
