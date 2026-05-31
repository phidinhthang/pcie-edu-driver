// SPDX-License-Identifier: GPL-2.0
/*
 * qemu_edu.c — Chapter 3: command/status/poll (the factorial unit).
 *
 * New idea this stage: the device does *asynchronous* work. We write N to a
 * command register; the device computes N! in a background worker and, while
 * busy, holds a "computing" bit set in its STATUS register. We POLL that bit
 * until it clears, then read the result back. This is the classic
 * command -> poll-for-completion -> read-result pattern.
 *
 * Everything from Chapter 2 (enable, map BAR0, ID + liveness) is unchanged;
 * we only add the factorial demo at the end of probe.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/bits.h>    /* BIT()        */
#include <linux/delay.h>   /* cpu_relax via processor headers; explicit here  */

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

#define EDU_REG_ID        0x00
#define EDU_REG_LIVENESS  0x04
#define EDU_REG_FACT      0x08   /* RW: write N to start; read N! when done */
#define EDU_REG_STATUS    0x20   /* RW: bit0 = computing (busy)            */

#define EDU_ID_MAGIC      0x010000ed
#define EDU_STATUS_COMPUTING  BIT(0)   /* set while the factorial worker is busy */

struct edu_drv {
	struct pci_dev *pdev;
	void __iomem   *mmio;
};

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

/*
 * Compute N! on the device using the command/status/poll pattern.
 *
 * Sequence:
 *   1. WRITE N to the factorial register  -> the device starts its worker AND
 *      sets STATUS.COMPUTING (it sets that bit synchronously, *before* this
 *      MMIO write returns, so there is no "poll before busy is set" race).
 *   2. POLL the status register until COMPUTING clears (the worker finished).
 *   3. READ the factorial register -> it now holds N!.
 *
 * @spins_out (optional) reports how many poll iterations we spun, so you can
 * see whether the device finished instantly or made us wait.
 *
 * Note: the device computes a 32-bit result, so N! overflows for N >= 13
 * (13! does not fit in 32 bits). Keep N <= 12 for exact answers.
 */
static u32 edu_factorial(struct edu_drv *edu, u32 n, unsigned int *spins_out)
{
	unsigned int spins = 0;
	const unsigned int max_spins = 10000000;   /* safety valve: never spin forever */

	/* (1) Kick off the computation. */
	iowrite32(n, edu->mmio + EDU_REG_FACT);

	/* (2) Busy-poll the status register until the device clears COMPUTING.
	 *     cpu_relax() is a cheap "I'm spinning" hint to the CPU (on x86 it's a
	 *     PAUSE instruction) that's polite to a sibling hyperthread. */
	while (ioread32(edu->mmio + EDU_REG_STATUS) & EDU_STATUS_COMPUTING) {
		cpu_relax();
		if (++spins >= max_spins) {
			dev_warn(&edu->pdev->dev,
				 "qemu_edu: factorial(%u) timed out (COMPUTING stuck)\n", n);
			break;
		}
	}

	if (spins_out)
		*spins_out = spins;

	/* (3) The result is now in the same register we wrote N into. */
	return ioread32(edu->mmio + EDU_REG_FACT);
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct edu_drv *edu;
	int err;
	u32 ident, live_w, live_r;

	edu = kzalloc(sizeof(*edu), GFP_KERNEL);
	if (!edu)
		return -ENOMEM;
	edu->pdev = pdev;
	pci_set_drvdata(pdev, edu);

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: pci_enable_device failed: %d\n", err);
		goto err_free;
	}

	err = pci_request_region(pdev, 0, "qemu_edu");
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: pci_request_region(BAR0) failed: %d\n", err);
		goto err_disable;
	}

	edu->mmio = pci_iomap(pdev, 0, 0);
	if (!edu->mmio) {
		dev_err(&pdev->dev, "qemu_edu: pci_iomap(BAR0) failed\n");
		err = -ENOMEM;
		goto err_release;
	}

	/* --- Chapter 2 checks (still useful as a sanity gate) --- */
	ident = ioread32(edu->mmio + EDU_REG_ID);
	dev_info(&pdev->dev, "qemu_edu: ID register = 0x%08x -> %s\n",
		 ident, ident == EDU_ID_MAGIC ? "OK" : "UNEXPECTED");

	live_w = 0xdeadbeef;
	iowrite32(live_w, edu->mmio + EDU_REG_LIVENESS);
	live_r = ioread32(edu->mmio + EDU_REG_LIVENESS);
	dev_info(&pdev->dev, "qemu_edu: liveness 0x%08x -> 0x%08x -> %s\n",
		 live_w, live_r, live_r == (u32)~live_w ? "OK" : "MISMATCH");

	/* --- Chapter 3: drive the factorial unit by polling --- */
	{
		static const u32 inputs[] = { 0, 1, 5, 10, 12 };
		int i;

		for (i = 0; i < ARRAY_SIZE(inputs); i++) {
			unsigned int spins;
			u32 n = inputs[i];
			u32 result = edu_factorial(edu, n, &spins);

			dev_info(&pdev->dev,
				 "qemu_edu: factorial(%2u) = %-10u (polled %u times)\n",
				 n, result, spins);
		}
	}

	dev_info(&pdev->dev, "qemu_edu: probe complete\n");
	return 0;

err_release:
	pci_release_region(pdev, 0);
err_disable:
	pci_disable_device(pdev);
err_free:
	kfree(edu);
	return err;
}

static void edu_remove(struct pci_dev *pdev)
{
	struct edu_drv *edu = pci_get_drvdata(pdev);

	pci_iounmap(pdev, edu->mmio);
	pci_release_region(pdev, 0);
	pci_disable_device(pdev);
	kfree(edu);

	dev_info(&pdev->dev, "qemu_edu: remove\n");
}

static struct pci_driver edu_driver = {
	.name     = "qemu_edu",
	.id_table = edu_ids,
	.probe    = edu_probe,
	.remove   = edu_remove,
};
module_pci_driver(edu_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("phidinhthang");
MODULE_DESCRIPTION("Educational driver for the QEMU edu PCI device — Ch3 factorial polling");
