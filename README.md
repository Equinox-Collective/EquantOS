# EquantOS

> A small monolithic x86_64 operating system built from scratch for learning,
> experimentation, and low-level systems programming.

[![Architecture](https://img.shields.io/badge/architecture-x86__64-6f42c1)](#current-capabilities)
[![Bootloader](https://img.shields.io/badge/bootloader-Limine-2f81f7)](https://github.com/limine-bootloader/limine)
[![Language](https://img.shields.io/badge/language-C%20%2B%20Assembly-555555)](#project-layout)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue)](LICENSE.md)

EquantOS is an experimental hobby OS with its own kernel, memory managers,
framebuffer terminal, diagnostic shell, filesystem layer, hardware drivers,
preemptive task scheduler, and early userspace support. It boots through
[Limine](https://github.com/limine-bootloader/limine) on both legacy BIOS and
UEFI systems.

> [!WARNING]
> EquantOS is in early alpha development. It is intended for virtual machines
> and development hardware, not for production use or storing important data.

## Current capabilities

- **Boot:** Limine protocol, higher-half kernel, hybrid BIOS/UEFI ISO
- **CPU setup:** GDT, IDT, PIC, interrupts, timer, SSE/FPU context handling
- **Memory:** physical and virtual memory managers, paging, and a kernel heap
- **Processes:** ELF64 loader, Ring 3 tasks, round-robin scheduler, and a small
  `int 0x80` syscall interface
- **Filesystems:** VFS, writable in-memory RAMFS, and experimental read-only
  FAT32 support
- **Storage:** ATA PIO access and MBR partition discovery
- **Devices:** PS/2 keyboard, PCI enumeration, framebuffer output, and COM1
  serial logging
- **Diagnostics:** panic handler, interactive shell, heap tests, memory metrics,
  disk inspection, and a bundled userspace memory test

The bundled `equantmemtest.elf` program is loaded as a Limine boot module,
placed in RAMFS, and started by the kernel to exercise userspace memory and
scheduler behavior.

## Requirements

Make sure these tools are available in `PATH`:

| Tool | Purpose |
| --- | --- |
| `x86_64-elf-gcc` and `x86_64-elf-ld` | Freestanding cross-compiler and linker |
| `nasm` | x86_64 assembly |
| GNU Make | Build orchestration |
| `xorriso` | Bootable ISO generation |
| Git | Fetching Limine binaries on the first build |
| `qemu-system-x86_64` | Running EquantOS locally |

A dedicated `x86_64-elf` cross-toolchain is required; the host compiler is not
a drop-in replacement. Linux, WSL, MSYS2, and native Windows environments can
be used as long as the tools above are installed and discoverable.

## Build and run

Clone the repository and build the ISO:

```sh
git clone https://github.com/Equinox-Collective/EquantOS.git
cd EquantOS
make
```

The resulting boot image is written to `build/equantos.iso`. The first build
requires an internet connection because the Makefile downloads the Limine v8
binary release.

Run it in QEMU:

```sh
make run
```

This is equivalent to launching QEMU with the generated ISO, the included
`disk.vhd` drive, and COM1 redirected to the current terminal:

```sh
qemu-system-x86_64 -cdrom build/equantos.iso -hda disk.vhd -serial stdio
```

To rebuild from a clean tree:

```sh
make clean
make
```

`make clean-all` also removes any downloaded Limine bootloader artifacts left
in the project root.

## Shell commands

After boot, type `help` at the `EquantOS>` prompt to list the available
commands.

| Command | Description |
| --- | --- |
| `help` | List all shell commands |
| `clear` | Clear the framebuffer terminal |
| `echo <text>` | Print text |
| `uptime` | Show time elapsed since boot |
| `eqfetch` | Display the EquantOS system banner |
| `ver` | Show the kernel version |
| `ls` | List the current VFS directory |
| `pwd` | Print the current directory |
| `cd <path>` | Change the current directory |
| `cat <file>` | Print a file |
| `hexdump <file>` | Print raw file contents in hexadecimal |
| `writefile <name> <text>` | Create a text file in RAMFS |
| `run <elf>` | Load and execute an ELF64 program from VFS |
| `mem` | Show physical-memory and heap usage |
| `memstress` | Run the kernel heap allocation stress test |
| `heapdump` | Write the heap block map to the serial log |
| `sysinfo` | Show detailed memory metrics |
| `diskinfo` | Show ATA drive and MBR partition information |
| `pciscan` | Rescan PCI devices and write results to the serial log |
| `reboot` | Reboot the machine |
| `shutdown` | Power off the machine |
| `panic_test` | Deliberately trigger a kernel panic for diagnostics |

If the first MBR partition in `disk.vhd` is FAT32, EquantOS mounts it read-only
at `/disk`.

## Boot sequence

At a high level, the kernel:

1. Initializes serial output, CPU descriptor tables, interrupts, and the timer.
2. Reads the Limine memory map and initializes physical/virtual memory plus the
   kernel heap.
3. Starts tasking, the scheduler, and the syscall dispatcher.
4. Mounts RAMFS at `/` and imports Limine boot modules into it.
5. Probes PCI and ATA devices, scans MBR partitions, and attempts to mount FAT32.
6. Initializes the framebuffer terminal and PS/2 keyboard.
7. Loads the bundled userspace test and enters the interactive shell.

## Project layout

```text
.
├── src/
│   ├── main.c                 # Kernel entry point and initialization
│   ├── linker.ld              # Kernel linker script
│   ├── equterm/               # Framebuffer terminal and shell
│   ├── kernel/
│   │   ├── core/              # CPU setup, interrupts, panic, PMM/VMM/heap
│   │   ├── drivers/           # ATA, keyboard, PCI, serial, display assets
│   │   ├── fs/                # VFS, RAMFS, MBR, and FAT32
│   │   ├── misc/              # Timer and power control
│   │   └── proc/              # Tasks, scheduler, ELF loader, and syscalls
│   ├── libs/                  # Freestanding string and stdio routines
│   └── userland/              # Bundled userspace test programs
├── DOCS/TODO.md               # Development roadmap
├── limine.conf                # Limine boot configuration
├── disk.vhd                   # Development disk image attached by make run
└── Makefile                   # Build, run, and cleanup targets
```

## Roadmap

The main planned areas are:

- GPT and NVMe support
- ext2/ext3/ext4 filesystems
- a broader Ring 3 userspace and SDK
- POSIX-compatible libc/syscalls, BusyBox, and a full shell
- USB support and an installer
- longer term: package management, graphics drivers, and a GUI

See [`DOCS/TODO.md`](DOCS/TODO.md) for the working checklist.

## Contributing

EquantOS is a learning-focused project, so focused bug reports, documentation
improvements, and small, reviewable pull requests are welcome. When changing a
kernel subsystem, include a short description of how you tested it in QEMU and
attach the relevant serial output when possible.

## License

EquantOS is distributed under the [GNU General Public License v2.0](LICENSE.md).
