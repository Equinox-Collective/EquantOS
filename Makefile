# ==============================================================================
# EquantOS Master Makefile (Cross-Platform Windows CMD & POSIX)
# ==============================================================================

# Toolchain definitions
CC  := x86_64-elf-gcc
LD  := x86_64-elf-ld
ASM := nasm

# Verbosity Control (Run `make V=1` to see full compiler command lines)
ifeq ($(V),1)
    Q :=
else
    Q := @
endif

# Kernel Flags
CFLAGS := -Wall -Wextra -O2 -g -pipe -ffreestanding -fno-stack-protector \
          -fno-pie -fno-pic -mno-red-zone -mcmodel=kernel \
          -mno-sse -mno-mmx -mno-sse2 -MMD -MP

ASMFLAGS := -f elf64
LDFLAGS  := -nostdlib -static -z max-page-size=0x1000 -T src/linker.ld

# Userspace SDK Flags (Musl Libc Integration)
USER_LDFLAGS  := -static -nostdlib -z max-page-size=0x1000 -z noexecstack -Ttext-segment 0x400000
USER_CRT_PRE   := sdk/sysroot/lib/crt1.o sdk/sysroot/lib/crti.o
USER_CRT_POST  := sdk/sysroot/lib/libc.a sdk/sysroot/lib/crtn.o

# GUI (Equi) Specific Compiler Flags: Full SSE2 Enabled, High Optimization
UI_CFLAGS     := -static -nostdinc -isystem sdk/sysroot/include -Iuserspace/equi \
                 -O3 -msse2 -Wall -Wextra -fno-pie -fno-pic -MMD -MP

# QEMU Hardware Emulation Flags
QEMU      := qemu-system-x86_64
qemu-system-x86_64 -cdrom build/equantos.iso -m 512M -vga std -serial stdio -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0 -drive file=disk_gpt_ext2.img,format=raw,if=none,id=nvme0 -device nvme,drive=nvme0,serial=deadbeef -drive file=disk_mbr_fat32.img,format=raw,if=none,id=fat0 -device ide-hd,drive=fat0 -d int,cpu_reset,guest_errors -D qemu_debug.log -no-reboot

# Hard-Disk Boot Flags
QEMUBDFLAGS := -m 512M \
               -vga std \
               -boot c \
               -bios OVMF.fd \
               -device qemu-xhci,id=xhci \
               -device usb-kbd,bus=xhci.0 \
               -device usb-mouse,bus=xhci.0 \
               -drive file=disk_gpt_ext2.img,format=raw,if=none,id=hd0 \
               -device ide-hd,drive=hd0,bootindex=1 \
               -serial stdio \
               -d guest_errors,unimp -D qemu_bd.log

# ==============================================================================
# Environment & Shell Detection
# ==============================================================================
ifeq ($(OS),Windows_NT)
    ifneq ($(MSYSTEM),)
        USE_POSIX := 1
    else
        USE_POSIX := 0
        SHELL     := cmd.exe
    endif
else
    USE_POSIX := 1
endif

ifeq ($(USE_POSIX),1)
    MKDIR = mkdir -p "$1"
    RMDIR = rm -rf "$1"
    RM    = rm -f "$1"
    CP    = cp "$1" "$2"
    DEV_NULL := > /dev/null 2>&1
    
    # ANSI Color Palette
    CLR_RESET   := \033[0m
    CLR_CC      := \033[1;34m
    CLR_ASM     := \033[1;35m
    CLR_LD      := \033[1;32m
    CLR_ISO     := \033[1;33m
    CLR_OK      := \033[1;92m
    CLR_INFO    := \033[1;36m

    LOG_STEP = @printf "  %b  %s\n" "$1" "$2"
    LOG_MSG  = @printf "%b\n" "$1"
else
    WINPATH = $(subst /,\,$(patsubst %/,%,$1))
    MKDIR   = if not exist "$(call WINPATH,$1)" mkdir "$(call WINPATH,$1)"
    RMDIR   = if exist "$(call WINPATH,$1)" rmdir /s /q "$(call WINPATH,$1)"
    RM      = if exist "$(call WINPATH,$1)" del /q /f "$(call WINPATH,$1)"
    CP      = copy /Y "$(call WINPATH,$1)" "$(call WINPATH,$2)" >nul
    DEV_NULL := > NUL 2>&1

    CLR_RESET   :=
    CLR_CC      := [CC]  
    CLR_ASM     := [ASM] 
    CLR_LD      := [LD]  
    CLR_ISO     := [ISO] 
    CLR_OK      := [SUCCESS]
    CLR_INFO    := [INFO]

    LOG_STEP = @echo   $1 $2
    LOG_MSG  = @echo $1
endif

# Recursive wildcard function
rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

# Include directories for Kernel
INC_DIRS := $(sort $(dir $(call rwildcard,src,*)))
CFLAGS   += $(addprefix -I, $(INC_DIRS))

# Kernel Sources and Objects
C_SOURCES   := $(call rwildcard,src,*.c)
ASM_SOURCES := $(call rwildcard,src,*.asm)
S_SOURCES   := $(call rwildcard,src,*.s)

C_OBJECTS   := $(patsubst src/%.c, build/obj/%.o, $(C_SOURCES))
ASM_OBJECTS := $(patsubst src/%.asm, build/obj/%.o, $(ASM_SOURCES))
S_OBJECTS   := $(patsubst src/%.s, build/obj/%.o, $(S_SOURCES))

ALL_OBJECTS := $(C_OBJECTS) $(ASM_OBJECTS) $(S_OBJECTS)
DEP_FILES   := $(ALL_OBJECTS:.o=.d)

# GUI Server (Equi) Multi-File Sources and Objects
UI_DIR      := userspace/equi
UI_SOURCES  := $(call rwildcard,$(UI_DIR),*.c)
UI_OBJECTS  := $(patsubst $(UI_DIR)/%.c, build/obj/ui/%.o, $(UI_SOURCES))
UI_DEPS     := $(UI_OBJECTS:.o=.d)

# Userspace Target List (Integrated into ISO)
ALL_USERSPACE := build/iso/hello.elf \
                 build/iso/musltest.elf \
                 build/iso/equantmemtest.elf \
                 build/iso/busybox.elf \
                 build/iso/font.psf \
                 build/iso/.bashrc \
                 build/iso/bash.elf \
                 build/iso/equi.elf \
				 build/iso/kdiag.elf

# ==============================================================================
# Master Targets
# ==============================================================================

.PHONY: all ui run runbd debug disks clean clean-disks clean-all help

all: build/equantos.iso

# Compile Master UI Binary Standalone
ui: build/iso/equi.elf
	$(call LOG_MSG,$(CLR_OK) Userspace GUI Compositor successfully built: build/iso/equi.elf)

# Compile Kernel C Sources
build/obj/%.o: src/%.c
	@$(call MKDIR,$(dir $@))
	$(call LOG_STEP,$(CLR_CC),$<)
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

# Compile Kernel NASM Sources
build/obj/%.o: src/%.asm
	@$(call MKDIR,$(dir $@))
	$(call LOG_STEP,$(CLR_ASM),$<)
	$(Q)$(ASM) $(ASMFLAGS) $< -o $@

# Compile Kernel GAS Sources
build/obj/%.o: src/%.s
	@$(call MKDIR,$(dir $@))
	$(call LOG_STEP,$(CLR_CC),$<)
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

# Link Kernel ELF
build/kernel.elf: $(ALL_OBJECTS) src/linker.ld
	@$(call MKDIR,build)
	$(call LOG_STEP,$(CLR_LD),$@)
	$(Q)$(LD) $(LDFLAGS) $(ALL_OBJECTS) -o $@

# ==============================================================================
# GUI Compositor (Equi) Compilation Rules
# ==============================================================================

build/obj/ui/%.o: $(UI_DIR)/%.c
	@$(call MKDIR,$(dir $@))
	$(call LOG_STEP,$(CLR_CC),$<)
	$(Q)$(CC) $(UI_CFLAGS) -c $< -o $@

build/iso/equi.elf: $(UI_OBJECTS)
	@$(call MKDIR,build/iso)
	$(call LOG_STEP,$(CLR_LD),$@)
	$(Q)$(LD) $(USER_LDFLAGS) $(USER_CRT_PRE) $(UI_OBJECTS) $(USER_CRT_POST) -o $@

# ==============================================================================
# Generic Userspace Applications
# ==============================================================================

build/iso/equantmemtest.elf: userspace/equantmemtest.c
	@$(call MKDIR,build/obj/userspace)
	@$(call MKDIR,build/iso)
	$(call LOG_STEP,$(CLR_CC),$<)
	$(Q)$(CC) -g -ffreestanding -fno-pie -fno-pic -nostdlib -c $< -o build/obj/userspace/equantmemtest.o
	$(Q)$(LD) -Ttext 0x400000 build/obj/userspace/equantmemtest.o -o $@

build/iso/%.elf: userspace/%.c
	@$(call MKDIR,build/obj/userspace)
	@$(call MKDIR,build/iso)
	$(call LOG_STEP,$(CLR_CC),$<)
	$(Q)$(CC) -static -nostdinc -isystem sdk/sysroot/include -fno-pie -fno-pic -c $< -o build/obj/userspace/$*.o
	$(Q)$(LD) $(USER_LDFLAGS) $(USER_CRT_PRE) build/obj/userspace/$*.o $(USER_CRT_POST) -o $@

build/iso/busybox.elf: res/busybox.elf
	@$(call MKDIR,build/iso)
	$(call LOG_STEP,$(CLR_INFO),$< -> $@)
	$(Q)$(call CP,res/busybox.elf,$@)

build/iso/font.psf: res/font.psf
	@$(call MKDIR,build/iso)
	$(call LOG_STEP,$(CLR_INFO),$< -> $@)
	$(Q)$(call CP,res/font.psf,$@)

build/iso/bash.elf: res/bash.elf
	@$(call MKDIR,build/iso)
	$(call LOG_STEP,$(CLR_INFO),$< -> $@)
	$(Q)$(call CP,res/bash.elf,$@)
	
build/iso/.bashrc: res/.bashrc
	@$(call MKDIR,build/iso)
	$(call LOG_STEP,$(CLR_INFO),$< -> $@)
	$(Q)$(call CP,res/.bashrc,$@)

# Auto-Dependency Inclusion
-include $(DEP_FILES)
-include $(UI_DEPS)

# ==============================================================================
# Bootable ISO & Disks Construction
# ==============================================================================

disks:
	$(call LOG_MSG,  $(CLR_INFO) Generating raw test disk images...)
	$(Q)python create_disks.py

limine-bios-cd.bin limine-bios.sys limine-uefi-cd.bin BOOTX64.EFI:
	$(call LOG_MSG,  $(CLR_INFO) Downloading Limine Bootloader binaries...)
	$(Q)curl -Lo limine-binary.tar.gz https://github.com/limine-bootloader/limine/releases/latest/download/limine-binary.tar.gz
	$(Q)tar -xzf limine-binary.tar.gz
	$(Q)$(call CP,limine-binary/limine-bios-cd.bin,limine-bios-cd.bin)
	$(Q)$(call CP,limine-binary/limine-bios.sys,limine-bios.sys)
	$(Q)$(call CP,limine-binary/limine-uefi-cd.bin,limine-uefi-cd.bin)
	$(Q)$(call CP,limine-binary/BOOTX64.EFI,BOOTX64.EFI)
	$(Q)$(call RMDIR,limine-binary)
	$(Q)$(call RM,limine-binary.tar.gz)

build/equantos.iso: build/kernel.elf $(ALL_USERSPACE) limine.conf limine-bios-cd.bin limine-uefi-cd.bin
	$(call LOG_MSG,  $(CLR_ISO) Constructing bootable ISO image...)
	@$(call MKDIR,build/iso/boot)
	@$(call MKDIR,build/iso/EFI/BOOT)
	$(Q)$(call CP,build/kernel.elf,build/iso/boot/kernel.elf)
	$(Q)$(call CP,limine.conf,build/iso/limine.conf)
	$(Q)$(call CP,limine-bios-cd.bin,build/iso/boot/limine-bios-cd.bin)
	$(Q)$(call CP,limine-bios.sys,build/iso/boot/limine-bios.sys)
	$(Q)$(call CP,limine-uefi-cd.bin,build/iso/boot/limine-uefi-cd.bin)
	$(Q)$(call CP,BOOTX64.EFI,build/iso/EFI/BOOT/BOOTX64.EFI)
	$(Q)xorriso -as mkisofs \
		-b boot/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		build/iso -o build/equantos.iso $(DEV_NULL)
	$(Q)$(call RM,limine-bios-cd.bin)
	$(Q)$(call RM,limine-bios.sys)
	$(Q)$(call RM,limine-uefi-cd.bin)
	$(Q)$(call RM,BOOTX64.EFI)
	$(call LOG_MSG,$(CLR_OK) EquantOS ISO successfully built at build/equantos.iso)

# ==============================================================================
# Emulation & Debug
# ==============================================================================

run: build/equantos.iso disks
	$(call LOG_MSG,  $(CLR_INFO) Launching EquantOS in QEMU...)
	$(Q)$(QEMU) -cdrom build/equantos.iso $(QEMUFLAGS)

runbd:
	$(call LOG_MSG,  $(CLR_INFO) Launching EquantOS from Hard Disk (disk_gpt_ext2.img)...)
	$(Q)$(QEMU) $(QEMUBDFLAGS)

debug: build/equantos.iso disks
	$(call LOG_MSG,  $(CLR_INFO) Launching EquantOS in debug mode (GDB server on port :1234)...)
	$(Q)$(QEMU) -cdrom build/equantos.iso $(QEMUFLAGS) -s -S

# ==============================================================================
# Clean
# ==============================================================================

clean:
	$(call LOG_MSG,  $(CLR_INFO) Removing build artifacts...)
	$(Q)$(call RMDIR,build)

clean-disks:
	$(call LOG_MSG,  $(CLR_INFO) Removing virtual disks...)
	$(Q)$(call RM,disk_gpt_ext2.img)
	$(Q)$(call RM,disk_mbr_fat32.img)

clean-all: clean clean-disks
	$(Q)$(call RM,limine-bios-cd.bin)
	$(Q)$(call RM,limine-bios.sys)
	$(Q)$(call RM,limine-uefi-cd.bin)
	$(Q)$(call RM,BOOTX64.EFI)
	$(Q)$(call RM,limine-binary.tar.gz)
	$(Q)$(call RMDIR,limine-binary)

help:
	@echo EquantOS Build System:
	@echo   make             - Builds full ISO image
	@echo   make ui          - Compiles standalone GUI compositor (equi.elf)
	@echo   make run         - Runs ISO in QEMU with NVMe and XHCI
	@echo   make clean       - Cleans build directory