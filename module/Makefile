# MigFlow kernel module (migflow.ko): ktrackd + kpebsd
# Builds against the running kernel (headers in /lib/modules/$(uname -r)/build).
KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m += migflow.o
migflow-objs := tracker.o sampler.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
