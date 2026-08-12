# Toolchain definitions
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
ASM = nasm

# CRITICAL FIX: Added -mno-sse -mno-mmx -mno-sse2 to prevent GCC auto-vectorization crashes in Ring 0
CFLAGS = -Wall -Wextra -O2 -g -pipe -ffreestanding -fno-stack-protector \
         -fno-pie -fno-pic -mno-red-zone -mcmodel=kernel \
         -mno-sse -mno-mmx -mno-sse2

ASMFLAGS = -f elf64

# CRITICAL FIX: Added -z max-page-size=0x1000 for strict 4KB page alignment
LDFLAGS = -nostdlib -static -z max-page-size=0x1000 -T src/linker.ld

# ==============================================================================
# Smart Environment & Shell Detection
# ==============================================================================
ifeq ($(OS),Windows_NT)
    ifneq ($(MSYSTEM),)
        USE_POSIX := 1
    else
        USE_POSIX := 0
        SHELL := cmd.exe
    endif
else
    USE_POSIX := 1
endif

ifeq ($(USE_POSIX),1)
    MKDIR = mkdir -p "$1"
    RMDIR = rm -rf "$1"
    RM    = rm -f "$1"
    CP    = cp "$1" "$2"
    EXT2_MAKE = dd if=/dev/zero of=disk.img bs=1M count=32 && mkfs.ext2 -F disk.img
else
    WINPATH = $(subst /,\,$(patsubst %/,%,$1))
    MKDIR   = if not exist "$(call WINPATH,$1)" mkdir "$(call WINPATH,$1)"
    RMDIR   = if exist "$(call WINPATH,$1)" rmdir /s /q "$(call WINPATH,$1)"
    RM      = if exist "$(call WINPATH,$1)" del /q /f "$(call WINPATH,$1)"
    CP      = copy /Y "$(call WINPATH,$1)" "$(call WINPATH,$2)" >nul
    EXT2_MAKE = winmakeext2 --size:32MB --output:disk.img
endif

# Recursive wildcard file search
rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

# Dynamic include paths from src/
INC_DIRS = $(sort $(dir $(call rwildcard,src,*)))
CFLAGS += $(addprefix -I, $(INC_DIRS))

# Source files search (excluding userland sources from kernel linking)
C_SOURCES   = $(filter-out src/userland/%, $(call rwildcard,src,*.c))
ASM_SOURCES = $(call rwildcard,src,*.asm)
S_SOURCES   = $(call rwildcard,src,*.s)

# Object files mapping
C_OBJECTS   = $(patsubst src/%.c, build/obj/%.o, $(C_SOURCES))
ASM_OBJECTS = $(patsubst src/%.asm, build/obj/%.o, $(ASM_SOURCES))
S_OBJECTS   = $(patsubst src/%.s, build/obj/%.o, $(S_SOURCES))

ALL_OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS) $(S_OBJECTS)

# Main target
all: build/equantos.iso

# Compile C kernel files
build/obj/%.o: src/%.c
	@$(call MKDIR,$(dir $@))
	$(CC) $(CFLAGS) -c $< -o $@

# Compile NASM assembly files (.asm)
build/obj/%.o: src/%.asm
	@$(call MKDIR,$(dir $@))
	$(ASM) $(ASMFLAGS) $< -o $@

# Compile GAS assembly files (.s)
build/obj/%.o: src/%.s
	@$(call MKDIR,$(dir $@))
	$(CC) $(CFLAGS) -c $< -o $@

# Link Kernel ELF
build/kernel.elf: $(ALL_OBJECTS) src/linker.ld
	@$(call MKDIR,build)
	$(LD) $(LDFLAGS) $(ALL_OBJECTS) -o $@

# Build userland test program equantmemtest.elf and place it directly into build/iso/
build/iso/equantmemtest.elf: src/userland/equantmemtest.c
	@$(call MKDIR,build/obj/userland)
	@$(call MKDIR,build/iso)
	$(CC) -g -ffreestanding -fno-pie -fno-pic -nostdlib -c src/userland/equantmemtest.c -o build/obj/userland/equantmemtest.o
	$(LD) -Ttext 0x400000 build/obj/userland/equantmemtest.o -o build/iso/equantmemtest.elf

build/iso/font.psf: res/font.psf
	@$(call MKDIR,build/iso)
	@$(call CP,res/font.psf,build/iso/font.psf)

# Generate EXT2 disk image using winmakeext2 (Windows) or mkfs.ext2 (Linux)
disks:
	py create_disks.py

# Download Limine binaries
limine-bios-cd.bin limine-bios.sys limine-uefi-cd.bin BOOTX64.EFI:
	@echo [BUILD] Fetching Limine bootloader binaries...
	git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1 _limine_bin
	@$(call CP,_limine_bin/limine-bios-cd.bin,limine-bios-cd.bin)
	@$(call CP,_limine_bin/limine-bios.sys,limine-bios.sys)
	@$(call CP,_limine_bin/limine-uefi-cd.bin,limine-uefi-cd.bin)
	@$(call CP,_limine_bin/BOOTX64.EFI,BOOTX64.EFI)
	@$(call RMDIR,_limine_bin)
	@echo [BUILD] Limine binaries ready.

# Build Hybrid ISO image (depends on kernel, test elf, and bootloader files)
build/equantos.iso: build/kernel.elf build/iso/equantmemtest.elf build/iso/font.psf limine.conf limine-bios-cd.bin limine-uefi-cd.bin
	@echo [BUILD] Preparing ISO root structure...
	@$(call MKDIR,build/iso/boot)
	@$(call MKDIR,build/iso/EFI/BOOT)
	@$(call CP,build/kernel.elf,build/iso/boot/kernel.elf)
	@$(call CP,limine.conf,build/iso/limine.conf)
	@$(call CP,limine-bios-cd.bin,build/iso/boot/limine-bios-cd.bin)
	@$(call CP,limine-bios.sys,build/iso/boot/limine-bios.sys)
	@$(call CP,limine-uefi-cd.bin,build/iso/boot/limine-uefi-cd.bin)
	@$(call CP,BOOTX64.EFI,build/iso/EFI/BOOT/BOOTX64.EFI)
	@echo [BUILD] Generating ISO image with xorriso...
	xorriso -as mkisofs \
		-b boot/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		build/iso -o build/equantos.iso
	@echo [BUILD] Cleaning up Limine binaries from project root...
	@$(call RM,limine-bios-cd.bin)
	@$(call RM,limine-bios.sys)
	@$(call RM,limine-uefi-cd.bin)
	@$(call RM,BOOTX64.EFI)
	@echo [SUCCESS] EquantOS ISO created at build/equantos.iso!

# REAL NVMe EMULATION: Removed -hda disk.vhd to prevent ATA false-positives
run: build/equantos.iso disks
	qemu-system-x86_64 -boot d \
		-cdrom build/equantos.iso \
		-drive file=disk_gpt_ext2.img,format=raw,if=none,id=nvme0 \
		-device nvme,drive=nvme0,serial=deadbeef \
		-drive file=disk_mbr_fat32.img,format=raw,if=none,id=fat0 \
		-device ide-hd,drive=fat0 \
		-serial stdio

clean:
	@$(call RMDIR,build)
	@$(call RM,disk.img)

clean-all: clean
	@$(call RM,limine-bios-cd.bin)
	@$(call RM,limine-bios.sys)
	@$(call RM,limine-uefi-cd.bin)
	@$(call RM,BOOTX64.EFI)

.PHONY: all run clean clean-all