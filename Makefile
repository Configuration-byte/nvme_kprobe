# Makefile for io_delay_kprobe kernel module

obj-m := io_delay_kprobe.o

# Kernel build directory
KDIR ?= /work/work/git_code/linux-6.6/

# Source directory (current directory)
PWD := $(shell pwd)

# Extra compiler flags
ccflags-y := -Wall -O2

# Kernel module build target
all:
	@echo "Building io_delay_kprobe module..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean build artifacts
clean:
	@echo "Cleaning io_delay_kprobe build..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Install module (requires root)
install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

# Reload module (for development)
reload: clean all
	@sudo rmmod io_delay_kprobe 2>/dev/null || true
	@sudo insmod io_delay_kprobe.ko

# Quick rebuild and load with parameters
reload-with-delay:
	@sudo rmmod io_delay_kprobe 2>/dev/null || true
	@sudo insmod io_delay_kprobe.ko io_delay_ns=10000000

# Debug - show kernel source info
dbg:
	@echo "KDIR: $(KDIR)"
	@echo "PWD: $(PWD)"
	@echo "Kernel: $(shell uname -r)"
	@ls -la $(KDIR) 2>/dev/null || echo "KDIR not found"

.PHONY: all clean install reload reload-with-delay dbg