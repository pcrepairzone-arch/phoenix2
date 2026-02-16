cmake_minimum_required(VERSION 3.16)
project(phoenix C ASM)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# === Toolchain (MSYS2 path) ===
set(CMAKE_C_COMPILER "C:/msys64/mingw64/bin/aarch64-none-elf-gcc.exe")
set(CMAKE_ASM_COMPILER "C:/msys64/mingw64/bin/aarch64-none-elf-as.exe")
set(CMAKE_LINKER "C:/msys64/mingw64/bin/aarch64-none-elf-ld.exe")
set(CMAKE_OBJCOPY "C:/msys64/mingw64/bin/aarch64-none-elf-objcopy.exe")

set(CMAKE_C_FLAGS "-Wall -O2 -ffreestanding -mcpu=cortex-a72 -mgeneral-regs-only -nostdlib -fno-builtin")
set(CMAKE_ASM_FLAGS "-mcpu=cortex-a72")

# === All source files (including spinlock) ===
set(SOURCES
    kernel/boot.c
    kernel/kernel.c
    kernel/sched.c
    kernel/task.c
    kernel/signal.c
    kernel/mmu.c
    kernel/pipe.c
    kernel/select.c
    kernel/irq.c
    kernel/timer.c
    kernel/pci.c
    kernel/vfs.c
    kernel/filecore.c
    kernel/dl.c
    kernel/blockdriver.c
    kernel/spinlock.c          # ← Added here
	kernel/blockdriver.h
	
    drivers/nvme/nvme.c
    drivers/usb/usb_storage.c
    drivers/bluetooth/bluetooth.c
    drivers/gpu/gpu.c
    drivers/mmc/mmc.c

    net/tcpip.c
    net/socket.c
    net/ipv4.c
    net/ipv6.c
    net/tcp.c
    net/udp.c
    net/arp.c

    wimp/wimp.c
    wimp/window.c
    wimp/event.c
    wimp/menu.c

    apps/paint.c
    apps/netsurf.c
		kernel/blockdriver.h
)

add_executable(phoenix ${SOURCES})

# Linker script
set_target_properties(phoenix PROPERTIES
    LINK_FLAGS "-T ${CMAKE_SOURCE_DIR}/kernel/linker.ld -nostdlib -static"
)

# Generate final bootable image
add_custom_command(TARGET phoenix POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary phoenix phoenix64.img
    COMMENT "Creating bootable image: phoenix64.img"
)

# Include directories
target_include_directories(phoenix PRIVATE
    kernel
    drivers
    net
    wimp
)