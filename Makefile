# --- Compiler Configuration ---
CC = x86_64-elf-gcc
LD = x86_64-elf-ld

CFLAGS = -Wall -Wextra -O2 -pipe -fno-pic -mno-red-zone -mcmodel=kernel \
         -ffreestanding -fno-stack-protector -fno-omit-frame-pointer \
         -I./include -m64 -march=x86-64

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld

# --- Files ---
CFILES = $(wildcard kernel/core/*.c) $(wildcard drivers/*.c)
OBJ = $(CFILES:.c=.o)

KERNEL = bin/kernel.elf
ISO = bin/EquantOS.iso

.PHONY: all clean run setup

all: setup $(ISO)

# Используем Windows-совместимый вариант игнорирования ошибок создания папок
setup:
	-@mkdir bin 2>NUL
	-@mkdir iso_root 2>NUL

# Compile C files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link the kernel
$(KERNEL): $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@

# Build the bootable ISO
$(ISO): $(KERNEL)
	-@rm -rf iso_root/* 2>NUL
	cp $(KERNEL) iso_root/
	cp limine.cfg iso_root/
	cp limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin iso_root/
	
	xorriso -as mkisofs -b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO)
	
	./limine.exe bios-install $(ISO)

clean:
	-@rm -f $(OBJ)
	-@rm -rf bin iso_root

run: all
	qemu-system-x86_64 -m 2G -M q35 -cdrom $(ISO) -serial stdio -boot d