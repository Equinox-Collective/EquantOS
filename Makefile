CC = x86_64-elf-gcc
LD = x86_64-elf-ld
ASM = nasm

CFLAGS = -Wall -Wextra -O2 -pipe -ffreestanding -fno-stack-protector -fno-pie -fno-pic -mno-red-zone -mcmodel=kernel
ASMFLAGS = -f elf64
LDFLAGS = -nostdlib -static -T linker.ld

# Automatically include root src/ and every subdirectory as an include path (-I)
INC_DIRS = $(shell find src -type d)
CFLAGS += $(addprefix -I, $(INC_DIRS))

# Recursively find all C and Assembly source files across the entire src/ tree
C_SOURCES = $(shell find src -name "*.c")
ASM_SOURCES = $(shell find src -name "*.asm" -o -name "*.s")

# Map sources to object files inside build/obj/ maintaining directory structure
C_OBJECTS = $(patsubst src/%.c, build/obj/%.o, $(C_SOURCES))
ASM_OBJECTS = $(patsubst src/%.asm, build/obj/%.o, $(patsubst src/%.s, build/obj/%.o, $(ASM_SOURCES)))
ALL_OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS)

# Default target: build the bootable ISO image
all: build/equantos.iso

# Compile C source files
build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile NASM assembly files (.asm)
build/obj/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

# Compile GAS assembly files (.s)
build/obj/%.o: src/%.s
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

# Link all object files into kernel ELF binary
build/kernel.elf: $(ALL_OBJECTS) linker.ld
	@mkdir -p build
	$(LD) $(LDFLAGS) $(ALL_OBJECTS) -o $@

# Automatically fetch Limine bootloader binaries if missing
limine-bios-cd.bin limine-bios.sys limine-uefi-cd.bin BOOTX64.EFI:
	@echo "[BUILD] Downloading Limine bootloader binaries..."
	@git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1 _limine_bin || \
	 git clone https://github.com/limine-bootloader/limine.git --depth=1 _limine_bin
	@cp _limine_bin/limine-bios-cd.bin ./
	@cp _limine_bin/limine-bios.sys ./
	@cp _limine_bin/limine-uefi-cd.bin ./
	@cp _limine_bin/BOOTX64.EFI ./
	@rm -rf _limine_bin
	@echo "[BUILD] Limine binaries ready."

# Build the hybrid bootable ISO image
build/equantos.iso: build/kernel.elf limine.conf limine-bios-cd.bin limine-uefi-cd.bin
	@echo "[BUILD] Preparing ISO root structure..."
	@rm -rf build/iso
	@mkdir -p build/iso/boot
	@mkdir -p build/iso/EFI/BOOT
	cp build/kernel.elf build/iso/boot/kernel.elf
	cp limine.conf build/iso/limine.conf
	cp limine-bios-cd.bin build/iso/boot/
	cp limine-bios.sys build/iso/boot/
	cp limine-uefi-cd.bin build/iso/boot/
	cp BOOTX64.EFI build/iso/EFI/BOOT/
	@echo "[BUILD] Generating ISO image with xorriso..."
	xorriso -as mkisofs \
		-b boot/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		build/iso -o build/equantos.iso
	@echo "[SUCCESS] EquantOS ISO created at build/equantos.iso!"

run: build/equantos.iso
	qemu-system-x86_64 -cdrom build/equantos.iso -serial stdio -m 2G

clean:
	rm -rf build

clean-all: clean
	rm -f limine-bios-cd.bin limine-bios.sys limine-uefi-cd.bin BOOTX64.EFI

.PHONY: all run clean clean-all