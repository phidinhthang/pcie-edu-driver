# Makefile — builds qemu_edu.ko as an out-of-tree kernel module.
#
# Run this INSIDE THE GUEST (guest$ make), where the matching kernel headers
# live. It does NOT compile the module itself — it hands the job to the
# kernel's own build system ("kbuild") so the module is built exactly the way
# the running kernel was.

# obj-m = "build these as loadable modules (.ko)". kbuild sees qemu_edu.o and
# links it into qemu_edu.ko. The name here fixes the module name.
obj-m += qemu_edu.o

# KDIR = the kernel "build" tree to compile against. /lib/modules/$(uname -r)/build
# is a symlink to the headers for the CURRENTLY RUNNING kernel — which is what a
# module must match. Overridable: `make KDIR=/path/to/other/kernel`.
KDIR ?= /lib/modules/$(shell uname -r)/build

# PWD = this directory (where our source lives). kbuild needs it via M=.
PWD := $(shell pwd)

# Default target. `-C $(KDIR)` switches into the kernel build tree; `M=$(PWD)`
# tells it "the module source is over here"; `modules` is the kbuild target.
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Remove all generated build artifacts (.ko, .o, .mod*, etc.).
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
