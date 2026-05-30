// SPDX-License-Identifier: GPL-2.0
/*
 * qemu_edu.c — Chapter 1: the smallest possible PCI driver (skeleton).
 *
 * Goal of this stage: register a pci_driver that matches the QEMU `edu`
 * device (vendor 0x1234, device 0x11e8). When the kernel enumerates a matching
 * device it calls our .probe; on module unload (or device removal) it calls
 * our .remove. Both only print to the kernel log.
 *
 * We deliberately DO NOT touch the hardware yet — not even read a register.
 * This stage proves three things at once:
 *   1. PCI enumeration found the device,
 *   2. the kernel matched it to OUR driver by vendor:device ID,
 *   3. our build -> insmod -> dmesg pipeline works end to end.
 * Everything else builds on this skeleton.
 */

#include <linux/module.h>   /* MODULE_*, the module_init/exit machinery       */
#include <linux/kernel.h>   /* printk / log levels                            */
#include <linux/pci.h>      /* struct pci_driver, pci_device_id, PCI_DEVICE() */

/* The IDs the edu device advertises on the PCI bus. We match on this pair. */
#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

/*
 * The table of (vendor, device) pairs this driver handles.
 *
 * PCI_DEVICE(v, d) fills in vendor=v, device=d and wildcards the rest
 * (subvendor / subdevice / class), i.e. "match any edu regardless of those."
 * The terminating { 0, } is a sentinel: the PCI core walks this array until it
 * hits an all-zero entry, so the table must always end with one.
 */
static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ 0, }
};

/*
 * Publishes the ID table into the module's metadata so userspace tooling
 * (depmod/udev) knows "this .ko handles 1234:11e8" and can auto-load it when
 * such a device appears. For our manual insmod it's not strictly required, but
 * it's standard and harmless — always include it.
 */
MODULE_DEVICE_TABLE(pci, edu_ids);

/*
 * .probe — the PCI core calls this ONCE for each matching device, after the
 * device has been enumerated and is ready to be claimed.
 *
 *   @pdev : our handle to this specific device (its BDF, config space, etc.).
 *   @id   : the entry from edu_ids[] that matched (handy if one driver handles
 *           several IDs and wants to branch on which one it got).
 *
 * Return value is the contract with the core:
 *   0            -> "I claim this device." The core binds us to it.
 *   negative err -> "not mine / setup failed." The core leaves the device
 *                   unbound and will try other drivers.
 */
static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	/*
	 * dev_info() is printk() that automatically prefixes the message with
	 * this device's identity (e.g. "qemu_edu 0000:00:03.0: ..."), so log
	 * lines are tied to the device. Prefer it over bare printk in drivers.
	 */
	dev_info(&pdev->dev, "qemu_edu: probe! claimed device %04x:%04x\n",
		 id->vendor, id->device);
	return 0;
}

/*
 * .remove — called when the device is removed or (our usual case) when the
 * module is unloaded with rmmod. This is where, in later chapters, we will
 * undo everything .probe set up (unmap BARs, free IRQs, free DMA buffers),
 * in reverse order. Right now there is nothing to undo.
 */
static void edu_remove(struct pci_dev *pdev)
{
	dev_info(&pdev->dev, "qemu_edu: remove\n");
}

/* Bind the callbacks and the ID table together under a driver name. */
static struct pci_driver edu_driver = {
	.name     = "qemu_edu",   /* shows up under /sys/bus/pci/drivers/qemu_edu */
	.id_table = edu_ids,
	.probe    = edu_probe,
	.remove   = edu_remove,
};

/*
 * Boilerplate macro that generates the module's init/exit functions:
 *   on insmod -> pci_register_driver(&edu_driver)
 *   on rmmod  -> pci_unregister_driver(&edu_driver)
 *
 * Registering does more than "remember this driver": the core immediately
 * scans devices already on the bus and calls our .probe for any that match.
 * That is why "probe!" appears the instant we insmod, with no hotplug needed.
 */
module_pci_driver(edu_driver);

MODULE_LICENSE("GPL");                 /* GPL: required to use many kernel symbols */
MODULE_AUTHOR("phidinhthang");
MODULE_DESCRIPTION("Educational driver for the QEMU edu PCI device — Ch1 skeleton");
