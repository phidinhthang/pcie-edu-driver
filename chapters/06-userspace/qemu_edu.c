// SPDX-License-Identifier: GPL-2.0
/*
 * qemu_edu.c — Chapter 6: a character device (drive the hardware from userspace).
 *
 * Chapters 2-5 lived entirely inside the kernel: probe() poked registers and ran
 * DMA on its own. A real driver exists to serve *userspace*. This chapter exposes
 * the device as /dev/qemu_edu, a 4 KB window onto the device's internal buffer:
 *
 *   write(2)  ->  copy user bytes into our DMA buffer  ->  DMA host -> device
 *   read(2)   ->  DMA device -> host  ->  copy bytes back to the user
 *
 * So `echo hi > /dev/qemu_edu` pushes "hi" through DMA into the device, and
 * `cat /dev/qemu_edu` pulls it back out. New machinery:
 *
 *   - alloc_chrdev_region()  : reserve a (major,minor) device number.
 *   - cdev_init / cdev_add    : register our file_operations for that number.
 *   - class_create/device_create : make udev create the /dev/qemu_edu node.
 *   - file_operations         : open/read/write/release — the syscall entry points.
 *   - copy_to_user/copy_from_user : the ONLY safe way to cross the user/kernel line.
 *   - a mutex                 : serialize access (one DMA engine, one buffer).
 *
 * The DMA engine and IRQ handler are unchanged from Chapter 5; read()/write()
 * just call into them on behalf of a user process.
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
#include <linux/dma-mapping.h>
#include <linux/cdev.h>          /* struct cdev, cdev_init, cdev_add        */
#include <linux/fs.h>            /* file_operations, alloc_chrdev_region    */
#include <linux/device.h>        /* class_create, device_create             */
#include <linux/mutex.h>         /* struct mutex                            */
#include <linux/uaccess.h>       /* copy_to_user, copy_from_user            */

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

#define EDU_REG_ID          0x00
#define EDU_REG_INT_STATUS  0x24
#define EDU_REG_INT_ACK     0x64
#define EDU_REG_DMA_SRC     0x80
#define EDU_REG_DMA_DST     0x88
#define EDU_REG_DMA_COUNT   0x90
#define EDU_REG_DMA_CMD     0x98

#define EDU_ID_MAGIC        0x010000ed

#define EDU_DMA_CMD_START   BIT(0)
#define EDU_DMA_CMD_TO_RAM  BIT(1)
#define EDU_DMA_CMD_IRQ     BIT(2)

#define EDU_DMA_BUF_BASE    0x40000
#define EDU_DMA_BUF_SIZE    0x1000   /* 4 KB */
#define EDU_DMA_MASK_BITS   28
#define EDU_IRQ_DMA         0x00000100

struct edu_drv {
	struct pci_dev    *pdev;
	void __iomem      *mmio;
	int                irq;
	struct completion  irq_done;
	u32                last_irq_status;

	void              *dma_buf;     /* CPU view of the coherent buffer */
	dma_addr_t         dma_handle;  /* device (bus) view of the same   */

	/* Character-device plumbing. */
	dev_t              devt;        /* allocated (major,minor)         */
	struct cdev        cdev;        /* ties devt to our file_operations*/
	struct class      *class;       /* so udev makes /dev/qemu_edu     */
	struct mutex       lock;        /* one DMA engine -> serialize I/O */
	size_t             data_len;    /* #bytes last written (read bound)*/
};

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

/* ---- interrupt + DMA engine: unchanged from Chapter 5 -------------------- */

static irqreturn_t edu_irq_handler(int irq, void *dev_id)
{
	struct edu_drv *edu = dev_id;
	u32 status;

	status = ioread32(edu->mmio + EDU_REG_INT_STATUS);
	if (status == 0)
		return IRQ_NONE;

	iowrite32(status, edu->mmio + EDU_REG_INT_ACK);
	edu->last_irq_status = status;
	complete(&edu->irq_done);

	return IRQ_HANDLED;
}

static int edu_dma_xfer(struct edu_drv *edu, dma_addr_t host_addr,
			u32 dev_addr, u32 len, bool to_ram)
{
	long timeleft;
	u32 cmd = EDU_DMA_CMD_START | EDU_DMA_CMD_IRQ;

	if (to_ram)
		cmd |= EDU_DMA_CMD_TO_RAM;

	reinit_completion(&edu->irq_done);

	if (to_ram) {
		writeq(dev_addr,  edu->mmio + EDU_REG_DMA_SRC);
		writeq(host_addr, edu->mmio + EDU_REG_DMA_DST);
	} else {
		writeq(host_addr, edu->mmio + EDU_REG_DMA_SRC);
		writeq(dev_addr,  edu->mmio + EDU_REG_DMA_DST);
	}
	writeq(len, edu->mmio + EDU_REG_DMA_COUNT);
	writeq(cmd, edu->mmio + EDU_REG_DMA_CMD);            /* trigger */

	timeleft = wait_for_completion_timeout(&edu->irq_done, msecs_to_jiffies(1000));
	if (timeleft == 0) {
		dev_warn(&edu->pdev->dev, "qemu_edu: DMA %s TIMED OUT\n",
			 to_ram ? "device->host" : "host->device");
		return -ETIMEDOUT;
	}
	return 0;
}

/* ---- character device: the userspace face of the driver ------------------ */

/*
 * open(): the kernel hands us the inode; we recover our struct edu_drv from the
 * embedded cdev with container_of, and stash it in file->private_data so read()
 * and write() can find it without any global variable. (This is also how one
 * driver instance per device works cleanly — each cdev is inside its own edu.)
 */
static int edu_open(struct inode *inode, struct file *file)
{
	struct edu_drv *edu = container_of(inode->i_cdev, struct edu_drv, cdev);

	file->private_data = edu;
	return 0;
}

static int edu_release(struct inode *inode, struct file *file)
{
	return 0;
}

/*
 * write(): take up to 4 KB from the user, DMA it INTO the device buffer.
 *
 * copy_from_user is mandatory: the user pointer lives in another address space
 * and may be invalid or swapped out; a plain memcpy would be a security hole and
 * could oops. It returns the number of bytes it COULD NOT copy (0 == success).
 *
 * Model: each write() replaces the device buffer's contents (it does not append).
 * We remember how many bytes we wrote so read() knows where the data ends.
 */
static ssize_t edu_write(struct file *file, const char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	struct edu_drv *edu = file->private_data;
	ssize_t ret;
	int err;

	if (count == 0)
		return 0;
	if (count > EDU_DMA_BUF_SIZE)
		count = EDU_DMA_BUF_SIZE;            /* clamp to the device buffer */

	if (mutex_lock_interruptible(&edu->lock))
		return -ERESTARTSYS;

	if (copy_from_user(edu->dma_buf, ubuf, count)) {
		ret = -EFAULT;
		goto out;
	}

	err = edu_dma_xfer(edu, edu->dma_handle, EDU_DMA_BUF_BASE, count, false);
	if (err) {
		ret = err;
		goto out;
	}

	edu->data_len = count;
	ret = count;
	dev_info(&edu->pdev->dev, "qemu_edu: write %zd bytes -> DMA host->device\n", ret);
out:
	mutex_unlock(&edu->lock);
	return ret;
}

/*
 * read(): DMA the device buffer back to host RAM, then copy it to the user.
 *
 * We use *ppos so tools like `cat` terminate: the first call (offset 0) refreshes
 * the buffer from the device and returns the data; once the reader has consumed
 * data_len bytes we return 0 (EOF). copy_to_user is the read-side twin of
 * copy_from_user, with the same safety contract.
 */
static ssize_t edu_read(struct file *file, char __user *ubuf,
			size_t count, loff_t *ppos)
{
	struct edu_drv *edu = file->private_data;
	ssize_t ret;
	int err;

	if (mutex_lock_interruptible(&edu->lock))
		return -ERESTARTSYS;

	/* Fetch the device's current buffer once, at the start of a read sequence. */
	if (*ppos == 0 && edu->data_len > 0) {
		err = edu_dma_xfer(edu, edu->dma_handle, EDU_DMA_BUF_BASE,
				   edu->data_len, true);
		if (err) {
			ret = err;
			goto out;
		}
		dev_info(&edu->pdev->dev,
			 "qemu_edu: read -> DMA device->host %zu bytes\n",
			 edu->data_len);
	}

	if (*ppos >= edu->data_len) {        /* nothing (more) to give -> EOF */
		ret = 0;
		goto out;
	}

	if (count > edu->data_len - *ppos)
		count = edu->data_len - *ppos;

	if (copy_to_user(ubuf, edu->dma_buf + *ppos, count)) {
		ret = -EFAULT;
		goto out;
	}

	*ppos += count;
	ret = count;
out:
	mutex_unlock(&edu->lock);
	return ret;
}

static const struct file_operations edu_fops = {
	.owner   = THIS_MODULE,    /* refcount: blocks rmmod while /dev is open */
	.open    = edu_open,
	.release = edu_release,
	.read    = edu_read,
	.write   = edu_write,
	.llseek  = default_llseek,
};

/* ---- probe / remove ------------------------------------------------------ */

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
	mutex_init(&edu->lock);

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: pci_enable_device failed: %d\n", err);
		goto err_free;
	}

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

	edu->dma_buf = dma_alloc_coherent(&pdev->dev, EDU_DMA_BUF_SIZE,
					  &edu->dma_handle, GFP_KERNEL);
	if (!edu->dma_buf) {
		dev_err(&pdev->dev, "qemu_edu: dma_alloc_coherent failed\n");
		err = -ENOMEM;
		goto err_free_irq;
	}

	/* --- expose the device to userspace as /dev/qemu_edu --- */

	/* 1. reserve a dynamic (major,minor); 1 minor. */
	err = alloc_chrdev_region(&edu->devt, 0, 1, "qemu_edu");
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: alloc_chrdev_region failed: %d\n", err);
		goto err_free_dma;
	}

	/* 2. bind our file_operations to that device number. After cdev_add the
	 *    device is LIVE: a matching open() can arrive immediately, so every
	 *    resource read()/write() touch must already be initialized. */
	cdev_init(&edu->cdev, &edu_fops);
	edu->cdev.owner = THIS_MODULE;
	err = cdev_add(&edu->cdev, edu->devt, 1);
	if (err) {
		dev_err(&pdev->dev, "qemu_edu: cdev_add failed: %d\n", err);
		goto err_unregister_chrdev;
	}

	/* 3. create a class + device so udev materializes /dev/qemu_edu for us. */
	edu->class = class_create("qemu_edu");
	if (IS_ERR(edu->class)) {
		err = PTR_ERR(edu->class);
		dev_err(&pdev->dev, "qemu_edu: class_create failed: %d\n", err);
		goto err_cdev_del;
	}
	if (IS_ERR(device_create(edu->class, &pdev->dev, edu->devt, NULL, "qemu_edu"))) {
		err = PTR_ERR(edu->class);   /* class is fine; device_create failed */
		dev_err(&pdev->dev, "qemu_edu: device_create failed\n");
		goto err_class_destroy;
	}

	dev_info(&pdev->dev, "qemu_edu: /dev/qemu_edu ready (major %d, minor %d)\n",
		 MAJOR(edu->devt), MINOR(edu->devt));
	dev_info(&pdev->dev, "qemu_edu: probe complete\n");
	return 0;

err_class_destroy:
	class_destroy(edu->class);
err_cdev_del:
	cdev_del(&edu->cdev);
err_unregister_chrdev:
	unregister_chrdev_region(edu->devt, 1);
err_free_dma:
	dma_free_coherent(&pdev->dev, EDU_DMA_BUF_SIZE, edu->dma_buf, edu->dma_handle);
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

	/* Tear down the userspace entry points FIRST so no new open/read/write can
	 * start, then release hardware resources (reverse order of probe). The
	 * .owner refcount already prevented rmmod while a fd was open. */
	device_destroy(edu->class, edu->devt);
	class_destroy(edu->class);
	cdev_del(&edu->cdev);
	unregister_chrdev_region(edu->devt, 1);

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
MODULE_DESCRIPTION("Educational driver for the QEMU edu PCI device — Ch6 char device / userspace");
