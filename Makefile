# Toolchain definitions
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
ASM = nasm

CFLAGS = -Wall -Wextra -O2 -g -pipe -ffreestanding -fno-stack-protector -fno-pie -fno-pic -mno-red-zone -mcmodel=kernel
ASMFLAGS = -f elf64
LDFLAGS = -nostdlib -static -T linker.ld

# ==============================================================================
# Smart Environment & Shell Detection
# ==============================================================================
ifeq ($(OS),Windows_NT)
    # Если задана MSYSTEM — запуск из Git Bash / MSYS2
    ifneq ($(MSYSTEM),)
        USE_POSIX := 1
    else
        USE_POSIX := 0
        # Для родной консоли CMD принудительно задаем SHELL = cmd.exe
        SHELL := cmd.exe
    endif
else
    # Linux / macOS / WSL
    USE_POSIX := 1
endif

ifeq ($(USE_POSIX),1)
    # Команды для Linux / macOS / Git Bash
    MKDIR = mkdir -p "$1"
    RMDIR = rm -rf "$1"
    RM    = rm -f "$1"
    CP    = cp "$1" "$2"
else
    # Нативные команды для Windows CMD / PowerShell
    WINPATH = $(subst /,\,$(patsubst %/,%,$1))
    MKDIR   = if not exist "$(call WINPATH,$1)" mkdir "$(call WINPATH,$1)"
    RMDIR   = if exist "$(call WINPATH,$1)" rmdir /s /q "$(call WINPATH,$1)"
    RM      = if exist "$(call WINPATH,$1)" del /q /f "$(call WINPATH,$1)"
    CP      = copy /Y "$(call WINPATH,$1)" "$(call WINPATH,$2)" >nul
endif

# Чистый GNU Make рекурсивный поиск файлов (работает везде)
rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

# Динамическое добавление всех папок из src/ в пути инклюдов (-I)
INC_DIRS = $(sort $(dir $(call rwildcard,src,*)))
CFLAGS += $(addprefix -I, $(INC_DIRS))

# Рекурсивный поиск всех исходников
C_SOURCES   = $(call rwildcard,src,*.c)
ASM_SOURCES = $(call rwildcard,src,*.asm)
S_SOURCES   = $(call rwildcard,src,*.s)

# Преобразование путей к объектным файлам
C_OBJECTS   = $(patsubst src/%.c, build/obj/%.o, $(C_SOURCES))
ASM_OBJECTS = $(patsubst src/%.asm, build/obj/%.o, $(ASM_SOURCES))
S_OBJECTS   = $(patsubst src/%.s, build/obj/%.o, $(S_SOURCES))

ALL_OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS) $(S_OBJECTS)

# Главный таргет
all: build/equantos.iso

# Компиляция файлов C
build/obj/%.o: src/%.c
	@$(call MKDIR,$(dir $@))
	$(CC) $(CFLAGS) -c $< -o $@

# Компиляция файлов NASM (.asm)
build/obj/%.o: src/%.asm
	@$(call MKDIR,$(dir $@))
	$(ASM) $(ASMFLAGS) $< -o $@

# Компиляция файлов GAS / GCC assembly (.s)
build/obj/%.o: src/%.s
	@$(call MKDIR,$(dir $@))
	$(CC) $(CFLAGS) -c $< -o $@

# Линковка ядра ELF
build/kernel.elf: $(ALL_OBJECTS) linker.ld
	@$(call MKDIR,build)
	$(LD) $(LDFLAGS) $(ALL_OBJECTS) -o $@

# Скачивание бинарников Limine
limine-bios-cd.bin limine-bios.sys limine-uefi-cd.bin BOOTX64.EFI:
	@echo [BUILD] Fetching Limine bootloader binaries...
	git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1 _limine_bin
	@$(call CP,_limine_bin/limine-bios-cd.bin,limine-bios-cd.bin)
	@$(call CP,_limine_bin/limine-bios.sys,limine-bios.sys)
	@$(call CP,_limine_bin/limine-uefi-cd.bin,limine-uefi-cd.bin)
	@$(call CP,_limine_bin/BOOTX64.EFI,BOOTX64.EFI)
	@$(call RMDIR,_limine_bin)
	@echo [BUILD] Limine binaries ready.

# Сборка гибридного ISO образа
build/equantos.iso: build/kernel.elf limine.conf limine-bios-cd.bin limine-uefi-cd.bin
	@echo [BUILD] Preparing ISO root structure...
	@$(call RMDIR,build/iso)
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

run: build/equantos.iso
	qemu-system-x86_64 -cdrom build/equantos.iso -serial stdio -m 2G

clean:
	@$(call RMDIR,build)

clean-all: clean
	@$(call RM,limine-bios-cd.bin)
	@$(call RM,limine-bios.sys)
	@$(call RM,limine-uefi-cd.bin)
	@$(call RM,BOOTX64.EFI)

.PHONY: all run clean clean-all