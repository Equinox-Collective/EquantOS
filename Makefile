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

# Create the bootable ISO image using Limine files
build/equantos.iso: build/kernel.elf src/boot/limine.conf
	@mkdir -p build/iso/boot
	@mkdir -p build/iso/EFI/BOOT
	cp build/kernel.elf build/iso/boot/kernel.elf
	cp src/boot/limine.conf build/iso/limine.conf
	# Copy Limine stage binaries if available in root or toolchain path
	# (Make sure limine.sys, BOOTX64.EFI are placed in root or handled accordingly)
	@echo "ISO root prepared successfully at build/iso/"

clean:
	rm -rf build

.PHONY: all clean