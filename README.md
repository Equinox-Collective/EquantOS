# EquantOS

```
architecture : x86_64
version      : 0.0.1-alpha
bootloader   : Limine v8
language     : C + x86_64 Assembly
libc         : musl 1.2.6
license      : GPL-2.0
```

Monolithic hobby OS written from zero. Framebuffer terminal, 
interactive shell, physical/virtual memory management, scheduler, 
VFS, FAT32/EXT2 drivers, NVMe, and Ring 3 userspace
with a musl libc port.

Boots via [Limine](https://github.com/limine-bootloader/limine) on BIOS and
UEFI. QEMU is the primary target. Has also run on real x86_64 hardware.

> **WARNING:** Early alpha. Run in a VM or on test hardware only.
> Do not store anything you care about on it.

---

## Status

Kernel version : `EquantOS 0.0.1 Alpha`

Current focus: We are entered LONG-TERM development so development is now not fully focused work but a continue of life.

### Done

- Boot: Limine base revision 3, BIOS/UEFI hybrid ISO, HHDM, framebuffer
- CPU: GDT, IDT, 8259 PIC, 100 Hz PIT, exception handling, FPU/SSE
- Memory: PMM, VMM, per-process address spaces, paging, 1 MiB kernel heap
- Processes: ELF64 loader, Ring 3 tasks, round-robin scheduler, FPU/SSE
  context save/restore, SysV initial stack, `syscall` and `int 0x80` entry
- Userspace: vendored musl 1.2.6 source, prebuilt static sysroot, `hello.elf`
  smoke test, `musltest.elf` syscall probe
- Filesystems: VFS, writable RAMFS, GPT/MBR discovery, FAT32, ext2 read/write
- Storage: PCI enumeration, NVMe namespace I/O, legacy ATA PIO
- Input/output: PS/2 keyboard, framebuffer terminal, COM1 serial log
- Shell: interactive diagnostic shell with ~25 commands
- USB: xHCI driver, USB HID input
- Installer: early-stage installer code present

### In progress

- GPU through LinuxKPI
- Own package manager (Including making network support)
- Sound implementation with PC Speaker, AC97 and continous.
- GUI

---

## Prerequisites

| Tool | Purpose |
|---|---|
| `x86_64-elf-gcc` | Main compiling |
| `nasm` | Assembly compiling |
| GNU Make | Build script |
| `xorriso` | ISO generation |
| Python 3 | Disk image generation |
| `qemu-system-x86_64` | Running the system |

### NOTE!

The repo includes musl headers and `sdk/sysroot/lib/libc.a` but the `*.o`
gitignore rule strips out the CRT startup objects. Before building userspace
you need these in `sdk/sysroot/lib/`:

```
crt1.o  crti.o  crtn.o
```

Rebuild the musl port into `sdk/sysroot/`.

---

## Commands

```sh
make             - Builds the EquantOS ISO image
make run         - Builds (if wasn't built before) and runs EquantOS inside QEMU
make debug       - Runs QEMU paused (-S) with GDB server enabled (-s)
make disks       - Generates test disk images
make clean       - Removes the build/ directory
make clean-all   - Removes build artifacts, disks, and bootloader cache
make V=1         - Enables verbose mode (prints actual GCC/LD commands)
```

> **CAUTION:** `clean-disks` and `clean-all` delete the disk images and any
> data written to them.

Creates two 64 MiB disk images if they do not exist, then launches QEMU:

| Image | Layout | Attached as |
|---|---|---|
| `disk_gpt_ext2.img` | GPT + ext2 | NVMe |
| `disk_mbr_fat32.img` | MBR + FAT32 | IDE |

Full QEMU command:

```sh
qemu-system-x86_64 \
  -m 512M -vga std -boot d \
  -cdrom build/equantos.iso \
  -device qemu-xhci,id=xhci \
  -device usb-kbd,bus=xhci.0 \
  -device usb-mouse,bus=xhci.0 \
  -drive file=disk_gpt_ext2.img,format=raw,if=none,id=nvme0 \
  -device nvme,drive=nvme0,serial=deadbeef \
  -drive file=disk_mbr_fat32.img,format=raw,if=none,id=fat0 \
  -device ide-hd,drive=fat0 \
  -serial stdio
```

At boot, the kernel scans the NVMe device only. The GPT/ext2 partition mounts
at `/ext2`. The IDE image exists for legacy driver development and is not
currently scanned. A FAT32 partition found on the NVMe device would mount
at `/disk`.

---

## Known issues

- Fresh clone is missing `crt1.o`, `crti.o`, `crtn.o` - userspace will not
  link until they are provided
- The IDE/FAT32 image is not scanned by the current boot path
- The 64 MiB FAT32 image geometry is rejected by the current FAT32 driver
  as too small for FAT32
- ext2: direct-block writes only, no indirect blocks, no delete, no rename,
  no clean unmount
- ELF loader, scheduler, FD model and syscall layer are development code,
  not hardened for multi-user use