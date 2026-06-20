// SPDX-License-Identifier: GPL-2.0
/*
 * qemu_edu.c — Chapter 5: DMA (the device moves whole buffers by itself).
 *
 * Until now every byte crossed the PCIe bus through a *register*: the CPU
 * issued one ioread32/iowrite32 per 4 bytes. That's fine for control, hopeless
 * for throughput. DMA (Direct Memory Access) inverts it: we hand the device a
 * *host RAM address* and a length, and the device's own engine reads/writes
 * that memory across the bus with no CPU involvement per byte. The CPU only
 * sets it up and waits for the completion interrupt.
 *
 * New machinery this chapter:
 *   - dma_set_mask_and_coherent() : tell the kernel how many address bits the
 *                                   device can drive (edu decodes only 28).
 *   - dma_alloc_coherent()        : allocate a buffer the device can reach,
 *                                   returning BOTH a CPU pointer (for us) and a
 *                                   dma_addr_t (the bus address we give the device).
 *   - program SRC/DST/COUNT/CMD   : 8-byte DMA registers; writing CMD triggers it.
 *   - completion via interrupt    : we reuse the EXACT IRQ path from Chapter 4 —
 *                                   the device raises an IRQ when the copy is done.
 *
 * Demo: put a string in host RAM, DMA it INTO the device's internal buffer,
 * wipe our copy, then DMA it BACK OUT into host RAM, and prove it survived the
 * round-trip — i.e. the bytes really lived inside the device and came home.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/bits.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>   /* dma_set_mask_and_coherent, dma_alloc_coherent */

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

#define EDU_REG_ID          0x00
#define EDU_REG_STATUS      0x20
#define EDU_REG_INT_STATUS  0x24   /* RO: which interrupt(s) are pending */
#define EDU_REG_INT_ACK     0x64   /* WO: write V -> clears those bits, lowers the IRQ */
#define EDU_REG_DMA_SRC     0x80   /* RW 8B: DMA source address      */
#define EDU_REG_DMA_DST     0x88   /* RW 8B: DMA destination address */
#define EDU_REG_DMA_COUNT   0x90   /* RW 8B: DMA byte count          */
#define EDU_REG_DMA_CMD     0x98   /* RW 8B: bit0 start, bit1 dir, bit2 irq-on-done */

#define EDU_ID_MAGIC        0x010000ed

/* DMA command register bits (from the datasheet). */
#define EDU_DMA_CMD_START   BIT(0)
#define EDU_DMA_CMD_TO_RAM  BIT(1)  /* 0 = host RAM -> device buffer; 1 = device buffer -> host RAM */
#define EDU_DMA_CMD_IRQ     BIT(2)  /* raise an interrupt when the transfer completes */

/* The edu device's internal scratch buffer lives at these *device-side*
 * addresses. DMA src/dst that fall in this range mean "the device buffer". */
#define EDU_DMA_BUF_BASE    0x40000
#define EDU_DMA_BUF_SIZE    0x1000   /* 4 KB */

/* The edu engine decodes only a 28-bit address space onto the host. We must
 * promise the kernel we won't hand it addresses wider than this. */
#define EDU_DMA_MASK_BITS   28

/* Bits the device sets in INT_STATUS to say *why* it interrupted. */
#define EDU_IRQ_DMA         0x00000100   /* a DMA transfer finished */

struct edu_drv {
	struct pci_dev    *pdev;
	void __iomem      *mmio;
	int                irq;
	struct completion  irq_done;
	u32                last_irq_status;

	/* DMA: a buffer the device can reach. dma_alloc_coherent gives us two
	 * views of the same memory:
	 *   dma_buf    — a CPU virtual pointer WE use to read/write the bytes.
	 *   dma_handle — the bus address the DEVICE uses to find that memory.
	 * They are NOT the same number (see README: bus vs physical vs virtual). */
	void              *dma_buf;
	dma_addr_t         dma_handle;
};

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

/*
 * Same interrupt handler as Chapter 4 — DMA completion arrives through the very
 * same path. Read why, ACK, record, wake the waiter, return. Runs in interrupt
 * context: must not sleep.
 */
static irqreturn_t edu_irq_handler(int irq, void *dev_id)
{
	struct edu_drv *edu = dev_id;
	u32 status;

	status = ioread32(edu->mmio + EDU_REG_INT_STATUS);
	if (status == 0)
		return IRQ_NONE;

	iowrite32(status, edu->mmio + EDU_REG_INT_ACK);   /* ACK / de-assert */

	edu->last_irq_status = status;
	complete(&edu->irq_done);

	return IRQ_HANDLED;
}

/*
 * Run one DMA transfer and block until the device's completion interrupt fires.
 *
 *   host_addr : the bus address (dma_handle) of our coherent buffer.
 *   dev_addr  : a device-side address inside [EDU_DMA_BUF_BASE, +SIZE).
 *   len       : byte count (<= EDU_DMA_BUF_SIZE).
 *   to_ram    : false = host RAM -> device buffer ; true = device buffer -> host RAM.
 *
 * Ordering matters: we program SRC, DST and COUNT first, then write CMD LAST,
 * because writing CMD with the START bit is what *triggers* the engine. The
 * MMIO accessors to the same device are ordered, so the address/count are
 * already latched by the time the trigger lands.
 */
static int edu_dma_xfer(struct edu_drv *edu, dma_addr_t host_addr,
			u32 dev_addr, u32 len, bool to_ram)
{
	long timeleft;
	u32 cmd = EDU_DMA_CMD_START | EDU_DMA_CMD_IRQ;

	if (to_ram)
		cmd |= EDU_DMA_CMD_TO_RAM;

	reinit_completion(&edu->irq_done);

	/* source/destination depend on direction. For each direction one side is
	 * host RAM (host_addr) and the other is the device's internal buffer. */
	if (to_ram) {
		writeq(dev_addr,  edu->mmio + EDU_REG_DMA_SRC);   /* from device buffer */
		writeq(host_addr, edu->mmio + EDU_REG_DMA_DST);   /* into host RAM       */
	} else {
		writeq(host_addr, edu->mmio + EDU_REG_DMA_SRC);   /* from host RAM       */
		writeq(dev_addr,  edu->mmio + EDU_REG_DMA_DST);   /* into device buffer  */
	}
	writeq(len, edu->mmio + EDU_REG_DMA_COUNT);

	writeq(cmd, edu->mmio + EDU_REG_DMA_CMD);             /* GO (the trigger)    */

	/* The engine runs on its own; the CPU sleeps until the IRQ wakes us. */
	timeleft = wait_for_completion_timeout(&edu->irq_done, msecs_to_jiffies(1000));
	if (timeleft == 0) {
		dev_warn(&edu->pdev->dev, "qemu_edu: DMA %s TIMED OUT\n",
			 to_ram ? "device->host" : "host->device");
		return -ETIMEDOUT;
	}
	return 0;
}

/*
 * The round-trip demo. Proves data physically moved into the device and back.
 */
static void edu_dma_roundtrip(struct edu_drv *edu)
{
	static const char msg[] =
		"Hello from host RAM, round-tripped through the edu device via DMA!";
	size_t len = sizeof(msg);   /* includes the trailing NUL */
	int err;

	/* 1. Put a known pattern in our DMA buffer (CPU writes via dma_buf). */
	memcpy(edu->dma_buf, msg, len);

	/* 2. Push it host RAM -> device internal buffer. */
	err = edu_dma_xfer(edu, edu->dma_handle, EDU_DMA_BUF_BASE, len, false);
	if (err)
		return;
	dev_info(&edu->pdev->dev,
		 "qemu_edu: DMA host->device done (INT_STATUS=0x%08x)\n",
		 edu->last_irq_status);

	/* 3. Wipe our copy, so if readback shows the message it MUST have come
	 *    from the device, not from leftover bytes in our buffer. */
	memset(edu->dma_buf, 0, len);

	/* 4. Pull it back device internal buffer -> host RAM. */
	err = edu_dma_xfer(edu, edu->dma_handle, EDU_DMA_BUF_BASE, len, true);
	if (err)
		return;
	dev_info(&edu->pdev->dev,
		 "qemu_edu: DMA device->host done (INT_STATUS=0x%08x)\n",
		 edu->last_irq_status);

	/* 5. Verify the round-trip. */
	if (memcmp(edu->dma_buf, msg, len) == 0)
		dev_info(&edu->pdev->dev,
			 "qemu_edu: DMA round-trip OK -> \"%s\"\n",
			 (char *)edu->dma_buf);
	else
		dev_err(&edu->pdev->dev, "qemu_edu: DMA round-trip MISMATCH\n");
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

	/* Promise the DMA layer we will only ever hand the device 28-bit
	 * addresses. This makes dma_alloc_coherent pick memory the device can
	 * actually reach. Must come before any DMA allocation. */
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(EDU_DMA_MASK_BITS));
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: no usable %d-bit DMA mask: %d\n",
			EDU_DMA_MASK_BITS, err);
		goto err_disable;
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

	/* Bus mastering: the DMA engine issues memory reads/writes on its own,
	 * so the device must be a bus master (same bit MSI needed in Ch4). */
	pci_set_master(pdev);

	nvec = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (nvec < 0) {
		dev_err(&pdev->dev, "qemu_edu: pci_alloc_irq_vectors(MSI) failed: %d\n", nvec);
		err = nvec;
		goto err_clear_master;
	}
	edu->irq = pci_irq_vector(pdev, 0);

	err = request_irq(edu->irq, edu_irq_handler, 0, "qemu_edu", edu);
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: request_irq(%d) failed: %d\n", edu->irq, err);
		goto err_free_vectors;
	}
	dev_info(&pdev->dev, "qemu_edu: MSI enabled, using IRQ %d\n", edu->irq);

	/* Allocate a DMA-coherent buffer: memory both the CPU and the device can
	 * see consistently (no manual cache flushing). dma_handle is the bus
	 * address we'll feed the device. */
	edu->dma_buf = dma_alloc_coherent(&pdev->dev, EDU_DMA_BUF_SIZE,
					  &edu->dma_handle, GFP_KERNEL);
	if (!edu->dma_buf) {
		dev_err(&pdev->dev, "qemu_edu: dma_alloc_coherent failed\n");
		err = -ENOMEM;
		goto err_free_irq;
	}
	dev_info(&pdev->dev,
		 "qemu_edu: DMA buffer: cpu=%p  bus=%pad  size=%u\n",
		 edu->dma_buf, &edu->dma_handle, EDU_DMA_BUF_SIZE);

	edu_dma_roundtrip(edu);

	dev_info(&pdev->dev, "qemu_edu: probe complete\n");
	return 0;

err_free_irq:
	free_irq(edu->irq, edu);
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

	/* Reverse order of probe. Free the DMA buffer, then quiesce interrupts
	 * (free_irq guarantees the handler isn't running) before unmapping the
	 * registers the handler touches. */
	dma_free_coherent(&pdev->dev, EDU_DMA_BUF_SIZE, edu->dma_buf, edu->dma_handle);
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
MODULE_DESCRIPTION("Educational driver for the QEMU edu PCI device — Ch5 DMA");
