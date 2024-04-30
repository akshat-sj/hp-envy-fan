KDIR ?= /lib/modules/$(shell uname -r)/build
.PHONY: all module clean
all: module
module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel W=1 modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel clean
