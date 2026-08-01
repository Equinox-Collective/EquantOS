CC = x86_64-elf-gcc
LD = x86_64-elf-ld

CFLAGS = -Wall -Wextra -O2 -pipe -ffreestanding -fno-stack-protector -fno-pie -fno-pic -mno-red-zone -mcmodel=kernel
LDFLAGS = -nostdlib -static -T linker.ld

# Automatically find all C source files inside src/
C_SOURCES = $(shell find src -name "*.c")
# Map source files to object files in build/obj/
OBJ_FILES = $(patsubst src/%.c, build/obj/%.o, $(C_SOURCES))

# Default target: build the bootable ISO image
all: build/equantos.iso

# Compile C source files into object files, maintaining directory hierarchy
build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Link object files into the final kernel ELF binary
build/kernel.elf: $(OBJ_FILES) linker.ld
	@mkdir -p build
	$(LD) $(LDFLAGS) $(OBJ_FILES) -o $@

# Automatically fetch Limine bootloader binaries if they are missing
limine-bios-cd.bin limine-bios.sys limine-uefi-cd.bin BOOTX64.EFI:
	@echo "[BUILD] Limine binaries not found. Automatically downloading via git..."
	@git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1 _limine_bin || \
	 git clone https://github.com/limine-bootloader/limine.git --depth=1 _limine_bin
	@cp _limine_bin/limine-bios-cd.bin ./
	@cp _limine_bin/limine-bios.sys ./
	@cp _limine_bin/limine-uefi-cd.bin ./
	@cp _limine_bin/BOOTX64.EFI ./
	@rm -rf _limine_bin
	@echo "[BUILD] Limine binaries successfully acquired!"

# Create the bootable ISO image using Limine and xorriso
build/equantos.iso: build/kernel.elf limine.conf limine-bios-cd.bin limine-uefi-cd.bin
	@echo "[BUILD] Preparing ISO root directory structure..."
	@rm -rf build/iso
	@mkdir -p build/iso/boot
	@mkdir -p build/iso/EFI/BOOT

	# Copy kernel and configuration file
	cp build/kernel.elf build/iso/boot/kernel.elf
	cp limine.conf build/iso/limine.conf

	# Copy Limine BIOS and UEFI boot files into ISO root
	cp limine-bios-cd.bin build/iso/boot/
	cp limine-bios.sys build/iso/boot/
	cp limine-uefi-cd.bin build/iso/boot/
	cp BOOTX64.EFI build/iso/EFI/BOOT/

	@echo "[BUILD] Generating hybrid bootable ISO image with xorriso..."
	xorriso -as mkisofs \
		-b boot/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		build/iso -o build/equantos.iso

	@echo "[SUCCESS] EquantOS bootable ISO successfully created at build/equantos.iso!"

run: build/equantos.iso
	qemu-system-x86_64 -cdrom build/equantos.iso -serial stdio -m 2G

clean:
	rm -rf build

# Optional target to clean downloaded bootloader bins as well
clean-all: clean
	rm -f limine-bios-cd.bin limine-bios.sys limine-uefi-cd.bin BOOTX64.EFI

.PHONY: all run clean clean-all