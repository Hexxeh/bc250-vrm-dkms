obj-m += bc250_vrm.o
bc250_vrm-y := bc250_vrm_core.o nct6687_smbus.o

KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_CC := $(shell grep -qs '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config && echo clang)
ifeq ($(KERNEL_CC),clang)
LLVM_FLAGS := LLVM=1
endif

.PHONY: all clean reload

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(LLVM_FLAGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(LLVM_FLAGS) clean

reload:
	$(error 복원 확인 없이 재적재하지 않습니다. README의 진단 계약을 확인하세요)
