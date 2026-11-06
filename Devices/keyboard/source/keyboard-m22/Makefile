obj-m += w202_zsk.o

KERNEL_SOURCE ?= /root/debian-devices/sources/w202ds-linux-6.18.35-display-m12
KERNEL_OUTPUT ?= /root/debian-devices/work/w202ds/mainline-6.18.35-display-m12
CROSS_COMPILE ?= aarch64-linux-gnu-

.PHONY: all clean
all:
	$(MAKE) -C $(KERNEL_SOURCE) O=$(KERNEL_OUTPUT) M=$(CURDIR) ARCH=arm64 CROSS_COMPILE=$(CROSS_COMPILE) W=1 modules
	$(CROSS_COMPILE)gcc-14 -static -O2 -Wall -Wextra -Werror -o uart-capture uart-capture.c
	$(CROSS_COMPILE)gcc-14 -static -O2 -Wall -Wextra -Werror -o keyboard-attach keyboard-attach.c
	gcc -O2 -Wall -Wextra -Werror -o protocol-test protocol-test.c
	./protocol-test

clean:
	$(MAKE) -C $(KERNEL_SOURCE) O=$(KERNEL_OUTPUT) M=$(CURDIR) ARCH=arm64 CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -f uart-capture keyboard-attach protocol-test
