// SPDX-License-Identifier: GPL-2.0
/*
 * qemu_edu.c — Chapter 2: first real hardware conversation (MMIO via BAR0).
 *
 * What this stage adds on top of the Ch1 skeleton, all inside .probe:
 *   1. pci_enable_device()  — power the device up so it responds to accesses.
 *   2. pci_request_region()  — reserve BAR0 so no one else uses it.
 *   3. pci_iomap()           — map BAR0's physical MMIO window into kernel
 *                              virtual address space, giving us a pointer.
 *   4. ioread32()/iowrite32()— read the identification register (expect the
 *                              magic 0x010000ed) and run the liveness check
 *                              (write X, read back ~X).
 * .remove tears all of that down in reverse order.
 *
 * We also introduce `struct edu_drv`: our per-device state, stashed on the
 * pci_dev via pci_set_drvdata() so .remove (and later, interrupt handlers) can
 * find it again.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>      /* ioread32 / iowrite32                 */
#include <linux/slab.h>    /* kzalloc / kfree                     */

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

/* BAR0 register offsets we use this chapter (full map lives in COURSE-CONTRACT). */
#define EDU_REG_ID        0x00   /* RO: identification magic, reads 0x010000ed */
#define EDU_REG_LIVENESS  0x04   /* RW: write X, read back ~X                  */

/* The exact value the identification register must return for edu v1.0. */
#define EDU_ID_MAGIC      0x010000ed

/*
 * Per-device private state. One of these is allocated per device in .probe.
 * For now it holds just the device handle and the BAR0 mapping; later chapters
 * add the IRQ number and DMA buffer here.
 */
struct edu_drv {
	struct pci_dev *pdev;   /* back-pointer to the PCI device      */
	void __iomem   *mmio;   /* BAR0 mapped into kernel virtual addr */
};

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct edu_drv *edu;
	int err;
	u32 ident, live_w, live_r;

	/* (a) Allocate our private state. GFP_KERNEL = normal allocation that
	 *     may sleep; fine here because probe runs in process context. */
	edu = kzalloc(sizeof(*edu), GFP_KERNEL);
	if (!edu)
		return -ENOMEM;
	edu->pdev = pdev;
	/* Stash it on the device so .remove can retrieve it with get_drvdata. */
	pci_set_drvdata(pdev, edu);

	/* (b) Enable the device: wake it from D3, enable its memory decoding so
	 *     accesses to its BARs actually reach it, and hook up its IRQ line.
	 *     Until this succeeds, reading its registers is undefined. */
	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: pci_enable_device failed: %d\n", err);
		goto err_free;
	}

	/* (c) Reserve BAR0 (region index 0). This is *ownership*, not mapping:
	 *     it marks the physical range busy in /proc/iomem under "qemu_edu",
	 *     so a second driver can't stomp on the same registers. */
	err = pci_request_region(pdev, 0, "qemu_edu");
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: pci_request_region(BAR0) failed: %d\n", err);
		goto err_disable;
	}

	/* (d) Map BAR0 into kernel virtual memory. The device's registers live
	 *     at a physical address (we saw 0xfea00000 in lspci); the CPU can't
	 *     use a physical address directly, so we ask for a kernel virtual
	 *     mapping of that window. maxlen=0 means "map the whole BAR".
	 *     The result is tagged __iomem: it is NOT normal memory — you must
	 *     go through ioread/iowrite, never dereference it directly. */
	edu->mmio = pci_iomap(pdev, 0, 0);
	if (!edu->mmio) {
		dev_err(&pdev->dev, "qemu_edu: pci_iomap(BAR0) failed\n");
		err = -ENOMEM;
		goto err_release;
	}

	/* (e) First real read: the identification register at offset 0x00.
	 *     ioread32 issues a single 32-bit MMIO load to BAR0+0x00 and handles
	 *     endianness (edu is little-endian) and ordering for us. */
	ident = ioread32(edu->mmio + EDU_REG_ID);
	dev_info(&pdev->dev, "qemu_edu: ID register = 0x%08x (expect 0x%08x) -> %s\n",
		 ident, EDU_ID_MAGIC,
		 ident == EDU_ID_MAGIC ? "OK" : "UNEXPECTED");

	/* (f) Liveness check: write a value, read it back; the device returns the
	 *     bitwise complement. A successful round-trip proves we can both
	 *     write AND read device registers, not just read. */
	live_w = 0xdeadbeef;
	iowrite32(live_w, edu->mmio + EDU_REG_LIVENESS);
	live_r = ioread32(edu->mmio + EDU_REG_LIVENESS);
	dev_info(&pdev->dev,
		 "qemu_edu: liveness wrote 0x%08x, read 0x%08x (~wrote=0x%08x) -> %s\n",
		 live_w, live_r, (u32)~live_w,
		 live_r == (u32)~live_w ? "OK" : "MISMATCH");

	dev_info(&pdev->dev, "qemu_edu: probe complete, BAR0 mapped at %p\n", edu->mmio);
	return 0;

	/* --- error unwinding: undo, in reverse, exactly what succeeded above --- */
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

	/* Tear down in the REVERSE order of probe's setup. */
	pci_iounmap(pdev, edu->mmio);   /* undo (d) */
	pci_release_region(pdev, 0);    /* undo (c) */
	pci_disable_device(pdev);       /* undo (b) */
	kfree(edu);                     /* undo (a) */

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
MODULE_DESCRIPTION("Educational driver for the QEMU edu PCI device — Ch2 MMIO");
