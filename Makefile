obj-m += bc250_vrm.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(CURDIR) LLVM=1 modules

clean:
	make -C $(KDIR) M=$(CURDIR) clean

reload: all
	-sudo rmmod bc250_vrm 2>/dev/null
	sudo insmod bc250_vrm.ko
