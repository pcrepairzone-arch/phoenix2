# Makefile for RISC OS Phoenix
# Supports Raspberry Pi 4 and Pi 5
#
# Usage:
#   make BOARD=pi4        # Build for Raspberry Pi 4 (default)
#   make BOARD=pi5        # Build for Raspberry Pi 5
#   make                  # Defaults to pi4

CC       = aarch64-linux-gnu-gcc
AS       = aarch64-linux-gnu-as
LD       = aarch64-linux-gnu-ld
OBJCOPY  = aarch64-linux-gnu-objcopy

# ── Board selection ──────────────────────────────────────────────────────────
BOARD ?= pi4

ifeq ($(BOARD),pi5)
    CPU      = cortex-a76
    BOARD_ID = 5
    $(info Building for Raspberry Pi 5 (cortex-a76))
else ifeq ($(BOARD),pi4)
    CPU      = cortex-a72
    BOARD_ID = 4
    $(info Building for Raspberry Pi 4 (cortex-a72))
else
    $(error Unknown BOARD=$(BOARD). Use BOARD=pi4 or BOARD=pi5)
endif

# ── Flags ────────────────────────────────────────────────────────────────────
CFLAGS  = -Wall -O2 -ffreestanding -mcpu=$(CPU) -mgeneral-regs-only \
          -nostdlib -fno-builtin -Ikernel -I. -Idrivers -Inet -Iwimp \
          -DPI_MODEL=$(BOARD_ID)

ASFLAGS = -mcpu=$(CPU)

# Use GCC to assemble .S so we get CPP and ldr x0,=symbol support
%.o: %.S
	$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

LDFLAGS = -T kernel/linker.ld -nostdlib -static

# ── Object files ─────────────────────────────────────────────────────────────
OBJS = \
    kernel/boot.o \
    kernel/exceptions.o \
    kernel/kernel.o \
    kernel/malloc.o \
    kernel/errno.o \
    kernel/sched.o \
    kernel/task.o \
    kernel/signal.o \
    kernel/mmu.o \
    kernel/pipe.o \
    kernel/select.o \
    kernel/irq.o \
    kernel/timer.o \
    kernel/pci.o \
    kernel/vfs.o \
    kernel/filecore.o \
    kernel/dl.o \
    kernel/blockdriver.o \
    kernel/spinlock.o \
    kernel/lib.o \
    kernel/devicetree.o \
    kernel/led_diag.o \
    kernel/periph_base.o \
    kernel/mmio.o \
    drivers/uart/uart.o \
    drivers/gpu/gpu.o \
    drivers/nvme/nvme.o \
    drivers/usb/vl805_init.o \
    drivers/usb/usb_storage.o \
    drivers/usb/usb_xhci.o \
    drivers/usb/usb_init.o \
    drivers/gpu/mailbox.o \
    drivers/gpu/mailbox_property.o \
    drivers/gpu/mailbox_test.o \
    drivers/gpu/audio_diag.o \
    drivers/gpu/framebuffer.o \
    drivers/gpu/font8x8.o \
    drivers/mmc/mmc.o \
    net/tcpip.o \
    net/socket.o \
    net/ipv4.o \
    net/ipv6.o \
    net/tcp.o \
    net/udp.o \
    net/arp.o \
    wimp/wimp.o \
    wimp/window.o \
    wimp/event.o \
    wimp/menu.o \
    apps/paint.o \
    apps/netsurf.o

TARGET = phoenix64.img

# ── Rules ────────────────────────────────────────────────────────────────────
all: $(TARGET)

$(TARGET): kernel.elf
	$(OBJCOPY) -O binary kernel.elf $(TARGET)
	@echo ""
	@echo "=== Build successful! ==="
	@echo "Board  : $(BOARD) (Pi $(BOARD_ID))"
	@echo "CPU    : $(CPU)"
	@echo "Output : $(TARGET)"

kernel.elf: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o kernel.elf

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o */*.o */*/*.o kernel.elf $(TARGET)

help:
	@echo "Usage:"
	@echo "  make BOARD=pi4   # Raspberry Pi 4 (default)"
	@echo "  make BOARD=pi5   # Raspberry Pi 5"
	@echo "  make clean       # Remove build artefacts"

.PHONY: all clean help
