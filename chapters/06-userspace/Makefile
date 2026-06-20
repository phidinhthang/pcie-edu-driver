# Makefile — builds qemu_edu.ko (Chapter 6). Same pattern as earlier chapters;
# see chapters/01-skeleton/Makefile for the line-by-line explanation.
#
# Build INSIDE THE GUEST:  guest$ make

obj-m += qemu_edu.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
