// SPDX-License-Identifier: GPL-2.0
/*
 * qemu_edu.c — Chapter 4: MSI interrupts (device notifies us).
 *
 * The big inversion: instead of the CPU spinning on a status bit (Chapter 3),
 * we ask the device to *raise an interrupt* when it's done, and the CPU is free
 * to do other work until then. New machinery:
 *
 *   - pci_set_master()         : let the device issue upstream memory writes
 *                                (an MSI *is* such a write; DMA will need it too).
 *   - pci_alloc_irq_vectors()  : turn on MSI and allocate one interrupt vector.
 *   - request_irq()            : register our handler for that vector.
 *   - an interrupt handler      : runs when the device signals; reads which
 *                                interrupt is pending, ACKs it, wakes the waiter.
 *   - struct completion         : the waiter sleeps until the handler signals.
 *
 * We demonstrate two things: (1) a "doorbell" self-test that raises an interrupt
 * on demand to prove the plumbing, and (2) factorial completion delivered by
 * interrupt instead of polling.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/bits.h>
#include <linux/interrupt.h>   /* request_irq, irqreturn_t, IRQ_HANDLED   */
#include <linux/completion.h>  /* struct completion, wait_for_completion  */

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

#define EDU_REG_ID          0x00
#define EDU_REG_FACT        0x08
#define EDU_REG_STATUS      0x20
#define EDU_REG_INT_STATUS  0x24   /* RO: which interrupt(s) are pending */
#define EDU_REG_INT_RAISE   0x60   /* WO: write V -> device raises IRQ, sets INT_STATUS |= V */
#define EDU_REG_INT_ACK     0x64   /* WO: write V -> clears those bits, lowers the IRQ        */

#define EDU_ID_MAGIC        0x010000ed
#define EDU_STATUS_IRQ_ENABLE  BIT(7)      /* in STATUS: raise IRQ when factorial finishes */

/* Bits the device sets in INT_STATUS to say *why* it interrupted. */
#define EDU_IRQ_FACT        0x00000001     /* factorial unit finished              */
#define EDU_IRQ_TEST        0x00010000     /* our arbitrary self-test value (any
                                            * value distinct from EDU_IRQ_FACT)    */

struct edu_drv {
	struct pci_dev    *pdev;
	void __iomem      *mmio;
	int                irq;            /* Linux IRQ number for our MSI vector  */
	struct completion  irq_done;       /* handler completes this; waiter sleeps on it */
	u32                last_irq_status;/* INT_STATUS the handler observed       */
};

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

/*
 * The interrupt handler. The kernel calls this when our MSI vector fires.
 *
 * It runs in INTERRUPT CONTEXT: it must NOT sleep (no kmalloc(GFP_KERNEL), no
 * mutex, no wait_for_completion), must be quick, and must tell the device to
 * stop asserting the interrupt (ACK). @dev_id is whatever we passed to
 * request_irq() — our struct edu_drv.
 */
static irqreturn_t edu_irq_handler(int irq, void *dev_id)
{
	struct edu_drv *edu = dev_id;
	u32 status;

	/* Why did the device interrupt us? Read the interrupt-status register. */
	status = ioread32(edu->mmio + EDU_REG_INT_STATUS);
	if (status == 0)
		return IRQ_NONE;   /* not us — be a good citizen on shared lines */

	/* ACK: writing the pending bits back to INT_ACK clears them in the device
	 * and de-asserts the interrupt. Forgetting this is a classic bug. */
	iowrite32(status, edu->mmio + EDU_REG_INT_ACK);

	/* Record what we saw, then wake any waiter. complete() is irq-safe and
	 * inserts the barrier that makes last_irq_status visible to the waiter. */
	edu->last_irq_status = status;
	complete(&edu->irq_done);

	return IRQ_HANDLED;   /* yes, this interrupt was ours and we handled it */
}

/*
 * Doorbell self-test: ask the device to raise an interrupt *right now* by
 * writing a test value to INT_RAISE, then wait for the handler to signal.
 * This proves the whole IRQ path independently of the factorial unit.
 */
static void edu_irq_selftest(struct edu_drv *edu)
{
	long timeleft;

	edu->last_irq_status = 0;
	reinit_completion(&edu->irq_done);

	iowrite32(EDU_IRQ_TEST, edu->mmio + EDU_REG_INT_RAISE);   /* ring the doorbell */

	timeleft = wait_for_completion_timeout(&edu->irq_done, msecs_to_jiffies(1000));
	if (timeleft == 0)
		dev_warn(&edu->pdev->dev, "qemu_edu: IRQ self-test TIMED OUT (no interrupt)\n");
	else
		dev_info(&edu->pdev->dev,
			 "qemu_edu: IRQ self-test OK, handler saw INT_STATUS=0x%08x\n",
			 edu->last_irq_status);
}

/*
 * Compute N! but wait for *completion via interrupt* instead of polling:
 *   1. tell the device to raise an IRQ when the factorial finishes (STATUS bit7)
 *   2. write N to start
 *   3. SLEEP on the completion — the CPU is free; the handler wakes us
 *   4. read the result
 *
 * Using a completion is race-free even if the interrupt fires before we reach
 * wait_for_completion(): complete() records the event, so the wait returns
 * immediately rather than missing it.
 */
static u32 edu_factorial_irq(struct edu_drv *edu, u32 n)
{
	long timeleft;

	edu->last_irq_status = 0;
	reinit_completion(&edu->irq_done);

	/* (1) arm the "raise IRQ when factorial done" bit. */
	iowrite32(EDU_STATUS_IRQ_ENABLE, edu->mmio + EDU_REG_STATUS);

	/* (2) start the computation. */
	iowrite32(n, edu->mmio + EDU_REG_FACT);

	/* (3) block until the handler signals (or 1s timeout — never wait forever). */
	timeleft = wait_for_completion_timeout(&edu->irq_done, msecs_to_jiffies(1000));
	if (timeleft == 0)
		dev_warn(&edu->pdev->dev,
			 "qemu_edu: factorial(%u) TIMED OUT waiting for IRQ\n", n);

	/* (4) collect the result. */
	return ioread32(edu->mmio + EDU_REG_FACT);
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct edu_drv *edu;
	int err, nvec;
	u32 ident;

	edu = kzalloc(sizeof(*edu), GFP_KERNEL);
	if (!edu)
		return -ENOMEM;
	edu->pdev = pdev;
	pci_set_drvdata(pdev, edu);
	init_completion(&edu->irq_done);

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: pci_enable_device failed: %d\n", err);
		goto err_free;
	}

	err = pci_request_region(pdev, 0, "qemu_edu");
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: pci_request_region failed: %d\n", err);
		goto err_disable;
	}

	edu->mmio = pci_iomap(pdev, 0, 0);
	if (!edu->mmio) {
		dev_err(&pdev->dev, "qemu_edu: pci_iomap failed\n");
		err = -ENOMEM;
		goto err_release;
	}

	ident = ioread32(edu->mmio + EDU_REG_ID);
	dev_info(&pdev->dev, "qemu_edu: ID register = 0x%08x -> %s\n",
		 ident, ident == EDU_ID_MAGIC ? "OK" : "UNEXPECTED");

	/* Bus mastering: an MSI is delivered as an upstream memory write from the
	 * device, so the device must be a bus master to send it. (DMA in Ch5 needs
	 * this too.) Enable it before we turn on MSI. */
	pci_set_master(pdev);

	/* Allocate exactly one MSI vector. This flips the device's "MSI Enable" bit
	 * on and wires the vector to a Linux IRQ number. min=max=1: we want one.
	 * Returns the count allocated (>0) or a negative errno. */
	nvec = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (nvec < 0) {
		dev_err(&pdev->dev, "qemu_edu: pci_alloc_irq_vectors(MSI) failed: %d\n", nvec);
		err = nvec;
		goto err_clear_master;
	}

	/* Translate "vector 0" into the Linux IRQ number we register a handler for. */
	edu->irq = pci_irq_vector(pdev, 0);

	/* Register our handler. flags=0 (MSI isn't shared); name shows in
	 * /proc/interrupts; the last arg is the dev_id handed back to the handler. */
	err = request_irq(edu->irq, edu_irq_handler, 0, "qemu_edu", edu);
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: request_irq(%d) failed: %d\n", edu->irq, err);
		goto err_free_vectors;
	}
	dev_info(&pdev->dev, "qemu_edu: MSI enabled, using IRQ %d\n", edu->irq);

	/* --- Prove the interrupt path, then use it for real --- */
	edu_irq_selftest(edu);

	{
		static const u32 inputs[] = { 5, 10, 12 };
		int i;

		for (i = 0; i < ARRAY_SIZE(inputs); i++) {
			u32 n = inputs[i];
			u32 result = edu_factorial_irq(edu, n);

			dev_info(&pdev->dev,
				 "qemu_edu: factorial(%2u) = %-10u (via interrupt)\n",
				 n, result);
		}
	}

	dev_info(&pdev->dev, "qemu_edu: probe complete\n");
	return 0;

err_free_vectors:
	pci_free_irq_vectors(pdev);
err_clear_master:
	pci_clear_master(pdev);
	pci_iounmap(pdev, edu->mmio);
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

	/* Stop the device from raising further factorial interrupts, then tear
	 * down in reverse order of probe. free_irq() guarantees the handler is not
	 * running once it returns, so it's safe to unmap mmio afterwards. */
	iowrite32(0, edu->mmio + EDU_REG_STATUS);   /* disable raise-on-factorial */
	free_irq(edu->irq, edu);
	pci_free_irq_vectors(pdev);
	pci_clear_master(pdev);
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
MODULE_DESCRIPTION("Educational driver for the QEMU edu PCI device — Ch4 MSI interrupts");
